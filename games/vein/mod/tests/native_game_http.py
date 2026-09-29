#!/usr/bin/env python3
"""Real LWS HTTP fallback against local HTTP/HTTPS plus a trusted WS peer."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time

from native_transport_tls import Peer, certificate


class Server:
    def __init__(self, key, cert, mode, tls):
        self.mode = mode
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.requests.append(self.path)
                if owner.mode == 'timeout-retry' and len(owner.requests) == 1:
                    time.sleep(4.5)
                if owner.mode == 'status':
                    self.send_response(503)
                    body = b'no players'
                else:
                    self.send_response(200)
                    body = (b'x' * (1024 * 1024 + 1) if owner.mode == 'oversize'
                            else b'[{"steamId":"76561198000000001","name":"test"}]')
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass

            def log_message(self, *_):
                pass

        self.http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            self.http.socket = context.wrap_socket(self.http.socket, server_side=True)
        self.port = self.http.server_port
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join(timeout=2)


def main():
    binary = Path(__file__).resolve().parent / 'build/native_game_http'
    with tempfile.TemporaryDirectory(prefix='vein-game-http-') as temporary:
        directory = Path(temporary)
        key, cert = certificate(directory, 'trusted', 1)
        other_key, other_cert = certificate(directory, 'untrusted', 1)
        ws = Peer(key, cert, 'valid')
        try:
            cases = [
                ('plain HTTP body', key, cert, 'http', 'localhost', 'valid', 'valid'),
                ('verified HTTPS body', key, cert, 'https', 'localhost', 'valid', 'valid'),
                ('HTTPS untrusted chain', other_key, other_cert, 'https', 'localhost', 'valid', 'reject'),
                ('HTTPS hostname mismatch', key, cert, 'https', '127.0.0.1', 'valid', 'reject'),
                ('HTTP non-200', key, cert, 'http', 'localhost', 'status', 'reject'),
                ('HTTP body above 1 MiB', key, cert, 'http', 'localhost', 'oversize', 'reject'),
                ('timeout releases mailbox for next request', key, cert, 'http', 'localhost',
                 'timeout-retry', 'timeout-retry'),
            ]
            for label, http_key, http_cert, scheme, host, server_mode, client_mode in cases:
                print(label, flush=True)
                server = Server(http_key, http_cert, server_mode, scheme == 'https')
                try:
                    url = f'{scheme}://{host}:{server.port}/api'
                    subprocess.run([str(binary), f'wss://localhost:{ws.port}/', str(cert), url,
                                    client_mode], check=True, timeout=12)
                    if client_mode != 'reject' or server_mode in ('status', 'oversize'):
                        assert server.requests and all(path == '/api/players' for path in server.requests), server.requests
                finally:
                    server.close()
            print('explicit empty disables built-in HTTP API', flush=True)
            subprocess.run([str(binary), f'wss://localhost:{ws.port}/', str(cert), '', 'reject'],
                           check=True, timeout=8)
        finally:
            ws.close()
            if ws.errors:
                raise AssertionError(ws.errors)


if __name__ == '__main__':
    main()
