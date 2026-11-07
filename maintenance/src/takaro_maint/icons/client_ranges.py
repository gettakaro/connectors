"""A sparse mirror of the parts of a client install that hold a build's packages.

The client's containers are far too large to copy (tens of gigabytes), so a build plans which byte ranges hold
the packages it needs and reads only those from ``--client DIR`` -- a local install or a read-only mount of a
remote one -- into sparse files of the same sizes under the cache. The decoder is then run against the mirror,
exactly as it would be against the install.

Two container families share this machinery, chosen by the route's ``access``: IoStore (``iostore-ranges``:
the ``.utoc`` files are copied whole and planned with ``iostore.plan``) and legacy paks (``pak-ranges``: the
footer and indexes are among the planned ranges and ``pakfile.plan`` does the planning).
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import uuid
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import net, output
from ..exit_codes import IntegrityError, UpstreamUnavailable
from ..steam.install import COMPLETE_MARKER
from . import iostore, pakfile

READ_CHUNK = 8 * 1024 * 1024
READ_WORKERS = 16
IOSTORE_ACCESS = "iostore-ranges"
PAK_ACCESS = "pak-ranges"
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
    encrypted_entries: int | None = None


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


def check(client: Path | None, utoc_files: list[str], access: str = IOSTORE_ACCESS) -> Path:
    """Fail fast, before any network or docker: every container file and every ``.ucas`` it implies must be present.

    The container headers are read (``.utoc`` or pak footer and index), so a container this tool cannot plan (an
    unknown version, an encrypted index, no directory index) is refused here and not after the downloads.
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
    if access == PAK_ACCESS:
        for pak in utoc_files:
            pakfile.read_index(client / pak, pak)
        return client
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


def _read_marker(root: Path) -> dict[str, Any] | None:
    marker = root / COMPLETE_MARKER
    if not marker.is_file():
        return None
    try:
        document = json.loads(marker.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    return document if isinstance(document, dict) and "ranges" in document else None


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


def _fill(
    client: Path, staging: Path, utoc_files: list[str], ranges: list[dict[str, Any]], access: str
) -> dict[str, Any]:
    sizes: dict[str, int] = {}
    utoc_hashes: dict[str, str] = {}
    for utoc in utoc_files if access == IOSTORE_ACCESS else []:
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
    for pak in utoc_files if access == PAK_ACCESS else []:
        target = staging / pak
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("wb") as handle:
            os.truncate(handle.fileno(), (client / pak).stat().st_size)
        sizes[pak] = (client / pak).stat().st_size
    total = sum(entry["length"] for entry in ranges)
    progress = [0]
    # Many small ranges over a network mount are latency-bound; each worker opens its own handles.
    with ThreadPoolExecutor(max_workers=READ_WORKERS) as pool:
        recorded = pool.map(
            lambda entry: _read_into(client / entry["file"], staging / entry["file"], entry, progress, total), ranges
        )
        for entry, digest in zip(ranges, recorded, strict=True):
            entry["sha256"] = digest
    return {"utoc": utoc_hashes, "ranges": ranges, "sizes": sizes}


def mirror(
    client: Path, utoc_files: list[str], stems: list[str], *, mirror_base: Path, access: str = IOSTORE_ACCESS
) -> Mirror:
    """Plan, then read only the planned ranges from ``client`` into ``mirror_base/<plan>/`` (cached by plan)."""
    safe = [iostore.safe_stem(stem) for stem in stems]
    tocs: dict[str, Any]
    if access == PAK_ACCESS:
        tocs = {pak: pakfile.read_index(client / pak, pak) for pak in utoc_files}
        ranges, missing = pakfile.plan(list(tocs.values()), safe)
        identities = {pak: index.identity_sha256 for pak, index in tocs.items()}
    else:
        tocs = {utoc: iostore.read_toc(client / utoc, utoc) for utoc in utoc_files if not _whole(utoc)}
        ranges, missing = iostore.plan(list(tocs.values()), safe)
        identities = {utoc: net.sha256_file(client / utoc) for utoc in utoc_files}
    entries = [{"file": r.file, "offset": r.offset, "length": r.length} for r in ranges]
    plan_sha, plan_name = _plan_name(identities, entries)
    root = mirror_base / plan_name

    marker = _read_marker(root)
    if marker is not None:
        problems = _mirror_problems(root, marker)
        if not problems:
            output.info(f"client ranges {plan_name} are already mirrored ({_megabytes(entries)})")
            return _result(root, plan_sha, marker, tocs, utoc_files, missing, access)
        output.warn(f"corrupt client mirror {root} ({problems[0]}); reading the ranges again once")
        shutil.rmtree(root)
    marker = _build_mirror(client, root, utoc_files, entries, access)
    problems = _mirror_problems(root, marker)
    if problems:
        raise IntegrityError(
            f"the client mirror {root} does not match its own marker ({problems[0]})", problems=problems
        )
    return _result(root, plan_sha, marker, tocs, utoc_files, missing, access)


def _megabytes(entries: list[dict[str, Any]]) -> str:
    return f"{sum(entry['length'] for entry in entries) / 1e6:.1f} MB"


def _build_mirror(
    client: Path, root: Path, utoc_files: list[str], entries: list[dict[str, Any]], access: str
) -> dict[str, Any]:
    staging = root.parent / f"tmp-{uuid.uuid4().hex}"
    try:
        staging.mkdir(parents=True)
        marker = _fill(client, staging, utoc_files, [dict(entry) for entry in entries], access)
        (staging / COMPLETE_MARKER).parent.mkdir(parents=True, exist_ok=True)
        # The marker is replaced atomically: written beside its final name, then renamed over it.
        sibling = staging / f"{COMPLETE_MARKER.as_posix()}.new"
        sibling.write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")
        os.replace(sibling, staging / COMPLETE_MARKER)
        os.replace(staging, root)
        return marker
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def _result(
    root: Path,
    plan_sha: str,
    marker: dict[str, Any],
    tocs: dict[str, Any],
    utoc_files: list[str],
    missing: list[str],
    access: str,
) -> Mirror:
    containers = {}
    for utoc in utoc_files:
        if access == PAK_ACCESS:
            index: pakfile.PakIndex = tocs[utoc]
            containers[utoc] = ContainerInfo(
                name=utoc,
                sha256=index.identity_sha256,
                size=index.size,
                version=index.version,
                entries=len(index.files),
                partitions=1,
                encrypted_entries=index.encrypted_entries,
            )
            continue
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
