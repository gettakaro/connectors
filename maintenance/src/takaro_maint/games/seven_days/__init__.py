"""The 7 Days to Die adapter: a Steam-delivered server and a Mono-built mod folder.

Nothing here is Minecraft-shaped. The server arrives as Steam depots rather than a jar,
so this adapter owns its own ``install``; the mod is built by a tracked shell script in
a pinned Mono container; and the artifact is a zip whose folder the server loads, so
deploying it means unpacking it where the file lands.
"""

from __future__ import annotations

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

GAME_ID = "7d2d"
REFERENCES_ROOT = "games/7d2d/_data/7dtd-binaries"
DIST_ROOT = "games/7d2d/_data/dist"
BUILD_SCRIPT = "games/7d2d/scripts/build-release.sh"
ASSEMBLY_CSHARP = "7DaysToDieServer_Data/Managed/Assembly-CSharp.dll"

# What this server actually writes about itself, in the order it writes it:
#   "INF Last played version: V 3.2.0"
#   "GamePref.GameVersion = V 3.2.0"
# Both are anchored on their own prefix so that a line about some *other* version -- the
# world file's, say ("Loaded world file from different version: 'V 4.0 (b8)'") -- is not
# mistaken for the server's. The build number is not in either line; the exact build is
# proven by the installed bytes, not by a log line.
_VERSION_BANNERS = (
    re.compile(r"INF Last played version:\s*V\s*(?P<version>[0-9][0-9.]*)(?:\s*\((?P<build>b[0-9]+)\))?"),
    re.compile(r"GamePref\.GameVersion\s*=\s*V\s*(?P<version>[0-9][0-9.]*)(?:\s*\((?P<build>b[0-9]+)\))?"),
)

# The mod folder the server loads, and the only path an artifact zip may write to.
MOD_FOLDER = "Takaro"
#: Where an archive is unpacked before it replaces the live mod folder.
MOD_STAGE = ".Takaro.staging"

#: The runner boots every game with 3 GB. An idle V 3.3.0 server on Navezgane already holds
#: 2.3 GB on the rig, so the default leaves it no headroom while the world loads; 6 GB keeps
#: it clear of the ceiling and leaves most of a 16 GB hosted runner free.
CONTAINER_MEMORY = "6g"

DONT_REMOVE = (
    "Managed by takaro-maint: this directory holds an exactly pinned Steam build.\n"
    "Its presence stops the image reinstalling the branch head with SteamCMD.\n"
)


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class SevenDaysAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the rig, the scripts and CI read. No ``_JAVA``: this server ships its own runtime."""
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
            f"{prefix}_ARTIFACT": str(resolved["artifactFileNames"]["server-mod"]),
            f"{prefix}_REFERENCES_DIR": f"{REFERENCES_ROOT}/{resolved['fp16']}",
        }
        assembly = server["files"].get(ASSEMBLY_CSHARP, {}).get("sha256")
        if assembly:
            env[f"{prefix}_ASSEMBLY_CSHARP_SHA256"] = str(assembly)
        # The dependency URLs and hashes the deps build verifies before it uses them.
        for name, dep in sorted(resolved["build"]["deps"].items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        """The server's own version line, when it writes one."""
        for banner in _VERSION_BANNERS:
            match = banner.search(log_line)
            if not match:
                continue
            build = match.group("build")
            version = match.group("version")
            return {
                "gameVersion": f"{version}.{build}" if build else version,
                "loader": "mono",
                "loaderVersion": None,
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
        """Run the tracked release script. The mod always compiles in the pinned Mono image.

        ``toolchain`` is accepted for parity with the Gradle games but changes nothing: the
        host has no Mono, so ``host`` would be a promise this adapter cannot keep.
        ``gradle_args`` (``--rerun-tasks`` from the release workflow) mean nothing to a
        script build; determinism comes from ``SOURCE_DATE_EPOCH`` and a clean stage.
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
        output.info(f"building {resolved['id']} {version} (mono container toolchain)")
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
        """The image's own environment. The mod reads Takaro/Config.xml, never the environment."""
        del takaro
        return {
            **{str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()},
            "PUID": str(os.getuid()),
            "PGID": str(os.getgid()),
        }

    def container_mounts(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """Server files, saves and logs. Saves and logs live under .takaro, which is preserved."""
        del resolved
        runtime = data_dir / ".takaro" / "runtime"
        (runtime / "saves").mkdir(parents=True, exist_ok=True)
        (runtime / "log").mkdir(parents=True, exist_ok=True)
        return [
            f"{data_dir}:/home/sdtdserver/serverfiles",
            f"{runtime / 'saves'}:/home/sdtdserver/.local/share/7DaysToDie",
            f"{runtime / 'log'}:/home/sdtdserver/log",
        ]

    def container_options(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """This server's own memory limit, which wins over the runner's because it comes last."""
        del resolved, data_dir
        return ["--memory", CONTAINER_MEMORY]

    # -- install --------------------------------------------------------------
    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        """This game's whole installation is one Steam depot set, so the adapter owns it."""
        del catalog
        dest = Path(args.dest).expanduser().resolve()
        cache = paths.cache_dir()
        log = cache / "steam" / "logs" / f"{target.game}-{target.id}.log"

        if getattr(args, "rollback", False):
            document = steam_install.rollback(target, dest=dest)
        else:
            if getattr(args, "reuse_world", False) or getattr(args, "fresh_world", False):
                output.info("7D2D keeps its saves outside the install directory; the world flags change nothing")
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
        """What a fresh Steam tree still needs before the image will run it."""
        (staging / "DONT_REMOVE.txt").write_text(DONT_REMOVE, encoding="utf-8")
        server_config = staging / "serverconfig.xml"
        rig_config = staging / "sdtdserver.xml"
        if server_config.is_file() and not rig_config.is_file():
            # A real file, never a link into the depot cache: LinuxGSM rewrites it in place.
            shutil.copyfile(server_config, rig_config)
        (staging / MOD_FOLDER).mkdir(exist_ok=True)
        (staging / "Mods").mkdir(exist_ok=True)

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """The server loads ``Mods/Takaro/``, so the zip is unpacked where it lands."""
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        folder = install_dir / MOD_FOLDER
        # Staged, not extracted over the live folder: deleting the mod first would let an
        # archive that fails halfway take the working install with it.
        stage = install_dir / MOD_STAGE
        shutil.rmtree(stage, ignore_errors=True)
        try:
            with open_zip(artifact) as archive:
                names = archive.namelist()
                for name in names:
                    relative = name.rstrip("/")
                    if not relative:
                        continue
                    if not relative.startswith(f"{MOD_FOLDER}/"):
                        raise ConflictError(
                            f"{artifact.name} holds '{name}', outside the single {MOD_FOLDER}/ folder; "
                            "nothing was extracted"
                        )
                    paths.safe_relative(relative, field="artifact zip entry")
                required = (f"{MOD_FOLDER}/ModInfo.xml", f"{MOD_FOLDER}/Takaro.dll")
                missing = [name for name in required if name not in names]
                if missing:
                    raise ConflictError(
                        f"{artifact.name} is missing {', '.join(missing)}; the installed mod is untouched"
                    )
                archive.extractall(stage)
            replace_directory(stage / MOD_FOLDER, folder, subject=f"{MOD_FOLDER}/")
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        for stale in sorted(install_dir.glob("takaro-7d2d-mod-*.zip")):
            if stale.name != artifact.name:
                stale.unlink()
        mod_info = folder / "ModInfo.xml"
        if mod_info.is_file():
            found = re.search(r'<Version\s+value="([^"]+)"', mod_info.read_text(encoding="utf-8", errors="replace"))
            if found:
                output.info(f"unpacked {MOD_FOLDER} {found.group(1)} into {component['installDir']}/")


GAME = SevenDaysAdapter()
