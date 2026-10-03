"""The reusable exact install: pinned depots in, a verified game directory out.

The whole installation arrives as one unit, so the generic ``install`` command's
file-by-file staging does not fit. What is kept is its promise: a failed preparation
leaves the existing directory byte-identical, user data is preserved, the previous
install is kept next to the new one, and the ledger is written last.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import shutil
import subprocess
import uuid
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import __version__, net, output
from ..exit_codes import ConflictError, IntegrityError, UsageError
from ..install.ledger import artifacts_of, read_ledger, write_ledger
from . import depotdownloader as dd

COMPLETE_MARKER = Path(".takaro") / "complete.json"
PREVIOUS_SUFFIX = ".previous"


@dataclass(frozen=True)
class SteamInput:
    """One ``steam-depots`` input, read from a target record."""

    name: str
    app: int
    branch: str
    buildid: int
    os_: str
    arch: str
    depots: dict[str, dict[str, Any]]
    files: dict[str, dict[str, Any]]
    credentials: dict[str, str] | None

    @classmethod
    def of(cls, target_record: dict[str, Any]) -> SteamInput:
        found = [(name, spec) for name, spec in target_record["inputs"].items() if spec.get("kind") == "steam-depots"]
        if len(found) != 1:
            raise UsageError(
                f"target '{target_record['id']}' declares {len(found)} steam-depots inputs; exactly one is supported"
            )
        name, spec = found[0]
        return cls(
            name=name,
            app=int(spec["app"]),
            branch=str(spec["branch"]),
            buildid=int(spec["buildid"]),
            os_=str(spec["os"]),
            arch=str(spec["arch"]),
            depots={str(k): dict(v) for k, v in spec["depots"].items()},
            files={str(k): dict(v) for k, v in spec["files"].items()},
            credentials=spec.get("credentials"),
        )

    def declared_paths(self) -> list[str]:
        return sorted(self.files)


def _safe_join(root: Path, relative: str, *, field: str) -> Path:
    """Join a record-supplied path under ``root``, refusing anything that could climb out.

    ``preserve[]`` names dot-directories (``.takaro/``), which the install-path grammar
    rejects, so the rule here is the one that matters for a join: relative, no ``..``,
    no empty segment, no backslash.
    """
    text = str(relative).replace("\\", "/")
    segments = [segment for segment in text.split("/") if segment not in ("", ".")]
    if not segments or text.startswith("/") or any(segment == ".." for segment in segments):
        raise UsageError(
            f"{field} must be a relative path inside the install directory "
            f"(no leading '/', no '..', no backslash), not {relative!r}"
        )
    return root.joinpath(*segments)


def depot_cache(cache: Path, app: int, depot: str, manifest: str) -> Path:
    return cache / "steam" / str(app) / str(depot) / str(manifest)


def _verify_declared(root: Path, files: dict[str, dict[str, Any]], *, required: bool) -> list[str]:
    """Every declared file under ``root`` that does not hash as recorded, as a reason list."""
    problems: list[str] = []
    for relative, expected in sorted(files.items()):
        path = _safe_join(root, relative, field="inputs.files key")
        if not path.is_file():
            if required:
                problems.append(f"{relative} is missing")
            continue
        digests = net.hash_file(path)
        if expected.get("sha256") and digests["sha256"] != expected["sha256"]:
            problems.append(f"{relative} sha256 expected {expected['sha256']} actual {digests['sha256']}")
        if expected.get("size") is not None and digests["size"] != int(expected["size"]):
            problems.append(f"{relative} size expected {expected['size']} actual {digests['size']}")
    return problems


def _complete(root: Path) -> dict[str, Any] | None:
    marker = root / COMPLETE_MARKER
    if not marker.is_file():
        return None
    try:
        return json.loads(marker.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def fetch_depot(
    spec: SteamInput,
    depot: str,
    *,
    cache: Path,
    log: Path,
    filelist: list[str] | None = None,
    force: bool = False,
) -> Path:
    """The cache directory holding exactly this depot manifest, downloading it if needed.

    A cache hit is checked against what the download recorded, never against what the
    catalog says today. The two are different questions: "are these still the bytes that
    arrived?" is corruption and costs a re-download, while "are these the bytes the record
    declares?" is a disagreement between the record and Steam, which no amount of
    re-downloading fixes — that one is answered by the caller, before anything is deleted.
    """
    manifest = str(spec.depots[depot]["manifest"])
    destination = depot_cache(cache, spec.app, depot, manifest)
    marker = _complete(destination)
    if not force and marker is not None:
        recorded = {path: entry for path, entry in (marker.get("files") or {}).items()}
        problems = _verify_declared(destination, recorded, required=True)
        if not problems:
            output.info(f"depot {depot} manifest {manifest} is already in the cache")
            return destination
        output.warn(f"corrupt depot cache {destination} ({problems[0]}); re-downloading once")
        shutil.rmtree(destination, ignore_errors=True)

    staging = destination.parent / f"tmp-{uuid.uuid4().hex}"
    try:
        dd.download(
            spec.app,
            depot,
            manifest,
            branch=spec.branch,
            os_=spec.os_,
            arch=spec.arch,
            dir=staging,
            cache=cache,
            log=log,
            filelist=filelist,
            validate=True,
            credentials=spec.credentials,
        )
        marker_path = staging / COMPLETE_MARKER
        marker_path.parent.mkdir(parents=True, exist_ok=True)
        # What arrived, so a later run can tell corruption from a re-pinned record.
        arrived: dict[str, Any] = {}
        for relative in spec.declared_paths():
            path = _safe_join(staging, relative, field="inputs.files key")
            if path.is_file():
                digests = net.hash_file(path)
                arrived[relative] = {"sha256": digests["sha256"], "size": int(digests["size"])}
        marker_path.write_text(
            json.dumps(
                {
                    "app": spec.app,
                    "depot": depot,
                    "manifest": manifest,
                    "downloadedAt": _now(),
                    "tool": f"depotdownloader/{dd.read_lock().version}",
                    "files": arrived,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.rmtree(destination, ignore_errors=True)
        os.replace(staging, destination)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return destination


def _now() -> str:
    return dt.datetime.now(dt.UTC).isoformat().replace("+00:00", "Z")


def _copy_tree(source: Path, destination: Path) -> None:
    """Copy a whole depot tree, letting the filesystem share blocks where it can."""
    destination.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        ["cp", "-a", "--reflink=auto", f"{source}/.", str(destination)],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        # No GNU cp (or a filesystem that refused): the slow path copies the same tree.
        output.debug(f"cp -a fell back to shutil ({completed.stderr.strip()})")
        shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=True)


def _refuse_symlinked_path(root: Path, entry: Path, relative: str) -> None:
    """Refuse a preserved entry reached through a symlink, or one that is a symlink."""
    current = entry
    while True:
        if current.is_symlink():
            raise IntegrityError(
                f"preserved entry {relative!r} is a symlink; refusing to carry it into the new install"
                if current == entry
                else f"preserved entry {relative!r} is reached through the symlink "
                f"{current.relative_to(root)}; refusing to carry it into the new install"
            )
        if current == root or current.parent == current:
            return
        current = current.parent


def _copy_preserved(dest: Path, staging: Path, preserve: list[str]) -> list[str]:
    """Carry the entries a target preserves from the existing install into the new one."""
    kept: list[str] = []
    for pattern in preserve:
        relative = pattern.rstrip("/")
        if not relative:
            continue
        source = _safe_join(dest, relative, field="preserve[]")
        if not source.exists():
            continue
        # A preserved entry is carried by *copying*, so a symlink anywhere on the way to
        # it -- or the entry itself -- would make the copy read whatever it points at and
        # write those bytes into the new install as a real file. The operator's own
        # `~/.ssh/id_ed25519` is not this game's server data, however the link got there.
        _refuse_symlinked_path(dest, source, relative)
        target = _safe_join(staging, relative, field="preserve[]")
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(source, target, symlinks=True)
        else:
            shutil.copy2(source, target)
        kept.append(relative)
    return kept


def ledger_inputs(spec: SteamInput, root: Path) -> list[dict[str, Any]]:
    """One ledger row per declared file, hashed where it now lives."""
    rows: list[dict[str, Any]] = []
    for relative in spec.declared_paths():
        path = _safe_join(root, relative, field="inputs.files key")
        digests = net.hash_file(path)
        rows.append(
            {
                "name": f"{spec.name}:{relative}",
                "path": relative,
                "sha256": digests["sha256"],
                "size": int(digests["size"]),
            }
        )
    return rows


def _write_ledger(target: Any, dest: Path, spec: SteamInput, previous: dict[str, Any] | None) -> dict[str, Any]:
    ledger = {
        "schemaVersion": 1,
        "game": target.game,
        "target": target.id,
        "fingerprint": target.fingerprint,
        "installedAt": _now(),
        "installedBy": f"takaro-maint/{__version__}",
        "inputs": ledger_inputs(spec, dest),
        "container": {
            "image": target.record["runtime"]["container"]["image"],
            "tag": target.record["runtime"]["container"]["tag"],
            "digest": target.record["runtime"]["container"]["digest"],
        },
        "world": {"revision": target.revision, "createdBy": target.id},
    }
    if previous and previous.get("fingerprint") == target.fingerprint:
        carried = artifacts_of(previous)
        if carried:
            ledger["artifacts"] = carried
    write_ledger(dest, ledger)
    return ledger


# Steam records which depot files are executable, but DepotDownloader writes every file
# 0644, so a tree taken straight from the manifests cannot start its server binary (or the
# helpers that binary spawns, such as a crash handler).
# The bit is restored from what the files are — an ELF image or a shebang script — rather
# than from a list of names a future build could grow out of.
_ELF_MAGIC = b"\x7fELF"
_SHEBANG = b"#!"


def _is_program(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            head = handle.read(4)
    except OSError:
        return False
    return head.startswith(_ELF_MAGIC) or head.startswith(_SHEBANG)


def restore_executables(root: Path) -> int:
    """Give every program in a freshly downloaded tree its executable bit back."""
    marked = 0
    for path in root.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        mode = path.stat().st_mode
        if mode & 0o111:
            continue
        if _is_program(path):
            path.chmod((mode | 0o755) & 0o7777)
            marked += 1
    if marked:
        output.info(f"restored the executable bit on {marked} file(s) the depots deliver as 0644")
    return marked

def tree_hash(root: Path) -> str:
    from ..commands.install import tree_hash as _tree_hash

    return _tree_hash(root)


def install_exact(
    target: Any,
    *,
    dest: Path,
    preserve: list[str],
    cache: Path,
    log: Path,
    dry_run: bool = False,
    post_install: Callable[[Path], None] | None = None,
) -> dict[str, Any]:
    """Install exactly the pinned depots into ``dest``, or leave ``dest`` untouched."""
    spec = SteamInput.of(target.record)
    existing = read_ledger(dest)

    if existing is not None and existing.fingerprint == target.fingerprint:
        problems = _verify_declared(dest, spec.files, required=True)
        if not problems:
            output.info(f"{dest} already holds {target.id} ({target.fp16}); nothing to do")
            return {
                "status": "already-installed",
                "game": target.game,
                "target": target.id,
                "fingerprint": target.fingerprint,
                "dest": str(dest),
                "previous": None,
                "depots": _depot_summary(spec),
                "inputs": existing.data.get("inputs", []),
                "preserved": [],
            }
        output.info("ledger present but stale: " + "; ".join(problems))

    staging = dest.with_name(dest.name + f".staging-{target.fp16}")
    if dry_run:
        return {
            "status": "dry-run",
            "game": target.game,
            "target": target.id,
            "fingerprint": target.fingerprint,
            "dest": str(dest),
            "previous": None,
            "depots": _depot_summary(spec),
            "inputs": [
                {"name": f"{spec.name}:{path}", "path": path, **spec.files[path]} for path in spec.declared_paths()
            ],
            "preserved": sorted(preserve),
            "staging": str(staging),
        }

    before = tree_hash(dest)
    shutil.rmtree(staging, ignore_errors=True)
    preserved: list[str] = []
    # The install that was in service, once it has been moved aside: whatever fails after
    # that point has to put it back.
    retired: Path | None = None
    # Did the staged tree reach `dest`? On a fresh install there is nothing to retire, so
    # a failure after the swap has to be undone by this flag rather than by `retired`.
    went_live = False
    try:
        staging.mkdir(parents=True, exist_ok=True)
        for depot in sorted(spec.depots):
            cached = fetch_depot(spec, depot, cache=cache, log=log)
            # Checked here, on the download itself: bytes that disagree with the record are
            # refused before a single one is copied, and the cache is kept — re-downloading
            # a depot cannot change what the record declares.
            in_depot = {
                relative: expected
                for relative, expected in spec.files.items()
                if _safe_join(cached, relative, field="inputs.files key").is_file()
            }
            problems = _verify_declared(cached, in_depot, required=True)
            if problems:
                raise IntegrityError(
                    f"depot {depot} manifest {spec.depots[depot]['manifest']} does not match the pinned target: "
                    + "; ".join(problems)
                    + f"; the existing install at {dest} is untouched",
                    target=target.id,
                )
            output.info(f"staging depot {depot} into {staging.name}")
            _copy_tree(cached, staging)
        shutil.rmtree(staging / COMPLETE_MARKER.parent, ignore_errors=True)

        problems = _verify_declared(staging, spec.files, required=True)
        if problems:
            raise IntegrityError(
                "the downloaded depot does not match the pinned target: "
                + "; ".join(problems)
                + f"; the existing install at {dest} is untouched",
                target=target.id,
            )

        if dest.exists():
            preserved = _copy_preserved(dest, staging, preserve)
        if post_install is not None:
            post_install(staging)

        previous_path = dest.with_name(dest.name + PREVIOUS_SUFFIX)
        previous_data = existing.data if existing is not None else None
        if dest.exists():
            shutil.rmtree(previous_path, ignore_errors=True)
            os.replace(dest, previous_path)
            retired = previous_path
        else:
            previous_path = None  # type: ignore[assignment]
            dest.parent.mkdir(parents=True, exist_ok=True)
        os.replace(staging, dest)
        went_live = True
        # The install is finished only once the directory can say what it is, so the ledger
        # is written inside the same protected window as the swap: a ledger that cannot be
        # written puts the install that was in service back rather than leaving the new tree
        # live under the old identity.
        ledger = _write_ledger(target, dest, spec, previous_data)
    except BaseException:
        _restore_retired(dest, staging, retired, went_live=went_live)
        shutil.rmtree(staging, ignore_errors=True)
        after = tree_hash(dest)
        if after == before:
            output.info(f"{dest} is byte-identical to what it was before the failed install")
        else:
            output.warn(f"{dest} changed during a failed install")
        raise
    return {
        "status": "installed",
        "game": target.game,
        "target": target.id,
        "fingerprint": target.fingerprint,
        "dest": str(dest),
        "previous": str(previous_path) if previous_path else None,
        "depots": _depot_summary(spec),
        "inputs": ledger["inputs"],
        "preserved": preserved,
    }


def _restore_retired(dest: Path, staging: Path, retired: Path | None, *, went_live: bool) -> None:
    """Put the install that was moved aside back into service after a failed swap.

    Called on every failure path, including the one where ``dest`` was never touched: with
    nothing retired there is nothing to undo. When the new tree did go live, it is moved
    back to the staging name first, so the caller's cleanup removes it.

    On a *fresh* install there is nothing to retire, so a failure after the swap -- the
    ledger write is inside the same window -- would otherwise leave the new tree live with
    no ledger: a directory full of game files that cannot say which target it holds, which
    every later run would read as an unknown install rather than as the failure it is.
    """
    if retired is None:
        if went_live and dest.exists():
            shutil.rmtree(staging, ignore_errors=True)
            try:
                os.replace(dest, staging)
            except OSError as exc:
                output.warn(f"could not remove the unfinished install at {dest} ({exc})")
                return
            output.warn(f"removed the unfinished install at {dest}")
        return
    if not retired.is_dir():
        return
    if dest.exists():
        shutil.rmtree(staging, ignore_errors=True)
        try:
            os.replace(dest, staging)
        except OSError as exc:
            output.warn(f"could not take {dest} out of service ({exc}); {retired} was left in place")
            return
    try:
        os.replace(retired, dest)
    except OSError as exc:
        output.warn(f"could not restore {retired} to {dest} ({exc})")
        return
    output.warn(f"restored the previous install to {dest} after a failed install")


def _depot_summary(spec: SteamInput) -> dict[str, Any]:
    return {
        depot: {"manifest": str(entry["manifest"]), "app": spec.app, "branch": spec.branch, "buildid": spec.buildid}
        for depot, entry in sorted(spec.depots.items())
    }


def rollback(target: Any, *, dest: Path) -> dict[str, Any]:
    """Put the install this target replaced back, and prove it is what its ledger says."""
    previous_path = dest.with_name(dest.name + PREVIOUS_SUFFIX)
    if not previous_path.is_dir():
        raise ConflictError(f"no previous install at {previous_path}; there is nothing to roll back to")
    previous_ledger = read_ledger(previous_path)
    if previous_ledger is None:
        raise ConflictError(f"{previous_path} holds no ledger; refusing to restore an unidentified install")

    # Checked before it goes live, against the hashes it recorded when it was installed:
    # rolling back to a tree that has since been damaged would replace one broken install
    # with another and call it a recovery.
    recorded = {
        str(row["path"]): {"sha256": row.get("sha256"), "size": row.get("size")}
        for row in previous_ledger.data.get("inputs", [])
        if row.get("path")
    }
    damaged = _verify_declared(previous_path, recorded, required=True)
    if damaged:
        raise IntegrityError(
            f"{previous_path} no longer matches its own ledger: "
            + "; ".join(damaged)
            + f"; refusing to put it back into service, {dest} is untouched",
            target=target.id,
        )

    current = read_ledger(dest)
    holding = previous_path.with_name(previous_path.name + f".tmp-{uuid.uuid4().hex}")
    moved_aside = False
    if dest.exists():
        os.replace(dest, holding)
        moved_aside = True
    try:
        os.replace(previous_path, dest)
    except BaseException:
        if moved_aside:
            os.replace(holding, dest)
        raise
    if moved_aside:
        try:
            os.replace(holding, previous_path)
        except OSError as exc:
            # The rollback itself is done; only the bookkeeping name is wrong.
            output.warn(f"rolled back, but the replaced install is left at {holding} ({exc})")

    restored = read_ledger(dest)
    assert restored is not None
    spec = SteamInput.of(target.record)
    # What the restored install is *not*: it predates the selected target, so a mismatch
    # here is expected and reported rather than fatal.
    problems = _verify_declared(dest, spec.files, required=False)
    return {
        "status": "rolled-back",
        "game": target.game,
        "target": target.id,
        "fingerprint": restored.fingerprint,
        "replaced": current.fingerprint if current is not None else None,
        "dest": str(dest),
        "previous": str(previous_path) if previous_path.is_dir() else None,
        "depots": _depot_summary(spec),
        "inputs": restored.data.get("inputs", []),
        "preserved": [],
        "problems": problems,
    }
