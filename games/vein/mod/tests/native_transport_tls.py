#!/usr/bin/env python3
"""Exercise the real LWS client against a local TLS/WebSocket peer, without Takaro credentials."""
import base64
import hashlib
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def certificate(directory, name, days):
    key, csr, cert = (directory / (name + suffix) for suffix in ('.key', '.csr', '.pem'))
    extensions = directory / 'extensions.cnf'
    extensions.write_text('subjectAltName=DNS:localhost\n')
    run('openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
        '-keyout', str(key), '-out', str(csr))
    run('openssl', 'x509', '-req', '-in', str(csr), '-signkey', str(key), '-days', str(days),
        '-extfile', str(extensions), '-out', str(cert))
    return key, cert


def exact(sock, length):
    result = b''
    while len(result) < length:
        part = sock.recv(length - len(result))
        if not part:
            raise EOFError()
        result += part
    return result


def frame(sock, opcode, data, final=True):
    header = bytes([(0x80 if final else 0) | opcode])
    if len(data) < 126:
        header += bytes([len(data)])
    elif len(data) <= 65535:
        header += b'\x7e' + struct.pack('!H', len(data))
    else:
        header += b'\x7f' + struct.pack('!Q', len(data))
    sock.sendall(header + data)


class Peer:
    def __init__(self, key, cert, mode):
        self.mode = mode
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(cert, key)
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.stop = threading.Event()
        self.errors = []
        self.sessions = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            try:
                raw, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            session = threading.Thread(target=self.session, args=(raw,), daemon=True)
            self.sessions.append(session)
            session.start()

    def session(self, raw):
        try:
            raw.settimeout(30)
            with self.context.wrap_socket(raw, server_side=True) as sock:
                headers = b''
                while b'\r\n\r\n' not in headers:
                    headers += exact(sock, 1)
                lines = headers.decode('ascii').split('\r\n')
                fields = dict(line.split(':', 1) for line in lines[1:] if ':' in line)
                fields = {k.lower(): v.strip() for k, v in fields.items()}
                accept = base64.b64encode(hashlib.sha1((fields['sec-websocket-key'] +
                    '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                reply = b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept
                if 'sec-websocket-protocol' in fields:
                    reply += b'\r\nSec-WebSocket-Protocol: ' + fields['sec-websocket-protocol'].encode()
                sock.sendall(reply + b'\r\n\r\n')
                if self.mode == 'blackhole':
                    self.stop.wait(35)
                    return
                if self.mode == 'oversize':
                    # Allow the harness to exercise outbound limits first, then test total
                    # fragmented-message size rather than only an individual fragment.
                    time.sleep(0.5)
                    frame(sock, 1, b'x' * (600 * 1024), final=False)
                    frame(sock, 0, b'x' * (600 * 1024))
                while not self.stop.is_set():
                    first, second = exact(sock, 2)
                    size = second & 127
                    if size == 126:
                        size = struct.unpack('!H', exact(sock, 2))[0]
                    elif size == 127:
                        size = struct.unpack('!Q', exact(sock, 8))[0]
                    if size > 40 * 1024 * 1024:
                        raise ValueError('unexpected client frame size')
                    mask = exact(sock, 4) if second & 128 else b''
                    payload = exact(sock, size)
                    if mask:
                        payload = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
                    opcode = first & 15
                    if os.getenv('VEIN_TLS_TRACE'):
                        print('peer opcode', opcode, 'bytes', size, flush=True)
                    if opcode == 9:
                        frame(sock, 10, b'unsolicited' if self.mode == 'bad-pong' else payload)
                    elif opcode == 1 and self.mode == 'prune' and payload == b'{"seq":1}':
                        self.errors.append('evicted event was transmitted')
                    elif opcode == 8:
                        return
        except (ssl.SSLError, EOFError, ConnectionError, socket.timeout, OSError):
            pass  # Expected for rejected certificates and forced client reconnects.
        except Exception as error:
            self.errors.append(str(error))
        finally:
            raw.close()

    def close(self):
        self.stop.set()
        self.listener.close()
        self.thread.join(timeout=2)


def main():
    binary = Path(__file__).resolve().parent / 'build/native_transport_tls'
    with tempfile.TemporaryDirectory(prefix='vein-tls-') as temporary:
        directory = Path(temporary)
        key, cert = certificate(directory, 'valid', 1)
        expired_key, expired_cert = certificate(directory, 'expired', -1)
        malformed_ca = directory / 'malformed-ca.pem'
        malformed_ca.write_text('This is not a CA certificate.\n')
        cases = [
            ('valid certificate and matching pong', key, cert, 'localhost', str(cert), 'valid'),
            ('evicted queued event is pruned before write', key, cert, 'localhost', str(cert), 'prune'),
            ('critical response written and confirmed by immediate pong', key, cert, 'localhost', str(cert), 'ticket'),
            ('untrusted chain', key, cert, 'localhost', '', 'reject'),
            ('hostname mismatch', key, cert, '127.0.0.1', str(cert), 'reject'),
            ('expired certificate', expired_key, expired_cert, 'localhost', str(expired_cert), 'reject'),
            ('missing CA file', key, cert, 'localhost', str(directory / 'missing.pem'), 'reject'),
            ('malformed CA file', key, cert, 'localhost', str(malformed_ca), 'reject'),
            ('unknown pong must not acknowledge; reconnect', key, cert, 'localhost', str(cert), 'bad-pong'),
            ('peer stops reading; watchdog reconnects without writability', key, cert, 'localhost', str(cert), 'blackhole'),
            ('fragmented incoming message above 1 MiB', key, cert, 'localhost', str(cert), 'oversize'),
        ]
        for label, private_key, public_cert, host, ca, mode in cases:
            print(label, flush=True)
            peer = Peer(private_key, public_cert, mode)
            try:
                subprocess.run([str(binary), f'wss://{host}:{peer.port}/', ca, mode], check=True, timeout=40)
                if peer.errors:
                    raise AssertionError(peer.errors)
            finally:
                peer.close()
                if peer.errors:
                    print('Peer errors:', peer.errors, flush=True)


if __name__ == '__main__':
    main()
