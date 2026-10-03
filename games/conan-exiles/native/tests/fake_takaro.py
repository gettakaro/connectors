#!/usr/bin/env python3
"""A fake Takaro Generic connector endpoint: TLS + WebSocket, written VEIN style
(games/vein/mod/tests/native_transport_tls.py), with no Takaro credentials involved.

It speaks just enough of the real server to drive the native connector:
  - answers `identify` with `identifyResponse` (or an error payload when told to reject);
  - answers WebSocket pings with pongs carrying the same payload (switchable, to hold events
    unconfirmed), and records every application frame the connector sends;
  - sends `request` frames and Takaro's application `ping`;
  - can drop every connection and refuse new ones for a while (an outage like the 20 s one hosted
    Takaro had on 2026-10-03).
"""
import base64
import hashlib
import json
import os
import queue
import select
import socket
import ssl
import struct
import subprocess
import threading
import time
from pathlib import Path


def make_cert(directory: Path):
    key, cert = directory / 'key.pem', directory / 'cert.pem'
    ext = directory / 'ext.cnf'
    ext.write_text('subjectAltName=DNS:localhost\n')
    csr = directory / 'req.csr'
    subprocess.run(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
                    '-keyout', str(key), '-out', str(csr)], check=True, capture_output=True)
    subprocess.run(['openssl', 'x509', '-req', '-in', str(csr), '-signkey', str(key), '-days', '2',
                    '-extfile', str(ext), '-out', str(cert)], check=True, capture_output=True)
    return key, cert


def _exact(sock, n):
    out = b''
    while len(out) < n:
        part = sock.recv(n - len(out))
        if not part:
            raise EOFError()
        out += part
    return out


def _send(sock, lock, opcode, data):
    header = bytes([0x80 | opcode])
    if len(data) < 126:
        header += bytes([len(data)])
    elif len(data) <= 65535:
        header += b'\x7e' + struct.pack('!H', len(data))
    else:
        header += b'\x7f' + struct.pack('!Q', len(data))
    with lock:
        sock.sendall(header + data)


class Conn:
    """One connection. Only its session thread touches the TLS socket (an ssl.SSLSocket must not
    be read and written from two threads at once); other threads queue frames in `outq`."""

    def __init__(self, server, sock, index):
        self.server, self.sock, self.index = server, sock, index
        self.lock = threading.Lock()
        self.outq = queue.Queue()
        self.alive = True

    def send_json(self, value):
        self.outq.put((1, json.dumps(value).encode()))

    def send_text(self, text):
        self.outq.put((1, text.encode()))

    def flush(self):
        while True:
            try:
                opcode, data = self.outq.get_nowait()
            except queue.Empty:
                return
            _send(self.sock, self.lock, opcode, data)

    def close(self):
        self.alive = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class FakeTakaro:
    def __init__(self, key, cert, port=0):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(str(cert), str(key))
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', port))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(8)
        self.listener.settimeout(0.2)
        self.stop = threading.Event()
        self.down_until = 0.0
        self.pongs = True
        self.reject_identify = False
        self.answer_identify = True
        self.frames = queue.Queue()   # (conn index, parsed JSON or raw text)
        self.log = []                 # every (time, conn index, value) received
        self.conns = []
        self.identifies = 0
        self.ws_pings = 0
        self.errors = []
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    # ---- control -------------------------------------------------------------------------------
    def current(self, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            live = [c for c in self.conns if c.alive]
            if live:
                return live[-1]
            time.sleep(0.02)
        raise AssertionError('no live connection from the connector')

    def outage(self, seconds):
        """Drop every connection now and refuse (close at once) new ones for `seconds`."""
        self.down_until = time.monotonic() + seconds
        for c in list(self.conns):
            c.close()

    def drop_all(self):
        for c in list(self.conns):
            c.close()

    def request(self, action, args, request_id=None, conn=None):
        rid = request_id or f'req-{time.monotonic_ns()}'
        (conn or self.current()).send_json({'type': 'request', 'requestId': rid,
                                            'payload': {'action': action, 'args': args}})
        return rid

    def next_frame(self, predicate, timeout=10, what='frame'):
        deadline = time.monotonic() + timeout
        skipped = []
        try:
            while time.monotonic() < deadline:
                try:
                    item = self.frames.get(timeout=0.1)
                except queue.Empty:
                    continue
                if predicate(item[1]):
                    return item
                skipped.append(item)
        finally:
            for s in skipped:
                self.frames.put(s)
        raise AssertionError(f'timed out waiting for {what}')

    def response(self, rid, timeout=10):
        return self.next_frame(lambda v: isinstance(v, dict) and v.get('type') == 'response'
                               and v.get('requestId') == rid, timeout, f'response {rid}')[1]

    def drain(self):
        out = []
        while True:
            try:
                out.append(self.frames.get_nowait())
            except queue.Empty:
                return out

    def close(self):
        self.stop.set()
        for c in list(self.conns):
            c.close()
        self.listener.close()
        self.thread.join(timeout=2)

    # ---- server --------------------------------------------------------------------------------
    def _serve(self):
        while not self.stop.is_set():
            try:
                raw, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if time.monotonic() < self.down_until:
                raw.close()  # outage: refuse
                continue
            threading.Thread(target=self._session, args=(raw,), daemon=True).start()

    def _session(self, raw):
        conn = None
        try:
            raw.settimeout(60)
            sock = self.ctx.wrap_socket(raw, server_side=True)
            headers = b''
            while b'\r\n\r\n' not in headers:
                headers += _exact(sock, 1)
            lines = headers.decode('ascii').split('\r\n')
            fields = {k.strip().lower(): v.strip() for k, v in
                      (line.split(':', 1) for line in lines[1:] if ':' in line)}
            if 'sec-websocket-protocol' in fields:
                self.errors.append('client sent a WebSocket subprotocol')
            accept = base64.b64encode(hashlib.sha1((fields['sec-websocket-key'] +
                                     '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            sock.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                         b'Sec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
            conn = Conn(self, sock, len(self.conns))
            self.conns.append(conn)
            conn.send_json({'type': 'connected', 'payload': {'clientId': f'fake-{conn.index}'}})
            message = b''
            while not self.stop.is_set() and conn.alive:
                conn.flush()
                if not sock.pending() and not select.select([sock], [], [], 0.02)[0]:
                    continue
                b0, b1 = _exact(sock, 2)
                size = b1 & 127
                if size == 126:
                    size = struct.unpack('!H', _exact(sock, 2))[0]
                elif size == 127:
                    size = struct.unpack('!Q', _exact(sock, 8))[0]
                mask = _exact(sock, 4) if b1 & 128 else b''
                if not mask:
                    self.errors.append('client frame not masked')
                payload = _exact(sock, size)
                if mask:
                    payload = bytes(v ^ mask[i % 4] for i, v in enumerate(payload))
                opcode = b0 & 15
                if opcode == 9:
                    self.ws_pings += 1
                    if self.pongs:
                        _send(sock, conn.lock, 10, payload)
                    continue
                if opcode == 8:
                    break
                if opcode in (1, 0):
                    message += payload
                    if not b0 & 0x80:
                        continue
                    text, message = message.decode(), b''
                    try:
                        value = json.loads(text)
                    except ValueError:
                        value = text
                    self.log.append((time.monotonic(), conn.index, value))
                    if isinstance(value, dict) and value.get('type') == 'identify':
                        self.identifies += 1
                        if self.answer_identify:
                            if self.reject_identify:
                                conn.send_json({'type': 'identifyResponse',
                                                'payload': {'error': {'message': 'bad token'}}})
                            else:
                                conn.send_json({'type': 'identifyResponse',
                                                'payload': {'gameServerId': 'fake-gameserver'}})
                    self.frames.put((conn.index, value))
        except (ssl.SSLError, EOFError, ConnectionError, socket.timeout, OSError, ValueError):
            pass
        except Exception as error:  # noqa: BLE001 - surfaced by the tests
            self.errors.append(repr(error))
        finally:
            if conn:
                conn.alive = False
            try:
                raw.close()
            except OSError:
                pass


def events_of(log, conn=None):
    """gameEvent payloads in arrival order, optionally only those from one connection."""
    out = []
    for _, index, value in log:
        if conn is not None and index != conn:
            continue
        if isinstance(value, dict) and value.get('type') == 'gameEvent':
            out.append(value['payload'])
    return out


if __name__ == '__main__':
    print('library module; see wire_test.py and so_test.py')
    os._exit(0)
