"""Getting a build's inputs: the filtered server-depot download and the client files.

The server depot is fetched through the pinned DepotDownloader at the target's pinned manifests, with a
file list, into a cache of its own (see ``IconsRoute.data_cache``). The client files are never downloaded:
they come from a directory the maintainer points at, which is checked for completeness before anything
touches the network.
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
from ..exit_codes import IntegrityError, UpstreamUnavailable, UsageError
from ..steam import depotdownloader as dd
from ..steam import vdf
from ..steam.install import COMPLETE_MARKER
from ..steam.selectors import selects
from .catalog_route import APPMANIFEST, IconsRoute

REPIN_HINT = (
    "if Steam no longer serves this pinned build, re-pin the target to the current public build "
    "(takaro-maint steam pin --game {game} --target {target} --write) and rebuild"
)


@dataclass(frozen=True)
class DataTree:
    """The server data a build maps: where it is and what it was."""

    root: Path
    files: int
    tree_sha256: str


def log_path(cache: Path, route: IconsRoute) -> Path:
    return cache / "steam" / "logs" / f"icons-{route.catalog_game}-{route.target.id}.log"


def fetch_data(route: IconsRoute, *, cache: Path, work: Path) -> DataTree:
    """Every depot's filtered files at the pinned manifests, merged under one root."""
    fetched = [_fetch_depot(route, depot, cache) for depot in route.depots]
    roots = [root for root, _ in fetched]
    rows: dict[str, str] = {}
    for _, files in fetched:
        rows.update({relative: entry["sha256"] for relative, entry in files.items()})
    if not rows:
        raise UpstreamUnavailable(
            f"the server depot served no files matching {route.selectors}; {dd.NO_FALLBACK}",
            selectors=route.selectors,
        )
    listing = "".join(f"{relative}\t{rows[relative]}\n" for relative in sorted(rows))
    tree = hashlib.sha256(listing.encode("utf-8")).hexdigest()
    return DataTree(root=_merged(roots, work), files=len(rows), tree_sha256=tree)


def _merged(roots: list[Path], work: Path) -> Path:
    if len(roots) == 1:
        return roots[0]
    merged = work / "data"
    for root in roots:
        shutil.copytree(root, merged, dirs_exist_ok=True)
    return merged


def _read_marker(directory: Path) -> dict[str, Any] | None:
    marker = directory / COMPLETE_MARKER
    if not marker.is_file():
        return None
    try:
        document = json.loads(marker.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    return document if isinstance(document, dict) and isinstance(document.get("files"), dict) else None


def _problems(directory: Path, marker: dict[str, Any]) -> list[str]:
    problems = []
    for relative, expected in sorted(marker["files"].items()):
        path = directory / relative
        if not path.is_file():
            problems.append(f"{relative} is missing")
        elif net.hash_file(path)["sha256"] != expected["sha256"]:
            problems.append(f"{relative} no longer hashes to {expected['sha256']}")
    return problems


def _fetch_depot(route: IconsRoute, depot: str, cache: Path) -> tuple[Path, dict[str, Any]]:
    destination = route.data_cache(cache, depot)
    marker = _read_marker(destination)
    if marker is not None:
        problems = _problems(destination, marker)
        if not problems:
            output.info(f"depot {depot} manifest {route.depots[depot]} is already in the filtered cache")
            return destination, marker["files"]
        output.warn(f"corrupt filtered cache {destination} ({problems[0]}); downloading it again once")
    if destination.exists():
        shutil.rmtree(destination)
    marker = _download(route, depot, destination, cache)
    problems = _problems(destination, marker)
    if problems:
        raise IntegrityError(
            f"the filtered download in {destination} does not match its own marker after a fresh download "
            f"({problems[0]})",
            problems=problems,
        )
    return destination, marker["files"]


def _download(route: IconsRoute, depot: str, destination: Path, cache: Path) -> dict[str, Any]:
    manifest = route.depots[depot]
    staging = destination.parent / f"tmp-{uuid.uuid4().hex}"
    try:
        try:
            dd.download(
                route.steam.app,
                depot,
                manifest,
                branch=route.steam.branch,
                os_=route.steam.os_,
                arch=route.steam.arch,
                dir=staging,
                cache=cache,
                log=log_path(cache, route),
                filelist=route.selectors,
                validate=True,
                credentials=route.steam.credentials,
            )
        except UpstreamUnavailable as exc:
            hint = REPIN_HINT.format(game=route.catalog_game, target=route.target.id)
            raise UpstreamUnavailable(f"{exc.message}\n{hint}", **exc.detail) from exc
        files = _keep_selected(staging, route.selectors)
        marker = {
            "app": route.steam.app,
            "depot": depot,
            "manifest": manifest,
            "selectors": route.selectors,
            "tool": dd.read_lock().version,
            "files": files,
        }
        (staging / COMPLETE_MARKER).parent.mkdir(parents=True, exist_ok=True)
        (staging / COMPLETE_MARKER).write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")
        destination.parent.mkdir(parents=True, exist_ok=True)
        os.replace(staging, destination)
        return marker
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def _keep_selected(directory: Path, selectors: list[str]) -> dict[str, dict[str, Any]]:
    """Drop what DepotDownloader wrote beside the files (its bookkeeping); hash what was asked for."""
    kept: dict[str, dict[str, Any]] = {}
    for path in sorted(directory.rglob("*"), reverse=True):
        if path.is_file():
            relative = path.relative_to(directory).as_posix()
            if selects(relative, selectors):
                digests = net.hash_file(path)
                kept[relative] = {"sha256": digests["sha256"], "size": digests["size"]}
            else:
                path.unlink()
        elif path.is_dir() and not any(path.iterdir()):
            path.rmdir()
    return dict(sorted(kept.items()))


def check_client(client: Path | None, files: list[str]) -> Path:
    """The client directory, once every needed file is known to be in it."""
    if client is None:
        raise UsageError(
            "--client DIR is required: the pixels come from an installed game client; it must hold " + ", ".join(files),
            needs=files,
        )
    if not client.is_dir():
        raise UpstreamUnavailable(f"--client {client} is not a directory", client=str(client))
    missing = [name for name in files if not (client / name).is_file()]
    if missing:
        raise UpstreamUnavailable(
            f"--client {client} lacks {len(missing)} of the needed files: {', '.join(missing)}", missing=missing
        )
    return client


def read_client_build(client: Path, app: int, depot: int) -> tuple[int | None, str | None]:
    """``(buildid, depot manifest)`` from the client's appmanifest, or ``(None, None)`` when there is none.

    Only those two values are read out of the file; it also names the Steam account that owns the install.
    """
    name = APPMANIFEST.format(app=app)
    for candidate in (client / name, client.parent.parent / name):
        if candidate.is_file():
            state = vdf.parse(candidate.read_text(encoding="utf-8", errors="replace")).get("AppState", {})
            buildid = state.get("buildid")
            manifest = (state.get("InstalledDepots") or {}).get(str(depot), {}).get("manifest")
            return (int(buildid) if buildid else None), (str(manifest) if manifest else None)
    return None, None


def hash_pixel_files(client: Path, files: list[str]) -> dict[str, dict[str, Any]]:
    return {name: {key: net.hash_file(client / name)[key] for key in ("sha256", "size")} for name in files}
