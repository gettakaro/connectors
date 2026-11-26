"""HTTP downloads with a content-addressed cache. Never falls back to a second URL."""

from __future__ import annotations

import functools
import hashlib
import http.client
import os
import re
import shutil
import socket
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any, Protocol

from . import __version__, output
from .exit_codes import IntegrityError, UpstreamUnavailable

USER_AGENT = f"takaro-connectors-maint/{__version__} (+https://github.com/gettakaro/connectors)"
TIMEOUT_SECONDS = 60
TIMEOUT_ATTEMPTS = 3
RETRY_DELAYS_SECONDS: tuple[float, ...] = (2.0, 6.0)
PinnedAddress = tuple[int, int, int, tuple[Any, ...]]


class Transport(Protocol):
    """How bytes are fetched. Swapped in tests for an in-process fake."""

    def open(self, url: str, headers: dict[str, str]) -> IO[bytes]: ...


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Turn a redirect into the original 3xx response instead of following its Location."""

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: IO[bytes],
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> urllib.request.Request | None:
        del req, fp, code, msg, headers, newurl
        return None


class _PinnedHTTPSConnection(http.client.HTTPSConnection):
    """HTTPS to a validated socket address, with TLS still authenticating ``host``."""

    source_address: tuple[str, int] | None
    _context: ssl.SSLContext

    def __init__(self, host: str, *args: Any, pinned_addresses: tuple[PinnedAddress, ...], **kwargs: Any) -> None:
        self._pinned_addresses = pinned_addresses
        super().__init__(host, *args, **kwargs)

    def connect(self) -> None:
        last_error: OSError | None = None
        for family, socktype, proto, sockaddr in self._pinned_addresses:
            raw = socket.socket(family, socktype, proto)
            try:
                raw.settimeout(self.timeout)
                if self.source_address:
                    raw.bind(self.source_address)
                raw.connect(sockaddr)
            except OSError as exc:
                raw.close()
                last_error = exc
                continue
            self.sock = raw
            break
        else:
            raise last_error or OSError("the bearer realm resolved to no connectable address")

        try:
            self.sock = self._context.wrap_socket(self.sock, server_hostname=self.host)
        except BaseException:
            if self.sock is not None:
                self.sock.close()
                self.sock = None
            raise


class _PinnedHTTPSHandler(urllib.request.HTTPSHandler):
    _context: ssl.SSLContext

    def __init__(self, addresses: tuple[PinnedAddress, ...]) -> None:
        super().__init__()
        self._addresses = addresses

    def https_open(self, req: urllib.request.Request) -> Any:
        connection = functools.partial(_PinnedHTTPSConnection, pinned_addresses=self._addresses)
        return self.do_open(connection, req, context=self._context)


class UrllibTransport:
    def open(self, url: str, headers: dict[str, str]) -> IO[bytes]:
        headers = dict(headers)
        authorization = headers.pop("Authorization", None)
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **headers})
        if authorization:
            # Only for this host: a redirect (GitHub's signed download URL) must not carry it.
            request.add_unredirected_header("Authorization", authorization)
        return urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS)  # noqa: S310

    def open_no_redirect(self, url: str, headers: dict[str, str], addresses: tuple[PinnedAddress, ...]) -> IO[bytes]:
        """Open one URL at a validated address, without redirects or another DNS lookup."""
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **headers})
        # Ignore environment proxies here: a proxy would replace the validated endpoint
        # with its own name resolution. The original hostname remains in the request and
        # is still the SNI/certificate name used by _PinnedHTTPSConnection.
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), _PinnedHTTPSHandler(addresses), _NoRedirect()
        )
        return opener.open(request, timeout=TIMEOUT_SECONDS)  # noqa: S310


_transport: Transport = UrllibTransport()


def set_transport(transport: Transport) -> None:
    global _transport
    _transport = transport


def get_transport() -> Transport:
    return _transport


def open_no_redirect(url: str, headers: dict[str, str], addresses: tuple[PinnedAddress, ...]) -> IO[bytes]:
    """Open exactly ``url`` at validated addresses; never resolve or redirect it again."""
    opener = getattr(_transport, "open_no_redirect", None)
    if opener is None:
        raise OSError("the configured HTTP transport cannot guarantee redirect refusal")
    return opener(url, headers, addresses)


@dataclass(frozen=True)
class Expectation:
    """What the catalog says the bytes must be."""

    sha256: str | None = None
    sha1: str | None = None
    size: int | None = None

    def satisfied_by(self, digests: dict[str, Any]) -> tuple[bool, str]:
        if self.sha256 and digests["sha256"] != self.sha256:
            return False, f"sha256 expected {self.sha256} actual {digests['sha256']}"
        if self.sha1 and digests["sha1"] != self.sha1:
            return False, f"sha1 expected {self.sha1} actual {digests['sha1']}"
        if self.size is not None and digests["size"] != self.size:
            return False, f"size expected {self.size} actual {digests['size']}"
        return True, ""


def _hash_file(path: Path) -> dict[str, Any]:
    sha256 = hashlib.sha256()
    sha1 = hashlib.sha1()
    size = 0
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            sha256.update(chunk)
            sha1.update(chunk)
    return {"sha256": sha256.hexdigest(), "sha1": sha1.hexdigest(), "size": size}


def hash_file(path: Path) -> dict[str, Any]:
    return _hash_file(path)


def sha256_file(path: Path) -> str:
    return _hash_file(path)["sha256"]


def _is_timeout(exc: BaseException) -> bool:
    """A read timeout arrives as TimeoutError, a connect timeout as URLError(reason=TimeoutError)."""
    return isinstance(exc, TimeoutError) or (
        isinstance(exc, urllib.error.URLError) and isinstance(exc.reason, TimeoutError)
    )


#: A GitHub release asset addressed by its id. The API answers that URL with the asset's JSON
#: description unless the request asks for the bytes.
_GITHUB_ASSET_ID_URL = re.compile(r"^https?://[^/]+/repos/[^/]+/[^/]+/releases/assets/[0-9]+$")


def _url_headers(url: str) -> dict[str, str]:
    """Headers a URL's own grammar requires, whoever is fetching it.

    An asset-id URL on api.github.com counts against the API rate limit, which an
    anonymous caller on a shared CI runner exhausts (HTTP 403). ``GH_TOKEN``, when set,
    authenticates that one request; the transport sends it unredirected, so it never
    reaches the download host the API redirects to.
    """
    if not _GITHUB_ASSET_ID_URL.match(url):
        return {}
    headers = {"Accept": "application/octet-stream"}
    token = os.environ.get("GH_TOKEN")
    parts = urllib.parse.urlsplit(url)
    if token and parts.scheme == "https" and parts.hostname == "api.github.com":
        headers["Authorization"] = f"Bearer {token}"
    return headers


def _fetch_to(url: str, target: Path, headers: dict[str, str]) -> dict[str, Any]:
    target.parent.mkdir(parents=True, exist_ok=True)
    headers = {**_url_headers(url), **headers}
    attempts = 0
    while True:
        attempts += 1
        try:
            with _transport.open(url, headers) as response, target.open("wb") as handle:
                shutil.copyfileobj(response, handle, length=1024 * 1024)
            break
        except urllib.error.HTTPError as exc:
            raise UpstreamUnavailable(f"{url} returned HTTP {exc.code}", url=url, status=exc.code) from exc
        except (TimeoutError, urllib.error.URLError) as exc:
            if not _is_timeout(exc):
                reason = getattr(exc, "reason", exc)
                raise UpstreamUnavailable(f"{url} is unreachable: {reason}", url=url) from exc
            if attempts >= TIMEOUT_ATTEMPTS:
                raise UpstreamUnavailable(f"{url} timed out after {attempts} attempts", url=url) from exc
            delay = RETRY_DELAYS_SECONDS[min(attempts - 1, len(RETRY_DELAYS_SECONDS) - 1)]
            output.debug(
                f"timeout on {url} (attempt {attempts} of {TIMEOUT_ATTEMPTS}); retrying the same URL in {delay:g}s"
            )
            time.sleep(delay)
        except OSError as exc:
            raise UpstreamUnavailable(f"{url} is unreachable: {exc}", url=url) from exc
    return _hash_file(target)


def fetch(url: str, dest: Path, expect: Expectation, *, no_cache: bool = False) -> dict[str, Any]:
    """Download ``url`` straight to ``dest``, verifying ``expect``. No cache involved."""
    headers = {"Cache-Control": "no-cache", "Pragma": "no-cache"} if no_cache else {}
    output.debug(f"GET {url}")
    digests = _fetch_to(url, dest, headers)
    ok, reason = expect.satisfied_by(digests)
    if not ok:
        dest.unlink(missing_ok=True)
        raise IntegrityError(f"{url}: {reason}", url=url, **digests)
    return digests


def download(url: str, expect: Expectation, cache: Path) -> Path:
    """Fetch ``url`` into the content-addressed cache and return the blob path.

    A cache hit is re-hashed before use; a corrupt blob is deleted and fetched once more.
    """
    blob = cache / "blobs" / "sha256" / expect.sha256 if expect.sha256 else None
    if blob is not None and blob.exists():
        digests = _hash_file(blob)
        ok, reason = expect.satisfied_by(digests)
        if ok:
            output.debug(f"cache hit {url}")
            return blob
        output.warn(f"corrupt cache blob for {url} ({reason}); re-downloading")
        blob.unlink(missing_ok=True)

    tmp = cache / "tmp" / uuid.uuid4().hex
    try:
        digests = _fetch_to(url, tmp, {})
        ok, reason = expect.satisfied_by(digests)
        if not ok:
            raise IntegrityError(f"{url}: {reason}", url=url, **digests)
        final = cache / "blobs" / "sha256" / digests["sha256"]
        final.parent.mkdir(parents=True, exist_ok=True)
        os.replace(tmp, final)
        return final
    finally:
        tmp.unlink(missing_ok=True)
