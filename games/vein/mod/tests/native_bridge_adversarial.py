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
    def __init__(self, cert, key, gate=False, env_overrides=None):
        self.peer = Peer(cert, key)
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
        assert self.lines.get(timeout=5) == 'READY'
        self.index = self.identify()
        self.pending = []

    def read_lines(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def identify(self):
        index = self.peer.connections.get(timeout=5)
        while True:
            i, opcode, data = self.peer.frames.get(timeout=5)
            if i == index and opcode == 1 and json.loads(data)['type'] == 'identify':
                self.identify_payload = json.loads(data)['payload']
                assert self.identify_payload['identityToken'] == 'test-identity'
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
        stderr = self.proc.stderr.read()
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
        overload(cert, key, False)
        overload(cert, key, True)
        completion_limit(cert, key)
        location_window(cert, key)
        stale_epoch(cert, key)


if __name__ == '__main__':
    main()
