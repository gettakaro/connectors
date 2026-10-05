"""The RuneScape: Dragonwilds adapter: one native LD_PRELOAD plugin that talks to Takaro itself.

The dedicated server is a plain Steam depot install -- one binary, its `.sym` symbol
table, a launcher script -- so it installs through the shared Steam exact-install path,
the way Conan Exiles does. There is no framework layer and nothing here to compile the
plugin against: it resolves the game's functions from the depot's own `.sym` file at
runtime rather than linking against server code, so `build.references` names nothing.

One artifact ships. The plugin is loaded with `LD_PRELOAD` onto the game binary (never
onto SteamCMD, which is 32-bit and fails outright with a 64-bit preload) and holds the
outbound Takaro WebSocket, the durable event outbox, timed bans and the log tail itself.
There is no sidecar.

The plugin is built in an image made from the pinned node:*-bookworm toolchain (g++ on
the same glibc as the dedicated-server image -- a `.so` linked against a newer glibc could
not be preloaded into the game binary) plus the four static libraries the target pins by
source archive hash in `build.deps`.

No runtime container exists for this game in the catalog: the operator supplies their own
dedicated server image, and the repository's `dev-servers/` rig is a separate, manual
harness. So this adapter defines no `container_mounts`/`container_options`/
`container_command`, and the target names `contract` as its required verification level
with runtime proof tracked as a follow-up, the same posture Dune: Awakening and Conan
Exiles both took at this stage.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tarfile
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import OK, BuildFailed, ConflictError
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env

GAME_ID = "dragonwilds"
DIST_ROOT = "games/dragonwilds/_data/dist"
BUILD_SCRIPT = "games/dragonwilds/scripts/build-release.sh"

#: The single top-level folder each role's archive may write, and the folder the operator
#: ends up using.
ROLE_FOLDER = {"plugin": "TakaroDragonwilds"}

LAUNCHER = "RSDragonwildsServer.sh"
SERVER_BINARY = "RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping"

# What the server prints about itself while it comes up (UE 5.6.1, real output in
# mod/docs/API.md): "LogInit: Build: ++dominion+staging-CL-240163" and
# "LogInit: Engine Version: 5.6.1-240163+++dominion+staging". Neither banner carries the
# other's number, so both are parsed and the verify runner merges them -- the same split
# every Unreal game here has.
_BUILD_BANNER = re.compile(r"LogInit: Build: \+\+(?P<build>dominion\+\w+-CL-\d+)")
_ENGINE_BANNER = re.compile(r"LogInit: Engine Version: (?P<engine>\d+\.\d+\.\d+)-\d+\+\+\+dominion\+\w+")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class DragonwildsAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the rig, the build script and CI read. No ``_JAVA``: nothing here is a JVM."""
        server = resolved["inputs"]["server"]
        depots = ";".join(f"{depot}:{server['depots'][depot]['manifest']}" for depot in sorted(server["depots"]))
        env = {
            **common_env(resolved, prefix),
            f"{prefix}_TOOLCHAIN": str(resolved["toolchainRef"]),
            f"{prefix}_REVISION": str(resolved["revision"]),
            f"{prefix}_STEAM_APP": str(server["app"]),
            f"{prefix}_STEAM_BRANCH": str(server["branch"]),
            f"{prefix}_STEAM_BUILDID": str(server["buildid"]),
            f"{prefix}_STEAM_DEPOTS": depots,
        }
        declared_hashes = (
            (f"{prefix}_SERVER_BINARY_SHA256", SERVER_BINARY),
            (f"{prefix}_LAUNCHER_SHA256", LAUNCHER),
        )
        for key, declared in declared_hashes:
            digest = server["files"].get(declared, {}).get("sha256")
            if digest:
                env[key] = str(digest)
        # One key per role, so no script spells an artifact name out itself.
        for role, name in sorted(resolved["artifactFileNames"].items()):
            env[f"{prefix}_ARTIFACT_{_env_key(role)}"] = str(name)
        for component in resolved["components"]:
            env[f"{prefix}_INSTALL_DIR_{_env_key(str(component['role']))}"] = str(component["installDir"])
        # The native source archives the builder image downloads and hash-checks.
        for name, dep in sorted(resolved["build"]["deps"].items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        """The build or the engine version, from whichever banner this line is."""
        build = _BUILD_BANNER.search(log_line)
        if build:
            return {"gameVersion": build.group("build"), "loader": "unreal", "loaderVersion": None}
        engine = _ENGINE_BANNER.search(log_line)
        if engine:
            return {"gameVersion": None, "loader": "unreal", "loaderVersion": engine.group("engine")}
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
        """Run the tracked release script; it always builds inside the pinned image.

        ``toolchain`` is accepted for parity with the Gradle games and changes nothing:
        the host is not assumed to have a C++ toolchain or the pinned static libraries, so
        ``host`` would be a promise this adapter cannot keep. ``gradle_args`` mean nothing to a script build;
        determinism comes from ``SOURCE_DATE_EPOCH`` and a clean stage.
        """
        del toolchain, gradle_args
        dist = repo_root / DIST_ROOT / resolved["fp16"]
        dist.mkdir(parents=True, exist_ok=True)
        # A half-finished build must not be able to report over yesterday's bytes.
        for artifact in self.artifact_paths(resolved, version, repo_root).values():
            artifact.unlink(missing_ok=True)
            artifact.with_suffix(artifact.suffix + ".meta.json").unlink(missing_ok=True)
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
        output.info(f"building {resolved['id']} {version} (pinned Bookworm builder image)")
        completed = subprocess.run(
            command, cwd=str(repo_root), capture_output=True, text=True, env=environment, check=False
        )
        log = completed.stdout + completed.stderr
        if completed.returncode != 0:
            output.error(log[-8000:])
            raise BuildFailed(f"{BUILD_SCRIPT} exited {completed.returncode}", target=str(resolved["id"]))
        del out
        return BuildResult(artifacts=self.artifact_paths(resolved, version, repo_root), log=log)

    # -- runtime --------------------------------------------------------------
    def runtime_env(self, resolved: dict[str, Any], takaro: dict[str, str]) -> dict[str, str]:
        """The pinned image's own environment, and nothing of Takaro's.

        No runtime container exists for this game yet (see the module docstring), so a
        verification run is not attempted here; this is the honest minimum rather than a
        boot recipe.
        """
        del takaro
        return {str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()}

    # -- install ----------------------------------------------------------------
    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        """The whole installation is one Steam depot set, so the adapter owns it."""
        del catalog
        dest = Path(args.dest).expanduser().resolve()
        cache = paths.cache_dir()
        log = cache / "steam" / "logs" / f"{target.game}-{target.id}.log"

        if getattr(args, "rollback", False):
            document = steam_install.rollback(target, dest=dest)
        else:
            if getattr(args, "reuse_world", False) or getattr(args, "fresh_world", False):
                output.info(
                    "Dragonwilds generates its world inside RSDragonwilds/Saved/, which is preserved; "
                    "the world flags change nothing"
                )
            document = steam_install.install_exact(
                target,
                dest=dest,
                preserve=self.preserve_globs(resolved),
                cache=cache,
                log=log,
                dry_run=bool(getattr(args, "dry_run", False)),
                post_install=self._post_install,
            )
            if document.get("status") == "already-installed" and not getattr(args, "dry_run", False):
                # The fast path compares hashes only, so a tree installed before the executable
                # bits were restored would otherwise stay unable to start.
                document["executablesRestored"] = steam_install.restore_executables(dest)
        output.emit("install", True, **document)
        return OK

    def _post_install(self, staging: Path) -> None:
        """What a fresh depot tree still needs before the launcher will run."""
        steam_install.restore_executables(staging)
        launcher = staging / LAUNCHER
        if launcher.is_file():
            launcher.chmod(0o755)
        (staging / "RSDragonwilds" / "Saved").mkdir(parents=True, exist_ok=True)

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """Unpack one role's archive into its install dir, replacing the folder atomically."""
        role = str(component["role"])
        folder_name = ROLE_FOLDER.get(role)
        if folder_name is None:
            raise ConflictError(f"no folder is defined for the '{role}' role; nothing was extracted")
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        install_dir.mkdir(parents=True, exist_ok=True)
        folder = install_dir / folder_name

        with tarfile.open(artifact, "r:gz") as archive:
            members = archive.getmembers()
            for member in members:
                relative = member.name.rstrip("/")
                if not relative or relative == ".":
                    continue
                if relative != folder_name and not relative.startswith(f"{folder_name}/"):
                    raise ConflictError(
                        f"{artifact.name} holds '{member.name}', outside the single {folder_name}/ "
                        "folder; nothing was extracted"
                    )
                paths.safe_relative(relative, field="artifact archive entry")
                if member.issym() or member.islnk():
                    raise ConflictError(f"{artifact.name} holds a link entry '{member.name}'; nothing was extracted")
            shutil.rmtree(folder, ignore_errors=True)
            # Every entry was checked above; `filter="data"` is the second lock, not the first.
            archive.extractall(install_dir, members=members, filter="data")

        # An older artifact for the same role must not stay behind and look installed.
        stem = artifact.name.split("-linux-")[0]
        for stale in sorted(install_dir.glob(f"{stem}-*.tar.gz")):
            if stale.name != artifact.name:
                stale.unlink()

        stamp = folder / "takaro-target.json"
        if stamp.is_file():
            try:
                data = json.loads(stamp.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                data = {}
            version = data.get("connectorVersion")
            if version:
                output.info(f"unpacked {folder_name} {version} into {component['installDir']}/")


GAME = DragonwildsAdapter()
