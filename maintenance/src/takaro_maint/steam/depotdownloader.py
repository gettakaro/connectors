"""The pinned DepotDownloader: obtained by hash, run with pinned arguments, never guessed.

Every Steam acquisition goes through one tool at one version, recorded in
``maintenance/tools.lock.json`` by url, size and sha256. Bytes that do not match the
lock are refused rather than run, and a manifest Steam will not serve is an upstream
failure -- never a silent fall back to whatever the branch head has become.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import uuid
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .. import net, output, paths, redact
from ..exit_codes import UpstreamUnavailable, UsageError

LOCK_RELATIVE = Path("maintenance") / "tools.lock.json"
TOOL_ID = "depotdownloader"
# What the cached executable was unpacked from, written beside it.
TOOL_MARKER = ".takaro-tool.json"

# The line every classified failure carries, so a caller (and a test) can tell this
# apart from any other non-zero exit.
NO_FALLBACK = "not falling back to branch head"

_MANIFEST_ID = re.compile(r"Manifest ID / date\s*:\s*(?P<id>[0-9]+)\s*/\s*(?P<date>.*)")
# What the tool prints once it has the manifest it is about to download: "Manifest <id> (<date>)".
_SERVED = re.compile(r"^\s*Manifest\s+(?P<id>[0-9]+)\s*\(", re.MULTILINE)
_TOTAL_FILES = re.compile(r"Total number of files\s*:\s*(?P<value>[0-9]+)")
_TOTAL_BYTES = re.compile(r"Total bytes on disk\s*:\s*(?P<value>[0-9]+)")
# "        14800      1 c2eb0…(40 hex) 0 7DaysToDieServer.x86_64"
_FILE_ROW = re.compile(
    r"^\s*(?P<size>[0-9]+)\s+(?P<chunks>[0-9]+)\s+(?P<sha1>[0-9a-f]{40})\s+(?P<flags>[0-9]+)\s+(?P<name>\S.*)$"
)

#: Steam's answer to an anonymous request for a manifest that is no longer the branch head. Set
#: as ``reason`` on the error so a caller can tell it apart from every other upstream failure,
#: which all share exit 4: a pull request that changes nothing about the game may skip this one.
MANIFEST_NOT_SERVED = "manifest-not-served"
_NOT_SERVED_MARKERS = (
    "no manifest request code",
    "unable to get manifest request code",
    "encountered 401 for depot manifest",
)

_UNAVAILABLE_MARKERS = (
    "unable to get manifest request code",
    "is not available",
    "not available for depot",
    "depot key",
    "invalid depot",
    "access denied",
    "no subscription",
    "login failure",
    "failed to login",
    "rate limit",
)


@dataclass(frozen=True)
class ToolSpec:
    """One entry of ``tools.lock.json``."""

    version: str
    platform: str
    executable: str
    url: str
    size: int
    sha256: str
    env: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class ManifestInfo:
    """What ``-manifest-only`` reported for one depot."""

    depot: str
    manifest: str
    date: str
    files: int
    bytes_on_disk: int
    entries: dict[str, dict[str, Any]]


def lock_path() -> Path:
    return paths.repo_root() / LOCK_RELATIVE


def read_lock() -> ToolSpec:
    """The pinned tool, or a usage error naming what is wrong with the lock."""
    path = lock_path()
    if not path.is_file():
        raise UsageError(f"no tool lock at {path}; Steam acquisition needs a pinned DepotDownloader")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        entry = document["tools"][TOOL_ID]
        return ToolSpec(
            version=str(entry["version"]),
            platform=str(entry["platform"]),
            executable=str(entry["executable"]),
            url=str(entry["url"]),
            size=int(entry["size"]),
            sha256=str(entry["sha256"]),
            env={str(k): str(v) for k, v in (entry.get("env") or {}).items()},
        )
    except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        raise UsageError(f"{path} does not pin '{TOOL_ID}' ({exc})") from exc


def ensure(cache: Path) -> Path:
    """The executable, downloaded and unpacked once, verified against the lock every time.

    ``TAKARO_MAINT_DEPOTDOWNLOADER`` replaces it, the way ``TAKARO_MAINT_DOCKER`` replaces
    docker: tests drive a stub through the same code path the real tool takes.
    """
    override = os.environ.get("TAKARO_MAINT_DEPOTDOWNLOADER")
    if override:
        exe = Path(override).expanduser()
        if not exe.is_file():
            raise UsageError(f"TAKARO_MAINT_DEPOTDOWNLOADER={override} is not a file")
        return exe

    spec = read_lock()
    # Keyed on what the lock pins, not only on the version: a re-pinned archive or another
    # platform's build cannot land on the same cache entry.
    installed = cache / "tools" / TOOL_ID / f"{spec.version}-{spec.platform}-{spec.sha256[:16]}"
    exe = installed / spec.executable
    if _cached_matches_lock(installed, exe, spec):
        return exe
    shutil.rmtree(installed, ignore_errors=True)

    # `net.download` refuses bytes that do not match the lock (exit 5) and an unreachable
    # release (exit 4); nothing below ever sees the wrong archive.
    blob = net.download(spec.url, net.Expectation(sha256=spec.sha256, size=spec.size), cache)
    staging = installed.parent / f".staging-{uuid.uuid4().hex}"
    staging.mkdir(parents=True, exist_ok=True)
    try:
        with zipfile.ZipFile(blob) as archive:
            for member in archive.namelist():
                # A zip may name any path it likes; the lock pins the bytes, not the layout.
                paths.safe_relative(member, field="DepotDownloader archive entry")
            archive.extractall(staging)
        extracted = staging / spec.executable
        if not extracted.is_file():
            raise UsageError(f"{spec.url} does not contain {spec.executable}")
        extracted.chmod(0o755)
        # The bytes that were unpacked from the locked archive, so a later run can tell the
        # cached executable apart from one that has been edited or half-replaced since.
        (staging / TOOL_MARKER).write_text(
            json.dumps(
                {
                    "version": spec.version,
                    "platform": spec.platform,
                    "archiveSha256": spec.sha256,
                    "executable": spec.executable,
                    "executableSha256": net.hash_file(extracted)["sha256"],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        installed.parent.mkdir(parents=True, exist_ok=True)
        os.replace(staging, installed)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    output.info(f"using DepotDownloader {spec.version} ({spec.platform})")
    return exe


def _cached_matches_lock(installed: Path, exe: Path, spec: ToolSpec) -> bool:
    """Is the executable already in the cache still the one the lock pins?

    Every run asks, because "it is there" is not the guarantee the lock makes: the bytes on
    disk are hashed against what was unpacked from the locked archive.
    """
    marker = installed / TOOL_MARKER
    if not exe.is_file() or not marker.is_file():
        return False
    try:
        recorded = json.loads(marker.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        recorded = {}
    if (
        recorded.get("archiveSha256") == spec.sha256
        and recorded.get("platform") == spec.platform
        and recorded.get("executableSha256") == net.hash_file(exe)["sha256"]
    ):
        return True
    output.warn(f"the cached DepotDownloader at {installed} does not match the lock; fetching it again")
    return False


def _hide(text: str, secrets: list[str]) -> str:
    """Hide this run's credentials, whatever their length, plus every secret in the env.

    ``redact.redact`` skips short values because it guesses at secrets by variable name.
    The values here are not guesses -- the target record named the variable they came from
    -- so a four-character branch password is hidden exactly like a long one.
    """
    for value in secrets:
        if value:
            text = text.replace(value, redact.PLACEHOLDER)
    return redact.redact(text)


def _classify(argv: list[str], completed: subprocess.CompletedProcess[str], log: Path, secrets: list[str]) -> None:
    """A non-zero DepotDownloader is always an upstream failure, never a fall back."""
    if completed.returncode == 0:
        return
    text = (completed.stdout or "") + (completed.stderr or "")
    lowered = text.lower()
    not_served = any(marker in lowered for marker in _NOT_SERVED_MARKERS)
    # Checked first: "depot key" also matches the success line printed just before the 401.
    reason = "manifest not served" if not_served else next((m for m in _UNAVAILABLE_MARKERS if m in lowered), None)
    tail = "\n".join(_hide(line, secrets) for line in text.strip().splitlines()[-20:])
    what = f"DepotDownloader exited {completed.returncode}"
    if reason:
        what += f" ({reason})"
    raise UpstreamUnavailable(
        f"{what}; {NO_FALLBACK}. See {log.name}:\n{tail}",
        argv=[_hide(part, secrets) for part in argv[1:]],
        **({"reason": MANIFEST_NOT_SERVED} if not_served else {}),
    )


def run(
    args: list[str],
    *,
    cwd: Path,
    log: Path,
    cache: Path,
    secrets: list[str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run the pinned tool, tee stdout+stderr into ``log`` and classify any failure."""
    exe = ensure(cache)
    spec_env = read_lock().env if not os.environ.get("TAKARO_MAINT_DEPOTDOWNLOADER") else {}
    argv = [str(exe), *args]
    cwd.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    hidden = list(secrets or [])
    output.debug("exec " + _hide(" ".join(argv), hidden))
    completed = subprocess.run(
        argv,
        cwd=str(cwd),
        env={**os.environ, **spec_env},
        capture_output=True,
        text=True,
        check=False,
    )
    with log.open("a", encoding="utf-8") as handle:
        handle.write(_hide(" ".join(argv[1:]), hidden) + "\n")
        handle.write(_hide((completed.stdout or "") + (completed.stderr or ""), hidden))
        handle.write(f"\n-- exit {completed.returncode}\n")
    _classify(argv, completed, log, hidden)
    return completed


def _parse_manifest_file(path: Path, depot: str) -> ManifestInfo:
    manifest = ""
    date = ""
    files = 0
    total = 0
    entries: dict[str, dict[str, Any]] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        found = _MANIFEST_ID.search(line)
        if found:
            manifest = found.group("id")
            date = found.group("date").strip()
            continue
        found = _TOTAL_FILES.search(line)
        if found:
            files = int(found.group("value"))
            continue
        found = _TOTAL_BYTES.search(line)
        if found:
            total = int(found.group("value"))
            continue
        row = _FILE_ROW.match(line)
        if row:
            entries[row.group("name").strip().replace("\\", "/")] = {
                "size": int(row.group("size")),
                "sha1": row.group("sha1"),
            }
    if not manifest:
        # The filename carries the id too: `manifest_<depot>_<id>.txt`.
        stem = path.stem.split("_")
        manifest = stem[-1] if stem else ""
    return ManifestInfo(depot=depot, manifest=manifest, date=date, files=files, bytes_on_disk=total, entries=entries)


def manifest_only(
    app: int,
    depot: str,
    branch: str,
    *,
    manifest: str | None,
    os_: str,
    arch: str,
    out: Path,
    cache: Path,
    log: Path,
    credentials: dict[str, str] | None = None,
) -> ManifestInfo:
    """Read one depot's manifest listing without downloading any content.

    ``out`` is a fresh temporary directory in both callers, so a listing that was already
    there when this started is not this run's -- it is a leftover, and answering with it
    would report the *old* manifest as the one Steam just served.
    """
    out.mkdir(parents=True, exist_ok=True)
    args = [
        "-app",
        str(app),
        "-depot",
        str(depot),
        "-branch",
        branch,
        "-os",
        os_,
        "-osarch",
        arch,
        "-manifest-only",
    ]
    if manifest:
        args += ["-manifest", str(manifest)]
    secrets = _apply_credentials(args, credentials)
    # DepotDownloader writes the listing under its own install directory
    # (``depots/<depot>/<manifest>/``) rather than next to the process, so the whole
    # output directory is searched rather than only its top level.
    before = {path for path in out.rglob("manifest_*.txt")}
    run(args, cwd=out, log=log, cache=cache, secrets=secrets)
    produced = sorted(path for path in out.rglob(f"manifest_{depot}_*.txt") if path not in before)
    if not produced:
        raise UpstreamUnavailable(
            f"DepotDownloader wrote no manifest listing for depot {depot}; {NO_FALLBACK}",
        )
    return _parse_manifest_file(produced[-1], str(depot))


def download(
    app: int,
    depot: str,
    manifest: str,
    *,
    branch: str,
    os_: str,
    arch: str,
    dir: Path,
    cache: Path,
    log: Path,
    filelist: list[str] | None = None,
    validate: bool = True,
    credentials: dict[str, str] | None = None,
) -> None:
    """Download exactly one depot manifest. A pinned manifest is never omitted."""
    if not manifest:
        raise UsageError(f"depot {depot} has no pinned manifest; {NO_FALLBACK}")
    dir.mkdir(parents=True, exist_ok=True)
    args = [
        "-app",
        str(app),
        "-depot",
        str(depot),
        "-manifest",
        str(manifest),
        "-branch",
        branch,
        "-os",
        os_,
        "-osarch",
        arch,
        "-dir",
        str(dir),
        "-max-downloads",
        "8",
    ]
    if validate:
        args.append("-validate")
    with tempfile.TemporaryDirectory(prefix="takaro-steam-filelist-") as tmp:
        if filelist:
            selector_file = Path(tmp) / "filelist.txt"
            selector_file.write_text("\n".join(filelist) + "\n", encoding="utf-8")
            args += ["-filelist", str(selector_file)]
        secrets = _apply_credentials(args, credentials)
        output.info(f"downloading depot {depot} manifest {manifest} (app {app}, branch {branch})")
        completed = run(args, cwd=dir, log=log, cache=cache, secrets=secrets)
    _assert_served(app, depot, str(manifest), dir=dir, completed=completed)


def _served_manifests(dir: Path, depot: str, completed: subprocess.CompletedProcess[str]) -> set[str]:
    """Every manifest id this run says it served, from the tool's own two records.

    It prints the manifest it resolved before downloading, and it leaves the manifest file
    it used in the download directory (``<depot>_<id>.manifest``, under ``.DepotDownloader``
    for the released builds). Both are read, because neither alone is guaranteed.
    """
    text = (completed.stdout or "") + (completed.stderr or "")
    served = {found.group("id") for found in _SERVED.finditer(text)}
    served |= {path.stem.split("_", 1)[1] for path in dir.rglob(f"{depot}_*.manifest") if "_" in path.stem}
    return served


def _assert_served(
    app: int,
    depot: str,
    manifest: str,
    *,
    dir: Path,
    completed: subprocess.CompletedProcess[str],
) -> None:
    """A zero exit is not the guarantee; being served the pinned manifest is.

    Whatever the tool would do on its own with a manifest it cannot get, this refuses to
    hand back a directory that holds anything but the requested one -- the caller labels
    its cache with that id, so branch-head content must never reach it.
    """
    served = _served_manifests(dir, depot, completed)
    if not served:
        raise UpstreamUnavailable(
            f"DepotDownloader recorded no manifest for depot {depot} of app {app}; "
            f"nothing proves {manifest} was served, {NO_FALLBACK}",
        )
    other = sorted(served - {manifest})
    if other:
        raise UpstreamUnavailable(
            f"DepotDownloader served depot {depot} manifest {', '.join(other)} where {manifest} was pinned; "
            f"{NO_FALLBACK}",
        )


def _apply_credentials(args: list[str], credentials: dict[str, str] | None) -> list[str]:
    """Append the credential arguments a target's record asks for, from the environment only."""
    if not credentials:
        return []
    if "branchPasswordEnv" in credentials:
        name = credentials["branchPasswordEnv"]
        value = os.environ.get(name)
        if not value:
            raise UsageError(f"this target's branch needs a password; set {name} in the environment")
        args += ["-branchpassword", value]
        return [value]
    if "accountEnv" in credentials:
        name = credentials["accountEnv"]
        value = os.environ.get(name)
        if not value:
            raise UsageError(f"this target needs a Steam account; set {name} in the environment")
        args += ["-username", value]
        return [value]
    return []
