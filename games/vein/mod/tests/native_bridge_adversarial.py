#!/usr/bin/env python3
"""Run real native bridge and LWS workers against a local TLS WebSocket peer."""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

BINARY = Path(__file__).resolve().parent / 'build/native_bridge_adversarial'


def exact(sock, count):
    out = b''
    while len(out) < count:
        chunk = sock.recv(count - len(out))
        if not chunk:
            raise EOFError()
        out += chunk
    return out


def eventually(check, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.1)
    raise AssertionError('condition not reached')


def send_frame(sock, opcode, data):
    length = len(data)
    header = bytes([0x80 | opcode])
    if length < 126:
        header += bytes([length])
    elif length <= 65535:
        header += b'\x7e' + struct.pack('!H', length)
    else:
        header += b'\x7f' + struct.pack('!Q', length)
    sock.sendall(header + data)


class Peer:
    def __init__(self, cert, key):
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(cert, key)
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.frames = queue.Queue()
        self.connections = queue.Queue()
        self.sockets = []
        self.stopped = threading.Event()
        self.errors = []
        self.acceptor = threading.Thread(target=self.accept_loop, daemon=True)
        self.acceptor.start()

    def accept_loop(self):
        while not self.stopped.is_set():
            try:
                raw, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self.session, args=(raw,), daemon=True).start()

    def session(self, raw):
        sock = None
        try:
            raw.settimeout(10)
            sock = self.context.wrap_socket(raw, server_side=True)
            headers = b''
            while b'\r\n\r\n' not in headers:
                headers += exact(sock, 1)
            fields = dict(line.split(':', 1) for line in headers.decode('ascii').split('\r\n')[1:] if ':' in line)
            fields = {key.lower(): value.strip() for key, value in fields.items()}
            accept = base64.b64encode(hashlib.sha1((fields['sec-websocket-key'] +
                '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            reply = b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept
            sock.sendall(reply + b'\r\n\r\n')
            index = len(self.sockets)
            self.sockets.append(sock)
            self.connections.put(index)
            while not self.stopped.is_set():
                first, second = exact(sock, 2)
                size = second & 127
                if size == 126:
                    size = struct.unpack('!H', exact(sock, 2))[0]
                elif size == 127:
                    size = struct.unpack('!Q', exact(sock, 8))[0]
                if size > 10 * 1024 * 1024:
                    raise AssertionError(f'unexpected client frame {size}')
                mask = exact(sock, 4) if second & 128 else b''
                payload = exact(sock, size)
                if mask:
                    payload = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
                opcode = first & 15
                if opcode == 9:
                    send_frame(sock, 10, payload)
                elif opcode == 8:
                    break
                else:
                    self.frames.put((index, opcode, payload))
        except (EOFError, OSError, ssl.SSLError, socket.timeout):
            pass
        except Exception as error:
            self.errors.append(str(error))
        finally:
            if sock:
                sock.close()
            raw.close()

    def send(self, index, value, opcode=1):
        data = value if isinstance(value, bytes) else json.dumps(value, separators=(',', ':')).encode()
        send_frame(self.sockets[index], opcode, data)

    def close_session(self, index):
        self.sockets[index].shutdown(socket.SHUT_RDWR)
        self.sockets[index].close()

    def close(self):
        self.stopped.set()
        self.listener.close()
        for sock in self.sockets:
            try:
                sock.close()
            except OSError:
                pass
        self.acceptor.join(timeout=2)


class Run:
    def __init__(self, cert, key, gate=False, env_overrides=None, reject_first=None, identify_now=True,
                 expected_identity='test-identity'):
        self.peer = Peer(cert, key)
        self.reject_first = reject_first
        self.expected_identity = expected_identity
        argv = [str(BINARY), f'wss://localhost:{self.peer.port}/', str(cert)]
        if gate:
            argv.append('gate')
        child_env = os.environ.copy()
        child_env.update({'TAKARO_SERVER_NAME': 'NativeBridge test', 'VEIN_HTTP_API': ''})
        child_env.update(env_overrides or {})
        self.proc = subprocess.Popen(argv,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, bufsize=1, env=child_env)
        self.lines = queue.Queue()
        threading.Thread(target=self.read_lines, daemon=True).start()
        self._err = []
        self._err_thread = threading.Thread(target=lambda: self._err.extend(self.proc.stderr), daemon=True)
        self._err_thread.start()
        assert self.lines.get(timeout=5) == 'READY'
        self.pending = []
        self.index = self.identify() if identify_now else None

    def stderr_so_far(self):
        return ''.join(self._err)

    def read_lines(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def identify(self):
        index = self.peer.connections.get(timeout=5)
        while True:
            i, opcode, data = self.peer.frames.get(timeout=5)
            if i == index and opcode == 1 and json.loads(data)['type'] == 'identify':
                self.identify_payload = json.loads(data)['payload']
                expected = getattr(self, 'expected_identity', 'test-identity')
                if expected is not None:
                    assert self.identify_payload['identityToken'] == expected, self.identify_payload
                if getattr(self, 'reject_first', None):
                    self.peer.send(index, {'type': 'identifyResponse',
                                           'payload': {'error': {'message': self.reject_first}}})
                    self.reject_first = None
                    index = self.peer.connections.get(timeout=10)
                    continue
                self.peer.send(index, {'type': 'identifyResponse', 'payload': {}})
                return index

    def request(self, ident, action, args=None, index=None):
        self.peer.send(self.index if index is None else index,
                       {'type': 'request', 'requestId': ident, 'payload': {'action': action, 'args': args}})

    def response(self, ident, timeout=5, index=None):
        target = self.index if index is None else index
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for pos, entry in enumerate(self.pending):
                i, opcode, data = entry
                if i == target and opcode == 1:
                    message = json.loads(data)
                    if message.get('requestId') == ident:
                        self.pending.pop(pos)
                        return message
            try:
                entry = self.peer.frames.get(timeout=min(0.2, end - time.monotonic()))
            except queue.Empty:
                continue
            self.pending.append(entry)
        raise AssertionError(f'timed out waiting for {ident}')

    def no_response(self, ident, seconds=0.3):
        try:
            self.response(ident, timeout=seconds)
        except AssertionError:
            return
        raise AssertionError(f'unexpected response for {ident}')

    def command(self, value, prefix):
        self.proc.stdin.write(value + '\n')
        self.proc.stdin.flush()
        line = self.lines.get(timeout=5)
        assert line.startswith(prefix), line
        return line[len(prefix):]

    def health(self):
        return json.loads(self.command('health', 'HEALTH '))

    def close(self):
        if self.proc.poll() is None:
            try:
                self.command('release', 'RELEASED')
                self.proc.stdin.write('quit\n')
                self.proc.stdin.flush()
                self.proc.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired, AssertionError):
                self.proc.kill()
                self.proc.wait(timeout=3)
        if hasattr(self, '_err_thread'):
            self._err_thread.join(timeout=5)
            stderr = ''.join(self._err)
        else:
            stderr = self.proc.stderr.read()
        self.stderr = stderr
        self.peer.close()
        assert self.proc.returncode == 0, stderr
        assert not self.peer.errors, self.peer.errors


def basic(cert, key):
    run = Run(cert, key)
    try:
        # Malformed frames are dropped; malformed requests with IDs get explicit errors.
        run.peer.send(run.index, b'{bad')
        run.peer.send(run.index, {'type': 'request', 'payload': {'action': 'echo', 'args': {}}})
        run.no_response('missing-id')
        run.peer.send(run.index, {'type': 'request', 'requestId': 'missing-action', 'payload': {}})
        assert 'error' in run.response('missing-action')
        for ident, args, expected in [('null', None, {}), ('array', [], {}),
                                      ('string', '{"x":3}', {'x': 3}), ('invalid-string', '{bad', {})]:
            run.request(ident, 'echo', args)
            response = run.response(ident)
            assert response.get('payload') == expected, (ident, response)
        run.request('depth64', 'echo', '[' * 64 + '0' + ']' * 64)
        assert run.response('depth64')['payload'] == {}
        run.request('depth65', 'echo', '[' * 65 + '0' + ']' * 65)
        assert 'depth' in run.response('depth65')['error']
        # A deeply nested outer frame is discarded before request dispatch; the connection
        # continues to serve the next valid request.
        outer = ('{"type":"request","requestId":"outer-deep","payload":{"action":"echo",'
                 '"args":' + '[' * 65 + '0' + ']' * 65 + '}}').encode()
        run.peer.send(run.index, outer)
        run.no_response('outer-deep')
        run.request('after-deep', 'echo', {'still': 'alive'})
        assert run.response('after-deep')['payload'] == {'still': 'alive'}
        run.request('handler-error', 'throw', {})
        assert 'intentional handler failure' in run.response('handler-error')['error']
        run.request('bad-json', 'badjson', {})
        assert 'error' in run.response('bad-json')
        run.request('too-big', 'huge', {})
        assert '8 MiB' in run.response('too-big')['error']
        run.peer.send(run.index, {'type': 'error', 'payload': {'message': 'test-identitytest-identity invalid event'}})
        end = time.monotonic() + 5
        while run.health().get('protocolErrorCount', 0) == 0 and time.monotonic() < end:
            time.sleep(0.02)
        health = run.health()
        assert health['protocolErrorCount'] == 1, health
        assert health['lastError'] == 'Takaro protocol error: [redacted][redacted] invalid event', health
        print('bridge frames, args, depth, errors and 8 MiB response: pass', flush=True)
    finally:
        run.close()


def config_semantics(cert, key):
    paths = queue.Queue()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            paths.put(self.path)
            body = b'{"source":"real-game-http-fallback"}'
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server = threading.Thread(target=http.serve_forever, daemon=True)
    server.start()
    try:
        url = f'  http://127.0.0.1:{http.server_port}/api///  '
        run = Run(cert, key, env_overrides={'TAKARO_SERVER_NAME': '', 'VEIN_HTTP_API': url})
        try:
            assert run.identify_payload['name'] == 'Takaro Dev Vein', run.identify_payload
            run.request('game-http-config', 'fetchGamePlayers', {})
            assert run.response('game-http-config')['payload'] == {'source': 'real-game-http-fallback'}
            assert paths.get(timeout=2) == '/api/players'
            print('bridge legacy empty server name and trimmed game HTTP URL: pass', flush=True)
        finally:
            run.close()
    finally:
        http.shutdown()
        http.server_close()
        server.join(timeout=2)


def console_status(cert, key):
    # The panel console (stdout in the game, stderr here) names a refused token and the recovery,
    # once per state change, and never prints the token.
    secret = 'sekret-registration-value'
    run = Run(cert, key, env_overrides={'TAKARO_REGISTRATION_TOKEN': secret},
              reject_first=f'Invalid registration token {secret}')
    run.close()
    lines = [l for l in run.stderr.splitlines() if l.startswith('[Takaro] ')]
    assert secret not in run.stderr, run.stderr
    assert any(l.startswith('[Takaro] connecting to wss://localhost:') for l in lines), lines
    refused = [i for i, l in enumerate(lines) if 'Takaro refused this server' in l]
    assert len(refused) == 1, lines
    assert 'TAKARO_REGISTRATION_TOKEN environment variable' in lines[refused[0] + 1], lines
    assert lines[refused[0] - 1].startswith('[Takaro] ****'), lines
    assert sum('connected to Takaro as' in l for l in lines) == 1, lines
    assert not any('not set' in l for l in lines), lines
    # No token: a banner, and no connection attempt at all.
    run = Run(cert, key, env_overrides={'TAKARO_REGISTRATION_TOKEN': ''}, identify_now=False)
    time.sleep(1.5)
    assert run.peer.connections.empty(), 'connected without a registration token'
    run.close()
    assert '[Takaro]   TAKARO_REGISTRATION_TOKEN not set, the server is not connected to Takaro.' in run.stderr, run.stderr
    assert 'no restart needed' in run.stderr, run.stderr
    assert 'connecting to wss://' not in run.stderr, run.stderr
    print('bridge console status lines: pass', flush=True)


SHIPPED_CFG = Path(__file__).resolve().parents[2] / 'takaro.cfg'


def write_cfg(path, text):
    # The way an editor or panel saves: a new file renamed over the old one.
    tmp = path.with_suffix('.editing')
    tmp.write_text(text)
    os.replace(tmp, path)


def set_line(text, key, value):
    out = [f'{key}={value}' if line.split('=', 1)[0].strip() == key else line for line in text.splitlines()]
    return '\n'.join(out) + '\n'


def next_identify(run, timeout=10):
    index = run.peer.connections.get(timeout=timeout)
    while True:
        i, opcode, data = run.peer.frames.get(timeout=timeout)
        if i == index and opcode == 1 and json.loads(data)['type'] == 'identify':
            return index, json.loads(data)['payload']


def config_reload(cert, key):
    # takaro.cfg as shipped, on a fresh install: banner, no connection, an identity is generated
    # and written back; a token saved while running connects without a restart; a rejected token
    # is reported and a corrected one reconnects; a half-saved file is ignored; a replaced file
    # (upgrade) falls back to the saved copy; the environment still wins.
    with tempfile.TemporaryDirectory(prefix='vein-cfg-') as tmp:
        root = Path(tmp)
        cfg, data = root / 'takaro.cfg', root / 'data'
        saved = data / 'saved-settings.cfg'
        cfg.write_text(SHIPPED_CFG.read_text())
        env = {'TAKARO_CONFIG_FILE': str(cfg), 'TAKARO_PLUGIN_DATA_DIR': str(data),
               'TAKARO_TEST_CONFIG_POLL_MS': '400', 'TAKARO_SERVER_NAME': '', 'TAKARO_IDENTITY_TOKEN': '',
               'TAKARO_REGISTRATION_TOKEN': ''}
        run = Run(cert, key, env_overrides=env, identify_now=False, expected_identity=None)
        try:
            time.sleep(2)
            assert run.peer.connections.empty(), 'fresh install connected without a token'
            ident = [l for l in cfg.read_text().splitlines() if l.startswith('TAKARO_IDENTITY_TOKEN=')]
            assert len(ident) == 1, cfg.read_text()
            identity = ident[0].split('=', 1)[1]
            assert len(identity) == 36 and identity.count('-') == 4, identity
            assert f'TAKARO_IDENTITY_TOKEN={identity}' in saved.read_text()
            assert 'TAKARO_REGISTRATION_TOKEN=\n' in saved.read_text()
            assert run.health()['config']['identitySource'] == 'generated'

            write_cfg(cfg, set_line(cfg.read_text(), 'TAKARO_REGISTRATION_TOKEN', 'tok-first'))
            index, payload = next_identify(run)
            assert payload == {'identityToken': identity, 'registrationToken': 'tok-first',
                               'name': 'My VEIN server'}, payload
            run.peer.send(index, {'type': 'identifyResponse', 'payload': {}})
            eventually(lambda: 'TAKARO_REGISTRATION_TOKEN=tok-first' in saved.read_text())
            assert 'tok-first' not in cfg.read_text().replace('TAKARO_REGISTRATION_TOKEN=tok-first', '')

            write_cfg(cfg, set_line(cfg.read_text(), 'TAKARO_REGISTRATION_TOKEN', 'tok-wrong'))
            index, payload = next_identify(run)
            assert payload['registrationToken'] == 'tok-wrong', payload
            run.peer.send(index, {'type': 'identifyResponse',
                                  'payload': {'error': {'message': 'Invalid registrationToken provided'}}})
            eventually(lambda: 'Takaro refused this server' in run.stderr_so_far())
            write_cfg(cfg, set_line(cfg.read_text(), 'TAKARO_REGISTRATION_TOKEN', 'tok-second'))
            # The rejected connection is retried with backoff; the corrected token reconnects at once.
            while True:
                index, payload = next_identify(run)
                if payload['registrationToken'] == 'tok-second':
                    break
                run.peer.send(index, {'type': 'identifyResponse',
                                      'payload': {'error': {'message': 'Invalid registrationToken provided'}}})
            run.peer.send(index, {'type': 'identifyResponse', 'payload': {}})
            eventually(lambda: run.health()['connection']['identified'])
            eventually(lambda: 'TAKARO_REGISTRATION_TOKEN=tok-second' in saved.read_text())

            # Half-saved: a line without '=' keeps the running settings.
            write_cfg(cfg, cfg.read_text().replace('TAKARO_REGISTRATION_TOKEN=tok-second', 'TAKARO_REGISTRATION_TOK'))
            time.sleep(2)
            assert run.peer.connections.empty(), 'a half-saved file reconnected'
            assert run.health()['connection']['identified']
            # Saving the same identity and token again changes nothing.
            write_cfg(cfg, set_line(cfg.read_text().replace('TAKARO_REGISTRATION_TOK\n', ''),
                                    'TAKARO_REGISTRATION_TOKEN', 'tok-second'))
            time.sleep(2)
            assert run.peer.connections.empty(), 'an unchanged connection setting reconnected'
        finally:
            run.close()
        out = run.stderr
        assert 'TAKARO_REGISTRATION_TOKEN not set' in out and str(cfg) in out, out
        assert 'no restart needed' in out and 'restart the server' not in out, out
        refused = [l for l in out.splitlines() if 'Takaro refused this server' in l]
        assert len(refused) == 1, out
        assert any('line' in l and 'keeping the current settings' in l for l in out.splitlines()), out
        for secret in ('tok-first', 'tok-wrong', 'tok-second', identity):
            assert secret not in out, (secret, out)

        # Upgrade that replaced takaro.cfg with the shipped one: token and identity come from the
        # saved copy, and the identity is written back into takaro.cfg.
        cfg.write_text(SHIPPED_CFG.read_text())
        run = Run(cert, key, env_overrides=env, expected_identity=identity)
        try:
            assert run.identify_payload['registrationToken'] == 'tok-second', run.identify_payload
            assert f'TAKARO_IDENTITY_TOKEN={identity}' in cfg.read_text()
            assert 'tok-second' not in cfg.read_text()
            assert run.health()['config']['registrationSource'] == 'saved copy'
        finally:
            run.close()

        # The environment wins over the file, and a file change it overrides does not reconnect.
        env_run = dict(env, TAKARO_REGISTRATION_TOKEN='tok-env', TAKARO_IDENTITY_TOKEN='id-env')
        write_cfg(cfg, set_line(cfg.read_text(), 'TAKARO_REGISTRATION_TOKEN', 'tok-file'))
        run = Run(cert, key, env_overrides=env_run, expected_identity='id-env')
        try:
            assert run.identify_payload['registrationToken'] == 'tok-env', run.identify_payload
            write_cfg(cfg, set_line(cfg.read_text(), 'TAKARO_REGISTRATION_TOKEN', 'tok-file-2'))
            time.sleep(2)
            assert run.peer.connections.empty(), 'a file change reconnected although the environment wins'
        finally:
            run.close()
        assert f'TAKARO_IDENTITY_TOKEN={identity}' in cfg.read_text()

    # An install that ran an older connector (its data directory exists) and never set an
    # identity keeps the old default "vein".
    with tempfile.TemporaryDirectory(prefix='vein-cfg-legacy-') as tmp:
        root = Path(tmp)
        cfg, data = root / 'takaro.cfg', root / 'data'
        (data / 'connector-state').mkdir(parents=True)
        (data / 'plugin.log').write_text('old run\n')
        cfg.write_text('TAKARO_REGISTRATION_TOKEN=tok-legacy\n')
        env = {'TAKARO_CONFIG_FILE': str(cfg), 'TAKARO_PLUGIN_DATA_DIR': str(data),
               'TAKARO_IDENTITY_TOKEN': '', 'TAKARO_REGISTRATION_TOKEN': ''}
        run = Run(cert, key, env_overrides=env, expected_identity='vein')
        try:
            assert run.health()['config']['identitySource'] == 'legacy default'
        finally:
            run.close()
        assert 'TAKARO_IDENTITY_TOKEN=vein' in cfg.read_text()
        assert 'TAKARO_IDENTITY_TOKEN=vein' in (data / 'saved-settings.cfg').read_text()
    print('bridge takaro.cfg reload, banners, identity and saved copy: pass', flush=True)


def overload(cert, key, bytes_mode):
    run = Run(cert, key)
    try:
        run.request('hold', 'hold', {})
        # The blocked action worker must not block the LWS callback or the bridge worker.
        start = time.monotonic()
        run.peer.send(run.index, b'test-ping', opcode=9)
        while True:
            i, opcode, data = run.peer.frames.get(timeout=2)
            if i == run.index and opcode == 10 and data == b'test-ping':
                break
            run.pending.append((i, opcode, data))
        assert time.monotonic() - start < 1.0
        payload = {'blob': 'x' * (900 * 1024)} if bytes_mode else {'n': 1}
        count = 8 if bytes_mode else 135
        for n in range(count):
            run.request(f'q{n}', 'echo', payload)
        deadline = time.monotonic() + 5
        errors = []
        while time.monotonic() < deadline:
            try:
                i, opcode, data = run.peer.frames.get(timeout=0.2)
            except queue.Empty:
                continue
            if i == run.index and opcode == 1:
                message = json.loads(data)
                if 'overloaded' in message.get('error', ''):
                    errors.append(message)
                else:
                    run.pending.append((i, opcode, data))
        health = run.health()
        assert errors, f'no explicit overload: {health}'
        assert health['overloads'] > 0, health
        assert health['queues']['actions'] <= 128, health
        label = '4 MiB bytes' if bytes_mode else '128 actions'
        print(f'bridge {label} overload, explicit error and responsive callback: pass', flush=True)
    finally:
        run.close()


def stale_epoch(cert, key):
    run = Run(cert, key)
    try:
        old_index = run.index
        run.request('old', 'hold', {})
        end = time.monotonic() + 5
        while run.health()['queues']['actions'] != 0 and time.monotonic() < end:
            time.sleep(0.02)
        run.peer.close_session(old_index)
        run.index = run.identify()
        assert run.index != old_index
        run.command('release', 'RELEASED')
        run.no_response('old', 0.5)
        run.request('new', 'echo', {'ok': True})
        assert run.response('new')['payload'] == {'ok': True}
        print('bridge stale epoch response discarded after reconnect: pass', flush=True)
    finally:
        run.close()


def completion_limit(cert, key):
    run = Run(cert, key)
    try:
        run.command('pause', 'PAUSED')
        run.request('big-1', 'nearhuge', {})
        end = time.monotonic() + 5
        while run.health()['queues']['completionBytes'] < 7 * 1024 * 1024 and time.monotonic() < end:
            time.sleep(0.02)
        first_bytes = run.health()['queues']['completionBytes']
        assert first_bytes > 7 * 1024 * 1024, first_bytes
        run.request('big-2', 'nearhuge', {})
        end = time.monotonic() + 5
        while run.health()['requestCount'] < 2 and time.monotonic() < end:
            time.sleep(0.02)
        health = run.health()
        # Full native serializes action effects until the bridge applies the
        # previous completion. While paused, the second action remains queued.
        assert health['queues']['actions'] == 1, health
        assert health['queues']['completionBytes'] <= 16 * 1024 * 1024, health
        run.command('unpause', 'UNPAUSED')
        assert len(run.response('big-1', timeout=10)['payload']) == 7900 * 1024
        assert len(run.response('big-2', timeout=10)['payload']) == 7900 * 1024
        print('bridge completion queue bound and stateful-action serialization: pass', flush=True)
    finally:
        run.close()


def location_window(cert, key):
    run = Run(cert, key, gate=True)
    player = '76561198000000001'
    try:
        run.request('before-window', 'getPlayerLocation', {'gameId': player})
        assert 'location unavailable' in run.response('before-window')['error']
        run.command('window:' + player, 'WINDOW')
        run.request('recent-window', 'getPlayerLocation', {'player': {'gameId': player}})
        assert run.response('recent-window')['payload'] == {'x': 0, 'y': 0, 'z': 0}
        run.request('other-player', 'getPlayerLocation', {'gameId': '76561198000000002'})
        assert 'location unavailable' in run.response('other-player')['error']
        run.request('real-position', 'getPlayerLocation', {'platformId': 'steam:76561198000000003'})
        assert run.response('real-position')['payload'] == {'x': 1, 'y': 2, 'z': 3}
        print('bridge recent join location fallback is scoped to player: pass', flush=True)
    finally:
        run.close()


def main():
    with tempfile.TemporaryDirectory(prefix='vein-bridge-') as tmp:
        directory = Path(tmp)
        key, csr, cert = (directory / name for name in ('peer.key', 'peer.csr', 'peer.pem'))
        ext = directory / 'extensions.cnf'
        ext.write_text('subjectAltName=DNS:localhost\n')
        subprocess.run(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
                        '-subj', '/CN=localhost', '-keyout', str(key), '-out', str(csr)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(['openssl', 'x509', '-req', '-in', str(csr), '-signkey', str(key),
                        '-days', '1', '-extfile', str(ext), '-out', str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        basic(cert, key)
        config_semantics(cert, key)
        console_status(cert, key)
        config_reload(cert, key)
        overload(cert, key, False)
        overload(cert, key, True)
        completion_limit(cert, key)
        location_window(cert, key)
        stale_epoch(cert, key)


if __name__ == '__main__':
    main()
