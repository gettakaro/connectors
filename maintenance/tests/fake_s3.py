"""An in-process stand-in for an S3-compatible bucket (Cloudflare R2), path-style.

Serves ``/<bucket>/<key>`` over HTTP/1.1, records every request in order and keeps objects in
memory, so a test can assert what a publisher did: which requests it made, in which order, with
which headers, and which bytes the bucket ended up holding. Authorization is checked only as far as
the access key id in the credential scope; the signature itself is not re-verified.
"""

from __future__ import annotations

import contextlib
import hashlib
import http.server
import re
import threading
from collections.abc import Iterator
from dataclasses import dataclass, field
from urllib.parse import unquote, urlparse

import pytest

BUCKET = "fixture-bucket-name"
KEY_ID = "AKIAFIXTUREKEYID0001"
SECRET = "fixture-secret-access-key-value"


@dataclass(frozen=True)
class Request:
    method: str
    key: str
    content_type: str | None
    cache_control: str | None
    signed_with: str | None


@dataclass
class FakeS3:
    bucket: str = BUCKET
    key_id: str = KEY_ID
    requests: list[Request] = field(default_factory=list)
    objects: dict[str, tuple[bytes, str, str]] = field(default_factory=dict)
    _failures: list[tuple[str, re.Pattern[str], int, str]] = field(default_factory=list, init=False)
    _server: http.server.ThreadingHTTPServer | None = field(default=None, init=False)
    _thread: threading.Thread | None = field(default=None, init=False)

    # -- state a test builds or inspects --------------------------------------
    def seed(self, key: str, payload: bytes, content_type: str = "application/octet-stream") -> None:
        self.objects[key] = (payload, content_type, "")

    def fail_on(self, method: str, key_regex: str, status: int = 403, code: str = "AccessDenied") -> None:
        """Answer matching requests with an S3 error. 403 is not retried by botocore, so tests stay fast."""
        self._failures.append((method, re.compile(key_regex), status, code))

    def clear_failures(self) -> None:
        self._failures.clear()

    def puts(self) -> list[Request]:
        return [request for request in self.requests if request.method == "PUT"]

    def deletes(self) -> list[Request]:
        return [request for request in self.requests if request.method == "DELETE"]

    def heads(self) -> list[Request]:
        return [request for request in self.requests if request.method == "HEAD"]

    @property
    def endpoint(self) -> str:
        assert self._server is not None
        host, port = self._server.server_address[:2]
        return f"http://{host}:{port}"

    # -- lifecycle -------------------------------------------------------------
    def __enter__(self) -> FakeS3:
        fake = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args: object) -> None:
                pass

            def _reply(self, status: int, body: bytes = b"", headers: dict[str, str] | None = None) -> None:
                self.send_response(status)
                for name, value in (headers or {}).items():
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)

            def _error(self, status: int, code: str) -> None:
                body = f"<?xml version='1.0'?><Error><Code>{code}</Code><Message>{code}</Message></Error>".encode()
                self._reply(status, body, {"Content-Type": "application/xml"})

            def _handle(self) -> None:
                length = int(self.headers.get("Content-Length") or 0)
                payload = self.rfile.read(length) if length else b""
                prefix = f"/{fake.bucket}/"
                path = unquote(urlparse(self.path).path)
                key = path[len(prefix) :] if path.startswith(prefix) else path
                authorization = self.headers.get("Authorization") or ""
                match = re.match(r"AWS4-HMAC-SHA256 Credential=([^/]+)/", authorization)
                fake.requests.append(
                    Request(
                        self.command,
                        key,
                        self.headers.get("Content-Type"),
                        self.headers.get("Cache-Control"),
                        match.group(1) if match else None,
                    )
                )
                if not match or match.group(1) != fake.key_id:
                    return self._error(403, "AccessDenied")
                for method, pattern, status, code in fake._failures:
                    if method == self.command and pattern.search(key):
                        return self._error(status, code)
                if self.command == "DELETE":
                    fake.objects.pop(key, None)
                    return self._reply(204)
                if self.command == "PUT":
                    fake.objects[key] = (
                        payload,
                        self.headers.get("Content-Type", ""),
                        self.headers.get("Cache-Control", ""),
                    )
                    return self._reply(200, b"", {"ETag": f'"{hashlib.md5(payload).hexdigest()}"'})
                stored = fake.objects.get(key)
                if stored is None:
                    return self._error(404, "NoSuchKey") if self.command == "GET" else self._reply(404)
                body, content_type, _ = stored
                headers = {"Content-Type": content_type, "ETag": f'"{hashlib.md5(body).hexdigest()}"'}
                if self.command == "HEAD":
                    headers["Content-Length"] = str(len(body))
                    self.send_response(200)
                    for name, value in headers.items():
                        self.send_header(name, value)
                    self.end_headers()
                    return
                self._reply(200, body, headers)

            do_GET = do_HEAD = do_PUT = do_DELETE = _handle  # noqa: N815

        self._server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc: object) -> None:
        assert self._server is not None and self._thread is not None
        self._server.shutdown()
        self._server.server_close()
        self._thread.join()


@contextlib.contextmanager
def serving(
    monkeypatch: pytest.MonkeyPatch,
    prefix: str = "STG_",
    key_id: str = KEY_ID,
    bucket: str = BUCKET,
    secret: str = SECRET,
) -> Iterator[FakeS3]:
    """A running fake with one environment's variables pointed at it, as the maintainer's shell would have them."""
    with FakeS3(bucket=bucket, key_id=key_id) as server:
        monkeypatch.setenv(f"{prefix}R2_ENDPOINT", server.endpoint)
        monkeypatch.setenv(f"{prefix}R2_BUCKET", bucket)
        monkeypatch.setenv(f"{prefix}R2_ACCESS_KEY_ID", key_id)
        monkeypatch.setenv(f"{prefix}R2_SECRET_ACCESS_KEY", secret)
        yield server


PROD_BUCKET = "fixture-prod-bucket-name"
PROD_KEY_ID = "AKIAFIXTUREPRODKEY01"
PROD_SECRET = "fixture-prod-secret-access-key-value"


def serving_production(monkeypatch: pytest.MonkeyPatch) -> contextlib.AbstractContextManager[FakeS3]:
    """A second fake wired to the ``PROD_R2_*`` variables, with credentials of its own."""
    return serving(monkeypatch, "PROD_", PROD_KEY_ID, PROD_BUCKET, PROD_SECRET)
