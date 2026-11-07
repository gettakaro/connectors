"""A sparse mirror of the parts of a client install that hold a build's packages.

The client's ``.ucas`` containers are far too large to copy (tens of gigabytes), so a build copies the ``.utoc``
files whole, plans which byte ranges hold the packages it needs (``iostore.plan``) and reads only those from
``--client DIR`` -- a local install or a read-only mount of a remote one -- into sparse files of the same sizes
under the cache. The decoder is then run against the mirror, exactly as it would be against the install.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import net, output
from ..exit_codes import IntegrityError, UpstreamUnavailable
from . import cache_entry, iostore

READ_CHUNK = 8 * 1024 * 1024
WHOLE_CONTAINER = "global"
PLAN_NAME_LENGTH = 16


@dataclass(frozen=True)
class ContainerInfo:
    """One planned ``.utoc``: what is recorded about it in the build record."""

    name: str
    sha256: str
    size: int
    version: int
    entries: int
    partitions: int


@dataclass(frozen=True)
class Mirror:
    root: Path
    plan_sha256: str
    ranges: list[dict[str, Any]]
    containers: dict[str, ContainerInfo]
    missing: list[str]
    bytes: int
    inputs_sha256: str


def _whole(utoc: str) -> bool:
    return Path(utoc).stem == WHOLE_CONTAINER


def _ucas_files(client: Path, utoc: str, *, whole: bool) -> list[str]:
    header = iostore.read_header(client / utoc, indexed=not whole)
    directory = Path(utoc).parent
    stem = Path(utoc).stem
    names = [f"{stem}.ucas" if index == 0 else f"{stem}_s{index}.ucas" for index in range(header.partition_count)]
    return [(directory / name).as_posix() for name in names]


def check(client: Path | None, utoc_files: list[str]) -> Path:
    """Fail fast, before any network or docker: every ``.utoc`` and every ``.ucas`` it implies must be present.

    The ``.utoc`` headers are read, so a container this tool cannot plan (an unknown version, an encrypted
    directory index, no directory index) is refused here and not after the downloads.
    """
    if client is None:
        raise UpstreamUnavailable(
            "--client DIR is required: the pixels come from an installed game client; it must hold "
            + ", ".join(utoc_files),
            needs=utoc_files,
        )
    if not client.is_dir():
        raise UpstreamUnavailable(f"--client {client} is not a directory", client=str(client))
    missing = [name for name in utoc_files if not (client / name).is_file()]
    if missing:
        raise UpstreamUnavailable(
            f"--client {client} lacks {len(missing)} of the needed files: {', '.join(missing)}", missing=missing
        )
    for utoc in utoc_files:
        missing.extend(name for name in _ucas_files(client, utoc, whole=_whole(utoc)) if not (client / name).is_file())
    if missing:
        raise UpstreamUnavailable(
            f"--client {client} lacks {len(missing)} container files: {', '.join(missing)}", missing=missing
        )
    return client


def _sha256_range(path: Path, offset: int, length: int) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        handle.seek(offset)
        remaining = length
        while remaining:
            chunk = handle.read(min(READ_CHUNK, remaining))
            if not chunk:
                raise IntegrityError(f"{path.name} ends before offset {offset + length}", file=path.name)
            digest.update(chunk)
            remaining -= len(chunk)
    return digest.hexdigest()


def _plan_name(utoc_hashes: dict[str, str], ranges: list[dict[str, Any]]) -> tuple[str, str]:
    canonical = json.dumps({"utoc": utoc_hashes, "ranges": [[r["file"], r["offset"], r["length"]] for r in ranges]})
    digest = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return digest, digest[:PLAN_NAME_LENGTH]


def _mirror_problems(root: Path, marker: dict[str, Any]) -> list[str]:
    problems = []
    for entry in marker["ranges"]:
        target = root / entry["file"]
        if not target.is_file() or target.stat().st_size != marker["sizes"][entry["file"]]:
            problems.append(f"{entry['file']} is missing or has the wrong size")
        elif _sha256_range(target, entry["offset"], entry["length"]) != entry["sha256"]:
            problems.append(f"{entry['file']} @{entry['offset']} no longer hashes to {entry['sha256']}")
    for name, digest in marker["utoc"].items():
        path = root / name
        if not path.is_file() or net.sha256_file(path) != digest:
            problems.append(f"{name} no longer hashes to {digest}")
    return problems


def _progress(done: int, total: int) -> None:
    sys.stderr.write(f"\rclient ranges {done / 1e6:8.1f} / {total / 1e6:.1f} MB")
    if done >= total:
        sys.stderr.write("\n")
    sys.stderr.flush()


def _read_into(source: Path, target: Path, entry: dict[str, Any], progress: list[int], total: int) -> str:
    digest = hashlib.sha256()
    with source.open("rb") as reader, target.open("r+b") as writer:
        reader.seek(entry["offset"])
        writer.seek(entry["offset"])
        remaining = entry["length"]
        while remaining:
            chunk = reader.read(min(READ_CHUNK, remaining))
            if not chunk:
                raise IntegrityError(
                    f"{source.name} is shorter than its table of contents implies: read ended "
                    f"{remaining} bytes before offset {entry['offset'] + entry['length']}",
                    file=entry["file"],
                )
            digest.update(chunk)
            writer.write(chunk)
            remaining -= len(chunk)
            progress[0] += len(chunk)
            _progress(progress[0], total)
    return digest.hexdigest()


def _fill(client: Path, staging: Path, utoc_files: list[str], ranges: list[dict[str, Any]]) -> dict[str, Any]:
    sizes: dict[str, int] = {}
    utoc_hashes: dict[str, str] = {}
    for utoc in utoc_files:
        target = staging / utoc
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(client / utoc, target)
        utoc_hashes[utoc] = net.sha256_file(target)
        whole = _whole(utoc)
        for ucas in _ucas_files(client, utoc, whole=whole):
            destination = staging / ucas
            if whole:
                shutil.copyfile(client / ucas, destination)
                digest = net.sha256_file(destination)
                utoc_hashes[ucas] = digest
            else:
                with destination.open("wb") as handle:
                    os.truncate(handle.fileno(), (client / ucas).stat().st_size)
            sizes[ucas] = (client / ucas).stat().st_size
    total = sum(entry["length"] for entry in ranges)
    progress = [0]
    for entry in ranges:
        recorded = _read_into(client / entry["file"], staging / entry["file"], entry, progress, total)
        entry["sha256"] = recorded
    return {"utoc": utoc_hashes, "ranges": ranges, "sizes": sizes}


def mirror(client: Path, utoc_files: list[str], stems: list[str], *, mirror_base: Path) -> Mirror:
    """Plan, then read only the planned ranges from ``client`` into ``mirror_base/<plan>/`` (cached by plan)."""
    planned = [utoc for utoc in utoc_files if not _whole(utoc)]
    tocs = {utoc: iostore.read_toc(client / utoc, utoc) for utoc in planned}
    ranges, missing = iostore.plan(list(tocs.values()), [iostore.safe_stem(stem) for stem in stems])
    entries = [{"file": r.file, "offset": r.offset, "length": r.length} for r in ranges]
    utoc_hashes = {utoc: net.sha256_file(client / utoc) for utoc in utoc_files}
    plan_sha, plan_name = _plan_name(utoc_hashes, entries)
    root = mirror_base / plan_name

    marker = cache_entry.read_marker(root, "ranges")
    if marker is not None:
        problems = _mirror_problems(root, marker)
        if not problems:
            output.info(f"client ranges {plan_name} are already mirrored ({_megabytes(entries)})")
            return _result(root, plan_sha, marker, tocs, utoc_files, missing)
        output.warn(f"corrupt client mirror {root} ({problems[0]}); reading the ranges again once")
    elif root.exists():
        output.warn(f"corrupt client mirror {root} (no usable marker); reading the ranges again once")
    marker = cache_entry.store(root, lambda staging: _build_mirror(client, staging, utoc_files, entries))
    problems = _mirror_problems(root, marker)
    if problems:
        raise IntegrityError(
            f"the client mirror {root} does not match its own marker ({problems[0]})", problems=problems
        )
    return _result(root, plan_sha, marker, tocs, utoc_files, missing)


def _megabytes(entries: list[dict[str, Any]]) -> str:
    return f"{sum(entry['length'] for entry in entries) / 1e6:.1f} MB"


def _build_mirror(client: Path, staging: Path, utoc_files: list[str], entries: list[dict[str, Any]]) -> dict[str, Any]:
    staging.mkdir(parents=True)
    return _fill(client, staging, utoc_files, [dict(entry) for entry in entries])


def _result(
    root: Path,
    plan_sha: str,
    marker: dict[str, Any],
    tocs: dict[str, iostore.Toc],
    utoc_files: list[str],
    missing: list[str],
) -> Mirror:
    containers = {}
    for utoc in utoc_files:
        header = tocs[utoc].header if utoc in tocs else iostore.read_header(root / utoc, indexed=False)
        containers[utoc] = ContainerInfo(
            name=utoc,
            sha256=marker["utoc"][utoc],
            size=(root / utoc).stat().st_size,
            version=header.version,
            entries=header.entry_count,
            partitions=header.partition_count,
        )
    return Mirror(
        root=root,
        plan_sha256=plan_sha,
        ranges=[dict(entry) for entry in marker["ranges"]],
        containers=containers,
        missing=missing,
        bytes=sum(entry["length"] for entry in marker["ranges"]),
        inputs_sha256=hashlib.sha256(json.dumps(marker, sort_keys=True).encode("utf-8")).hexdigest(),
    )
