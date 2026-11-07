"""An immutable local mirror of the client files a build selected, mounted read-only into the decoder containers.

The installed client may sit on a network mount and holds far more than a build reads. The decoders get only the
selected files, copied once into a directory named after their content. Nothing in a mirror is ever rewritten: a
changed client gives a different directory.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import net, output
from ..exit_codes import IntegrityError

MARKER = Path(".takaro") / "complete.json"


@dataclass(frozen=True)
class ClientMirror:
    root: Path
    files: dict[str, dict[str, Any]]
    tree_sha256: str


def _tree_sha256(files: dict[str, dict[str, Any]]) -> str:
    listing = "".join(f"{name}\t{files[name]['sha256']}\n" for name in sorted(files))
    return hashlib.sha256(listing.encode("utf-8")).hexdigest()


def _hash_all(root: Path, names: list[str]) -> dict[str, dict[str, Any]]:
    return {name: {key: net.hash_file(root / name)[key] for key in ("sha256", "size")} for name in names}


def _intact(directory: Path, files: dict[str, dict[str, Any]]) -> bool:
    marker = directory / MARKER
    if not marker.is_file() or json.loads(marker.read_text(encoding="utf-8"))["files"] != files:
        return False
    return all((directory / name).is_file() and _hash_all(directory, [name])[name] == files[name] for name in files)


def mirror(cache: Path, *, pack: str, app: int, buildid: int | None, client: Path, files: list[str]) -> ClientMirror:
    """The mirror of ``files`` (client-relative) under the cache, created when it is missing or no longer intact."""
    wanted = _hash_all(client, sorted(files))
    tree = _tree_sha256(wanted)
    base = cache / "icons" / "client-files" / pack / str(app) / (str(buildid) if buildid else "unknown")
    directory = base / tree[:16]
    if directory.exists():
        if _intact(directory, wanted):
            output.info(f"client files mirror {tree[:12]} is already complete")
            return ClientMirror(directory, wanted, tree)
        output.warn(f"corrupt client files mirror {directory}; copying it again once")
        shutil.rmtree(directory)
    _copy(client, wanted, directory)
    if not _intact(directory, wanted):
        raise IntegrityError(f"the client files mirror {directory} does not match its own marker after a fresh copy")
    return ClientMirror(directory, wanted, tree)


def _copy(client: Path, files: dict[str, dict[str, Any]], directory: Path) -> None:
    staging = directory.parent / f"tmp-{uuid.uuid4().hex}"
    try:
        for name in files:
            destination = staging / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(client / name, destination)
        marker = staging / MARKER
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(json.dumps({"files": files}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        directory.parent.mkdir(parents=True, exist_ok=True)
        os.replace(staging, directory)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
