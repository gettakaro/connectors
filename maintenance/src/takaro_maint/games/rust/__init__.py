"""The Rust adapter: a Steam-delivered server, a modding framework, and a source plugin.

Three things make this game its own shape. The server arrives as two Steam depots, so the
adapter owns its ``install`` the way 7D2D does. On top of it sits Carbon, a framework that
is *not* part of the depot: it is a GitHub release asset, unpacked into the same tree and
pinned by its own hash, so the install has a second half and the ledger has four extra
witnesses. And the artifact is a ``.cs`` file that Carbon compiles when it loads it, so the
release is source, the compile-check in CI is the authoritative build for the fingerprint,
and deploying means putting the file where the framework looks for it.

There is no image to build. The runtime container is a digest-pinned base image; what turns
it into a Rust server is the installed tree and the tracked ``games/rust/start.sh``, mounted
in and named as the container's command.
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import shutil
import subprocess
import tarfile
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from ... import net, output, paths
from ...exit_codes import OK, BuildFailed, ConflictError, IntegrityError
from ...install.ledger import check_ledger, ledger_path, read_ledger, write_ledger
from ...providers import provider_for
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env

GAME_ID = "rust"
REFERENCES_ROOT = "games/rust/_data/rust-binaries"
CARBON_REFERENCES_ROOT = "games/rust/_data/carbon-refs"
DIST_ROOT = "games/rust/_data/dist"
BUILD_SCRIPT = "games/rust/scripts/build-release.sh"
#: What the release script records beside each artifact. The generic build writes its own
#: ``<artifact>.meta.json`` over the script's, so this build's pins live under their own name.
PROVENANCE_SUFFIX = ".provenance.json"
LAUNCHER = "games/rust/start.sh"
ASSEMBLY_CSHARP = "RustDedicated_Data/Managed/Assembly-CSharp.dll"

#: Carbon and Oxide both load a plugin by its class-named file, so this is the only name
#: the framework will compile. The versioned artifact beside it is what the ledger records.
PLUGIN_FILE = "carbon/plugins/TakaroConnector.cs"
PLUGIN_PREFIX = "takaro-rust-plugin-"
CARBON_INPUT = "carbon"
CARBON_ARCHIVE = "takaro/Carbon.Linux.Release.tar.gz"
CARBON_CONFIG = "carbon/config.json"

#: What the ledger re-hashes to prove a boot did not self-update Carbon underneath us:
#: the two assemblies that are Carbon, the Doorstop loader that injects it, and the
#: environment script the launcher sources.
CARBON_WITNESSES = (
    "carbon/managed/Carbon.dll",
    "carbon/managed/Carbon.Common.dll",
    "libdoorstop.so",
    "carbon/tools/environment.sh",
)

#: DepotDownloader writes every file 0644: Steam's manifests carry no POSIX mode, so the
#: server binary and the shell scripts beside it arrive unrunnable. These are the ones
#: something actually executes. The mode is not part of any hash, so setting it here
#: changes nothing the ledger or `inputsIntact` guards.
EXECUTABLES = (
    "RustDedicated",
    "runds.sh",
    "carbon.sh",
    "carbon/tools/environment.sh",
)

#: Directories Carbon expects to exist, plus the ones this repository's launcher uses.
CARBON_DIRECTORIES = (
    "carbon/plugins",
    "carbon/configs",
    "carbon/data",
    "carbon/logs",
    "takaro",
    "takaro/home",
    "server",
)

#: Seeded only when Carbon has not written its own config yet. Carbon ships a self-updater
#: that replaces carbon/managed/*.dll on boot as soon as a newer production build exists,
#: which would silently unpin the framework half of this target.
#:
#: The shape is Carbon's own, read off a config the framework generated for itself
#: (``SelfUpdating`` is an object, not a flag). It matters more than it looks: Carbon's
#: preloader reads this file before it prints anything, and a document it cannot
#: deserialise takes the whole framework down without a word in the log -- the server then
#: boots perfectly, unmodded. Only the self-update keys are written; every other setting
#: stays at whatever the installed Carbon's own default is.
CARBON_CONFIG_SEED: dict[str, Any] = {
    "SelfUpdating": {"Enabled": False, "HookUpdates": False, "RedirectUri": None},
}

#: The runtime container is a plain base image; the game is the mounted tree and this script.
CONTAINER_COMMAND = ("/bin/bash", "/takaro/start.sh")

#: The runner boots every game with 3 GB. A verification world (size 1000, ten slots) does
#: pass inside that, but it sits *at* the ceiling (2.97-2.999 GiB of 3) through world
#: generation, living off page-cache reclaim, and on a busy host docker OOM-killed it (exit 137)
#: during ``Processing World``. 8 GB is the footprint a played server is sized
#: for and leaves half of a 16 GB hosted runner free; the rig's compose file sets no limit.
CONTAINER_MEMORY = "8g"

_PROTOCOL = re.compile(r"^Protocol:\s*(?P<protocol>[0-9][0-9.]*)")
_CARBON_BANNER = re.compile(r"Initialized Carbon\.Startup (?P<version>[0-9]+(?:\.[0-9]+)+)")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


def _carbon_spec(resolved: dict[str, Any]) -> dict[str, Any]:
    return dict(resolved["inputs"][CARBON_INPUT])


def _carbon_download_url(spec: dict[str, Any]) -> str:
    """github.com's own download URL for the asset, which serves the bytes to any client.

    The API's asset-id URL is the immutable coordinate for a re-uploaded tag, but it answers
    with JSON unless the request asks for ``application/octet-stream``, and neither
    ``catalog validate --online`` nor ``setup-environment.sh`` (both of which re-fetch this
    string and re-hash it) can ask for that. So the tag URL is what is recorded, and the pin
    that matters is the recorded sha256.

    The price is stated in the record's ``support.notes`` and is worth stating here too:
    Carbon re-uploads ``production_build`` in place, and on the day it does, this URL serves
    different bytes, the sha256 gate refuses them, and this target can no longer be installed
    from anything it records. Recovering it then means a new target pinning the new bytes --
    or teaching the shared ``github-release`` provider to fetch an asset-id URL with an
    ``Accept: application/octet-stream`` header, which is the only immutable coordinate
    GitHub offers and is not this adapter's to add.
    """
    return f"https://github.com/{spec['repo']}/releases/download/{spec['tag']}/{spec['asset']}"


def _safe_members(archive: tarfile.TarFile, root: Path) -> list[tarfile.TarInfo]:
    """Every entry of the archive, once it is certain that none of them escapes ``root``."""
    root_resolved = root.resolve()
    members = []
    for member in archive.getmembers():
        name = member.name.replace("\\", "/")
        segments = [segment for segment in name.split("/") if segment not in ("", ".")]
        if name.startswith("/") or any(segment == ".." for segment in segments):
            raise IntegrityError(f"the Carbon archive holds '{member.name}', which escapes the install directory")
        if member.islnk() or member.issym():
            # A hard link names a path relative to the archive root; a symlink names one
            # relative to its own directory. Resolving both the same way would let
            # `root-evil` pass as a prefix of `root`, so containment is a path
            # relationship, not a string one.
            base = root_resolved if member.islnk() else (root_resolved / name).parent
            link = base.joinpath(member.linkname.replace("\\", "/")).resolve()
            if not link.is_relative_to(root_resolved):
                raise IntegrityError(f"the Carbon archive links '{member.name}' outside the install directory")
        members.append(member)
    return members


def _source_record(catalog: Any, game_id: str, spec: dict[str, Any]) -> dict[str, Any]:
    """The game.json source this input names, as the provider expects to be handed it."""
    sources = catalog.game(game_id).record.get("sources") or {}
    source = dict(sources[str(spec["source"])])
    source.setdefault("id", str(spec["source"]))
    return source


def _carbon_is_stale(dest: Path, target: Any) -> bool:
    """Does a directory that claims this target really hold its Carbon half too?"""
    ledger = read_ledger(dest)
    if ledger is None or ledger.fingerprint != target.fingerprint:
        return False  # not this target at all; install_exact decides what to do with it
    rows = [row for row in ledger.data.get("inputs", []) if str(row.get("name", "")).startswith(f"{CARBON_INPUT}:")]
    if not rows:
        return True
    return bool(check_ledger(dest, target.record, target.fingerprint))


@contextlib.contextmanager
def _ledger_set_aside(dest: Path) -> Iterator[None]:
    """Make the Steam half's fast path miss, without destroying the install's identity.

    ``install_exact`` answers ``already-installed`` from the ledger alone, so a tree whose
    Carbon rows are stale has to be made to miss it. Deleting the ledger would do that, but
    a re-install that then fails -- a 404 on the Carbon asset, bytes that disagree with the
    recorded sha256, an archive that will not unpack -- leaves the old install byte-identical
    and unable to say what it is, so ``ledger check`` can no longer answer for a tree that is
    perfectly fine. The ledger is therefore moved *beside* the install (never inside it,
    where ``preserve`` would carry it into the next tree) and put back on any failure.
    """
    ledger = ledger_path(dest)
    if not ledger.is_file():
        yield
        return
    aside = dest.with_name(dest.name + ".stale-ledger")
    os.replace(ledger, aside)
    try:
        yield
    except BaseException:
        # `install_exact` puts the retired tree back without its ledger, because it was
        # already gone when the install started; this is what makes that tree whole again.
        _put_ledger_back(dest, aside, ledger)
        raise
    # A finished install has written its own ledger, and the stale one has nothing left to
    # say; anything else means the tree is still the one this ledger describes.
    if ledger.is_file():
        aside.unlink(missing_ok=True)
    else:
        _put_ledger_back(dest, aside, ledger)


def _put_ledger_back(dest: Path, aside: Path, ledger: Path) -> None:
    """Return the set-aside ledger to an install that is still there, or drop it."""
    if dest.is_dir():
        ledger.parent.mkdir(parents=True, exist_ok=True)
        os.replace(aside, ledger)
    else:
        # No install left to describe: a ledger on its own would claim one that is not there.
        aside.unlink(missing_ok=True)


def _install_carbon(staging: Path, spec: dict[str, Any], source: dict[str, Any], cache: Path) -> None:
    """The framework half, into the staged tree: verified bytes, then its own directories.

    Nothing here touches the live install. The archive is fetched (404 exits 4, wrong bytes
    exit 5) and unpacked inside the staging directory, which is only swapped into place once
    every declared file has been verified.
    """
    archive = staging / CARBON_ARCHIVE
    archive.parent.mkdir(parents=True, exist_ok=True)
    provider_for("github-release").fetch_input(spec, source, archive, cache)
    output.info(f"unpacking {spec['asset']} ({str(spec['sha256'])[:16]}\u2026) into the staged install")
    with tarfile.open(archive, "r:gz") as tar:
        # `_safe_members` has already refused anything that escapes; the `data` filter
        # is the second pair of eyes, and it is the strictest one the stdlib offers --
        # it refuses links, devices and absolute paths on its own. The pinned Carbon
        # archive is plain files and directories, so nothing it ships needs more.
        tar.extractall(staging, members=_safe_members(tar, staging), filter="data")  # noqa: S202
    for directory in CARBON_DIRECTORIES:
        (staging / directory).mkdir(parents=True, exist_ok=True)
    for relative in EXECUTABLES:
        path = staging / relative
        if path.is_file():
            path.chmod(path.stat().st_mode | 0o111)
    config = staging / CARBON_CONFIG
    if not config.is_file():
        # Never over an operator's own file: this seeds only the config Carbon has not
        # written yet. Whatever the config says, the ledger witnesses are the hard guard.
        config.write_text(json.dumps(CARBON_CONFIG_SEED, indent=2) + "\n", encoding="utf-8")


def _append_carbon_rows(dest: Path, spec: dict[str, Any]) -> list[dict[str, Any]]:
    """Record the Carbon archive and its witnesses, so `ledger check` guards them too.

    This is a second write of an already-valid ledger rather than part of the Steam module's
    protected window: a failure here leaves a valid, Steam-only ledger, which the next
    install recognises as stale and repairs.
    """
    ledger = read_ledger(dest)
    if ledger is None:
        raise ConflictError(f"{dest} has no ledger to record the Carbon half in")
    rows = list(ledger.data.get("inputs", []))
    rows.append(
        {
            "name": f"{CARBON_INPUT}:{CARBON_ARCHIVE}",
            "path": CARBON_ARCHIVE,
            "sha256": str(spec["sha256"]),
            "size": int(spec["size"]),
        }
    )
    for witness in CARBON_WITNESSES:
        digests = net.hash_file(dest / witness)
        rows.append(
            {
                "name": f"{CARBON_INPUT}:{witness}",
                "path": witness,
                "sha256": digests["sha256"],
                "size": int(digests["size"]),
            }
        )
    data = dict(ledger.data)
    data["inputs"] = rows
    write_ledger(dest, data)
    return rows


class RustAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the rig, the scripts and CI read. No ``_JAVA``: this server ships its own runtime."""
        server = resolved["inputs"]["server"]
        carbon = _carbon_spec(resolved)
        depots = ";".join(f"{depot}:{server['depots'][depot]['manifest']}" for depot in sorted(server["depots"]))
        env = {
            **common_env(resolved, prefix),
            f"{prefix}_TOOLCHAIN": str(resolved["toolchainRef"]),
            f"{prefix}_REVISION": str(resolved["revision"]),
            f"{prefix}_STEAM_APP": str(server["app"]),
            f"{prefix}_STEAM_BRANCH": str(server["branch"]),
            f"{prefix}_STEAM_BUILDID": str(server["buildid"]),
            f"{prefix}_STEAM_DEPOTS": depots,
            f"{prefix}_ARTIFACT": str(resolved["artifactFileNames"]["plugin"]),
            f"{prefix}_REFERENCES_DIR": f"{REFERENCES_ROOT}/{resolved['fp16']}",
            f"{prefix}_CARBON_REFERENCES_DIR": f"{CARBON_REFERENCES_ROOT}/{resolved['fp16']}",
            f"{prefix}_CARBON_ASSET": str(carbon["asset"]),
            f"{prefix}_CARBON_TAG": str(carbon["tag"]),
            f"{prefix}_CARBON_SHA256": str(carbon["sha256"]),
            f"{prefix}_CARBON_SIZE": str(carbon["size"]),
            f"{prefix}_CARBON_URL": str(resolved["resolvedUrls"].get(CARBON_INPUT, "")),
            f"{prefix}_CARBON_DOWNLOAD_URL": _carbon_download_url(carbon),
        }
        assembly = server["files"].get(ASSEMBLY_CSHARP, {}).get("sha256")
        if assembly:
            env[f"{prefix}_ASSEMBLY_CSHARP_SHA256"] = str(assembly)
        for name, dep in sorted(resolved["build"]["deps"].items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        """One half of the runtime identity, from whichever of the two lines this is.

        Rust writes its wire protocol (``Protocol: 2632.287.1``) and Carbon writes its own
        version (``Initialized Carbon.Startup 2.0.259.0``). Neither line carries the other's
        fact, so each returns only what it knows and the hooks merge the two.
        """
        protocol = _PROTOCOL.search(log_line.strip())
        if protocol:
            return {"gameVersion": protocol.group("protocol"), "loader": "carbon", "loaderVersion": None}
        carbon = _CARBON_BANNER.search(log_line)
        if carbon:
            version = carbon.group("version")
            # Carbon's banner is a four-part assembly version; its release is the first three.
            parts = version.split(".")
            return {
                "gameVersion": None,
                "loader": "carbon",
                "loaderVersion": ".".join(parts[:3]) if len(parts) > 3 else version,
            }
        return None

    # -- build ----------------------------------------------------------------
    def artifact_paths(self, resolved: dict[str, Any], version: str, repo_root: Path) -> dict[str, Path]:
        out = repo_root / DIST_ROOT / resolved["fp16"]
        return {
            component["role"]: out / component["artifact"].replace("{version}", version)
            for component in resolved["components"]
        }

    def build(
        self,
        resolved: dict[str, Any],
        version: str,
        out: Path,
        toolchain: str,
        repo_root: Path,
        gradle_args: list[str] | None = None,
        source_revision: str | None = None,
    ) -> BuildResult:
        """Run the tracked release script, which compile-checks in the pinned .NET SDK image.

        ``toolchain`` and ``gradle_args`` are accepted for parity with the Gradle games and
        change nothing: the host has no .NET SDK, and determinism here comes from
        ``SOURCE_DATE_EPOCH`` and a clean stage rather than from a task graph.

        ``out`` is the release directory the caller then copies the artifact into and writes
        its own generic ``<artifact>.meta.json`` in. The script's ``<artifact>.provenance.json``
        -- the Carbon pin, the depot manifests, the Assembly-CSharp hash and the toolchain
        digest this build actually used -- is carried there too, because nothing downstream
        would otherwise take it out of ``games/rust/_data/dist``.
        """
        del toolchain, gradle_args
        dist = repo_root / DIST_ROOT / resolved["fp16"]
        dist.mkdir(parents=True, exist_ok=True)
        environment = dict(os.environ)
        environment["TAKARO_MAINT_REPO_ROOT"] = str(repo_root)
        if source_revision:
            environment["TAKARO_SOURCE_REVISION"] = source_revision
        if "SOURCE_DATE_EPOCH" not in environment:
            stamp = subprocess.run(
                ["git", "-C", str(repo_root), "log", "-1", "--format=%ct"],
                capture_output=True,
                text=True,
                check=False,
            )
            if stamp.returncode == 0 and stamp.stdout.strip().isdigit():
                environment["SOURCE_DATE_EPOCH"] = stamp.stdout.strip()
        command = ["bash", str(repo_root / BUILD_SCRIPT), version, str(dist), "--target", str(resolved["id"])]
        output.info(f"building {resolved['id']} {version} (dotnet SDK container compile-check)")
        completed = subprocess.run(
            command, cwd=str(repo_root), capture_output=True, text=True, env=environment, check=False
        )
        log = completed.stdout + completed.stderr
        if completed.returncode != 0:
            output.error(log[-8000:])
            raise BuildFailed(f"{BUILD_SCRIPT} exited {completed.returncode}", target=str(resolved["id"]))
        artifacts = self.artifact_paths(resolved, version, repo_root)
        out.mkdir(parents=True, exist_ok=True)
        for produced in artifacts.values():
            provenance = produced.with_name(produced.name + PROVENANCE_SUFFIX)
            if provenance.is_file():
                shutil.copy2(provenance, out / provenance.name)
        return BuildResult(artifacts=artifacts, log=log)

    # -- runtime --------------------------------------------------------------
    def runtime_env(self, resolved: dict[str, Any], takaro: dict[str, str]) -> dict[str, str]:
        """The launcher's arguments and the connector's configuration, both from the environment."""
        return {
            **{str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()},
            # RCON is not published to the host; this only has to be set for the server to start.
            "RCON_PASSWORD": "takaro-verify",
            "HOME": "/rust/takaro/home",
            **takaro,
        }

    def container_mounts(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """The installed tree, plus the tracked launcher the container runs."""
        del resolved
        return [
            f"{data_dir}:/rust",
            f"{paths.repo_root() / LAUNCHER}:/takaro/start.sh:ro",
        ]

    def container_options(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """Rust's own memory limit, which wins over the runner's because it comes last."""
        del resolved, data_dir
        return ["--memory", CONTAINER_MEMORY]

    def container_command(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """The base image has no entrypoint of its own; the tracked launcher is the server."""
        del resolved, data_dir
        return list(CONTAINER_COMMAND)

    # -- install --------------------------------------------------------------
    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        """Two pinned depots plus the pinned Carbon archive, or nothing at all."""
        dest = Path(args.dest).expanduser().resolve()
        cache = paths.cache_dir()
        log = cache / "steam" / "logs" / f"{target.game}-{target.id}.log"

        if getattr(args, "rollback", False):
            output.emit("install", True, **steam_install.rollback(target, dest=dest))
            return OK

        if getattr(args, "reuse_world", False) or getattr(args, "fresh_world", False):
            output.info("Rust keeps its saves in server/<identity>, which is preserved; the world flags change nothing")

        spec = _carbon_spec(resolved)
        source = _source_record(catalog, target.game, spec)
        dry_run = bool(getattr(args, "dry_run", False))

        def post_install(staging: Path) -> None:
            _install_carbon(staging, spec, source, cache)

        # The Steam half answers `already-installed` out of the declared depot files alone.
        # Carbon is not in the depot, so its rows are checked here before that answer is
        # trusted; a stale one falls through to a full install, which re-runs post_install.
        stale = not dry_run and _carbon_is_stale(dest, target)
        if stale:
            output.info("the Carbon half of this install is missing or stale; installing it again")
        with _ledger_set_aside(dest) if stale else contextlib.nullcontext():
            document = steam_install.install_exact(
                target,
                dest=dest,
                preserve=self.preserve_globs(resolved),
                cache=cache,
                log=log,
                dry_run=dry_run,
                post_install=post_install,
            )
        if document["status"] == "installed":
            document["inputs"] = _append_carbon_rows(dest, spec)
        output.emit("install", True, **document)
        return OK

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """Carbon loads a plugin by its class-named file, so the artifact is copied onto it.

        The versioned file in ``takaro/`` stays as the ledger's record of what was deployed;
        ``carbon/plugins/TakaroConnector.cs`` is what the framework actually compiles.
        """
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        plugin = dest / PLUGIN_FILE
        plugin.parent.mkdir(parents=True, exist_ok=True)
        staged = plugin.with_suffix(".cs.tmp")
        shutil.copyfile(artifact, staged)
        os.replace(staged, plugin)
        for stale in sorted(install_dir.glob(f"{PLUGIN_PREFIX}*.cs")):
            if stale.name != artifact.name:
                stale.unlink()
        found = re.search(
            r'\[Info\("TakaroConnector",\s*"Takaro",\s*"([^"]+)"\)\]',
            artifact.read_text(encoding="utf-8", errors="replace"),
        )
        if found:
            output.info(f"loaded TakaroConnector {found.group(1)} as {PLUGIN_FILE}")


GAME = RustAdapter()
