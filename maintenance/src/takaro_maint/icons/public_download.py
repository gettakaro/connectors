"""Public downloads pinned by hash: the version manifest, ``server.jar`` and ``client.jar`` of a Minecraft release.

Everything is public Mojang data and is fetched without an account. The version manifest and the server jar are the
target's own pins (``inputs.<name>`` of kind ``mojang-version``); the client jar is pinned in the ``icons`` block per
target revision, so the target fingerprint does not move. The manifest vouches for both jars: a difference between what
it names and what the catalog pins is an integrity failure, not something to fetch around.

Downloads are cached by sha1 (``net.download`` only caches by sha256, which a Mojang pin does not give).
"""

from __future__ import annotations

import json
import os
import shutil
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import net, output
from ..catalog import ids
from ..catalog.loader import Target
from ..exit_codes import IntegrityError, UsageError

MARKER = Path(".takaro") / "complete.json"


@dataclass(frozen=True)
class PinnedDownload:
    url: str
    sha1: str
    size: int | None
    name: str

    def directory(self, cache: Path) -> Path:
        return cache / "icons" / "downloads" / "sha1" / self.sha1

    def cached(self, cache: Path) -> bool:
        return (self.directory(cache) / MARKER).is_file()


@dataclass(frozen=True)
class DownloadedFile:
    path: Path
    url: str
    sha1: str
    sha256: str
    size: int

    def record(self) -> dict[str, Any]:
        return {"url": self.url, "sha1": self.sha1, "sha256": self.sha256, "size": self.size}


def _expectation(pin: PinnedDownload) -> net.Expectation:
    return net.Expectation(sha1=pin.sha1, size=pin.size)


def _cached_file(pin: PinnedDownload, directory: Path) -> DownloadedFile | None:
    """The file of a complete cache entry when its bytes still match the pin, else ``None`` (after a warning)."""
    path = directory / pin.name
    if not (directory / MARKER).is_file():
        return None
    digests = net.hash_file(path) if path.is_file() else None
    if digests is not None and _expectation(pin).satisfied_by(digests)[0]:
        output.debug(f"cache hit {pin.url}")
        return DownloadedFile(path, pin.url, pin.sha1, digests["sha256"], digests["size"])
    output.warn(f"corrupt cached download {pin.name} ({pin.sha1[:12]}); downloading again once")
    shutil.rmtree(directory)
    return None


def fetch(pin: PinnedDownload, cache: Path) -> DownloadedFile:
    """The pinned file in the cache, downloaded when missing or corrupt; a wrong download is an integrity failure."""
    directory = pin.directory(cache)
    hit = _cached_file(pin, directory)
    if hit is not None:
        return hit
    staging = directory.parent / f"tmp-{uuid.uuid4().hex}"
    try:
        digests = net.fetch(pin.url, staging / pin.name, _expectation(pin))
        marker = staging / MARKER
        marker.parent.mkdir(parents=True)
        marker.write_text(
            json.dumps(
                {"url": pin.url, "sha1": pin.sha1, "sha256": digests["sha256"], "size": digests["size"]}, indent=2
            )
            + "\n",
            encoding="utf-8",
        )
        os.replace(staging, directory)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return DownloadedFile(directory / pin.name, pin.url, pin.sha1, digests["sha256"], digests["size"])


@dataclass(frozen=True)
class PublicInputs:
    """The three pins of one target revision's public-download route, resolved without the network."""

    input: str
    version: str
    manifest: PinnedDownload
    server: PinnedDownload
    client: PinnedDownload

    @classmethod
    def of(cls, game_record: dict[str, Any], target: Target, block: dict[str, Any]) -> PublicInputs:
        name = str(block["data"]["input"])
        spec = target.record["inputs"].get(name)
        if spec is None or spec.get("kind") != "mojang-version":
            raise UsageError(f"target '{target.id}' has no mojang-version input named '{name}'")
        pin = (block["pixels"]["downloads"]).get(target.revision)
        if pin is None:
            raise UsageError(
                f"no client.jar pin for revision {target.revision} (icons.pixels.downloads); "
                f"use --target with a pinned revision ({', '.join(sorted(block['pixels']['downloads']))}) or add a pin",
                revision=target.revision,
            )
        server = spec["server"]
        return cls(
            input=name,
            version=str(spec["version"]),
            manifest=PinnedDownload(
                ids.resolved_url(game_record, spec["source"], spec["manifest"]["path"]),
                str(spec["manifest"]["sha1"]),
                None,
                "version.json",
            ),
            server=PinnedDownload(
                ids.resolved_url(game_record, server["source"], server["path"]),
                str(server["sha1"]),
                int(server["size"]),
                "server.jar",
            ),
            client=PinnedDownload(
                ids.resolved_url(game_record, pin["source"], pin["path"]),
                str(pin["sha1"]),
                int(pin["size"]),
                "client.jar",
            ),
        )


@dataclass(frozen=True)
class PublicFiles:
    version: str
    manifest: DownloadedFile
    server: DownloadedFile
    client: DownloadedFile


def _require(actual: Any, expected: Any, what: str, url: str) -> None:
    if actual != expected:
        raise IntegrityError(f"{url}: the version manifest names {what} {actual}, the catalog pins {expected}", url=url)


def acquire(public: PublicInputs, cache: Path) -> PublicFiles:
    """Fetch and cross-check the manifest, then the server jar and the client jar it vouches for."""
    manifest = fetch(public.manifest, cache)
    document = json.loads(manifest.path.read_text(encoding="utf-8"))
    url = public.manifest.url
    _require(document.get("id"), public.version, "version", url)
    for role, pin in (("server", public.server), ("client", public.client)):
        named = document["downloads"][role]
        _require(named["sha1"], pin.sha1, f"{role} sha1", url)
        _require(int(named["size"]), pin.size, f"{role} size", url)
    return PublicFiles(public.version, manifest, fetch(public.server, cache), fetch(public.client, cache))
