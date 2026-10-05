"""The Conan Exiles adapter: a Steam-delivered UE5 server with the native connector inside it.

The dedicated server arrives as Steam depots and stays untouched. The connector is a native
library loaded into the server process (``LD_PRELOAD`` on Linux, a ``winmm.dll`` proxy on
Windows), shipped as one zip per platform target. The image that runs the server is not a
game image at all -- the Enhanced Linux build needs nothing but glibc, libstdc++ and libgcc,
so the pinned Node image the target records serves as the server runtime, and the server is
started by a command rather than by an entrypoint.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import OK, BuildFailed, ConflictError
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env, open_zip, replace_directory

GAME_ID = "conan-exiles"
DIST_ROOT = "games/conan-exiles/_data/dist"
BUILD_SCRIPT = "games/conan-exiles/scripts/build-release.sh"

#: The one folder an artifact zip may write to, and the binary one of which it must carry
#: (Linux, Windows).
NATIVE_FOLDER = "TakaroConanNative"
NATIVE_BINARIES = ("libtakaro-conan-native.so", "winmm.dll")

LAUNCHER = "ConanSandboxServer.sh"
SERVER_BINARY = "ConanSandbox/Binaries/Linux/ConanSandboxServer-Linux-Shipping"

CONTAINER_ROOT = "/conan"
GAME_PORT = 7777
QUERY_PORT = 27015

#: Observed resident set of a running Enhanced server is ~10 GB; the runner's own 3g cap
#: would kill it during map load, so this game names its own.
MEMORY = "14g"

# What the server prints about itself in its first hundred lines:
#   "LogInit: Build: ++exiles+release-CL-373655"
#   "LogInit: Engine Version: 5.6.1-373655+++exiles+release"
# The first names the game build, the second the engine; neither carries the other, so
# both are parsed and `verify.scan_runtime_identity` merges them.
_BUILD_BANNER = re.compile(r"LogInit: Build: \+\+(?P<cl>exiles\+release-CL-\d+)")
_ENGINE_BANNER = re.compile(r"LogInit: Engine Version: (?P<engine>\d+\.\d+\.\d+)-\d+\+\+\+exiles\+release")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class ConanExilesAdapter(BaseAdapter):
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
            f"{prefix}_PLATFORM": str(resolved["platform"]),
        }
        # One key per shipped component (``_ARTIFACT_NATIVE``).
        for role, name in sorted(resolved["artifactFileNames"].items()):
            env[f"{prefix}_ARTIFACT_{_env_key(role)}"] = str(name)
        declared_hashes = (
            (f"{prefix}_LAUNCHER_SHA256", LAUNCHER),
            (f"{prefix}_SERVER_BINARY_SHA256", SERVER_BINARY),
        )
        for key, declared in declared_hashes:
            digest = server["files"].get(declared, {}).get("sha256")
            if digest:
                env[key] = str(digest)
        # The pinned dependency URLs and hashes (the Windows build downloads zig from these).
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
            return {"gameVersion": build.group("cl"), "loader": "unreal", "loaderVersion": None}
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
        """Run the tracked release script; it always builds inside pinned images.

        ``toolchain`` is accepted for parity with the Gradle games and changes nothing:
        the host is not assumed to have a compiler, so ``host`` would be a promise this
        adapter cannot keep. ``gradle_args`` mean nothing to a script build; determinism comes from
        ``SOURCE_DATE_EPOCH`` and a clean stage.
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
        output.info(f"building {resolved['id']} {version} (container toolchain)")
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
        """The image's own environment; the server reads its settings from its own Saved/ tree."""
        del takaro
        return {str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()}

    def container_mounts(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """One mount: the whole install, where the launcher and the server's Saved/ tree live."""
        del resolved
        (data_dir / ".takaro" / "home").mkdir(parents=True, exist_ok=True)
        (data_dir / "ConanSandbox" / "Saved" / "Logs").mkdir(parents=True, exist_ok=True)
        return [f"{data_dir}:{CONTAINER_ROOT}"]

    def container_options(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """More memory than the runner's default, and the host user: this image has no game user.

        Docker takes the last ``--memory``, so the runner's own cap is overridden rather
        than fought with; ``--user`` makes the server write its world and logs as the
        caller, which is who owns the data dir the install wrote.
        """
        del resolved, data_dir
        return ["--memory", MEMORY, "--user", f"{os.getuid()}:{os.getgid()}"]

    def container_command(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """The launcher, with the rig's game and query ports. No RCON: the verifier does not use it."""
        del resolved, data_dir
        return [
            f"{CONTAINER_ROOT}/{LAUNCHER}",
            "-log",
            "-server",
            "-nosteamclient",
            f"-Port={GAME_PORT}",
            f"-QueryPort={QUERY_PORT}",
        ]

    # -- install --------------------------------------------------------------
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
                    "Conan Exiles generates its world inside ConanSandbox/Saved/, which is preserved; "
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
        output.emit("install", True, **document)
        return OK

    def _post_install(self, staging: Path) -> None:
        """What a fresh depot tree still needs before the launcher will run.

        The depot carries no executable bit on the shell launcher, and the server creates
        ``ConanSandbox/Saved`` itself only when it can -- which it cannot when the tree is
        owned by the install rather than by the container's user.
        """
        launcher = staging / LAUNCHER
        if launcher.is_file():
            launcher.chmod(0o755)
        (staging / "ConanSandbox" / "Saved").mkdir(parents=True, exist_ok=True)

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """The zip is one folder, unpacked into the component's ``installDir``.

        The native connector lands as ``TakaroConanNative/``: the library the server preloads,
        or the ``winmm.dll`` an operator copies next to the server exe.
        """
        self._unpack(
            dest,
            component,
            artifact,
            folder=NATIVE_FOLDER,
            required=(f"{NATIVE_FOLDER}/takaro-target.json",),
            required_any=tuple(f"{NATIVE_FOLDER}/{name}" for name in NATIVE_BINARIES),
            stale_glob="takaro-conan-exiles-native-*.zip",
        )

    def _unpack(
        self,
        dest: Path,
        component: dict[str, Any],
        artifact: Path,
        *,
        folder: str,
        required: tuple[str, ...],
        required_any: tuple[str, ...],
        stale_glob: str,
    ) -> None:
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        target_dir = install_dir / folder
        # Staged, not extracted over the live folder: deleting the folder first would let
        # an archive that fails halfway take the working install with it.
        stage = install_dir / f".{folder}.staging"
        shutil.rmtree(stage, ignore_errors=True)
        try:
            with open_zip(artifact) as archive:
                names = archive.namelist()
                for name in names:
                    relative = name.rstrip("/")
                    if not relative:
                        continue
                    # The folder's own entry is the one name that is the folder rather than
                    # a path inside it; a deterministic zip written by CPython carries it.
                    if relative != folder and not relative.startswith(f"{folder}/"):
                        raise ConflictError(
                            f"{artifact.name} holds '{name}', outside the single {folder}/ folder; "
                            "nothing was extracted"
                        )
                    paths.safe_relative(relative, field="artifact zip entry")
                missing = [name for name in required if name not in names]
                if required_any and not any(name in names for name in required_any):
                    missing.append(" or ".join(required_any))
                if missing:
                    raise ConflictError(
                        f"{artifact.name} is missing {', '.join(missing)}; the installed {folder}/ is untouched"
                    )
                archive.extractall(stage)
            replace_directory(stage / folder, target_dir, subject=f"{folder}/")
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        for stale in sorted(install_dir.glob(stale_glob)):
            if stale.name != artifact.name:
                stale.unlink()
        stamp = target_dir / "takaro-target.json"
        if stamp.is_file():
            try:
                data = json.loads(stamp.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                data = {}
            version = data.get("connectorVersion")
            if version:
                output.info(f"unpacked {folder} {version} into {component['installDir']}/")


GAME = ConanExilesAdapter()
