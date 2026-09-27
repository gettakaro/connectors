"""Exactly pinned native ARK Linux server plus a Generic WebSocket sidecar."""

from __future__ import annotations

import hashlib
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

GAME_ID = "ark"
DIST_ROOT = "games/ark/_data/dist"
BUILD_SCRIPT = "games/ark/scripts/build-release.sh"
SERVER_BINARY = "ShooterGame/Binaries/Linux/ShooterGameServer"
SERVER_ROOT = "/ark"
FOLDERS = {"server-plugin": "TakaroArkNative", "sidecar": "TakaroArkSidecar"}
_VERSION = re.compile(r"(?:ARK server version|ShooterGameServer Version)\s*[:=]\s*(?P<version>[0-9.]+)", re.I)


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class ArkAdapter(BaseAdapter):
    id = GAME_ID

    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        server = resolved["inputs"]["server"]
        depots = ";".join(f"{key}:{server['depots'][key]['manifest']}" for key in sorted(server["depots"]))
        env = {
            **common_env(resolved, prefix),
            f"{prefix}_TOOLCHAIN": str(resolved["toolchainRef"]),
            f"{prefix}_REVISION": str(resolved["revision"]),
            f"{prefix}_STEAM_APP": str(server["app"]),
            f"{prefix}_STEAM_BRANCH": str(server["branch"]),
            f"{prefix}_STEAM_BUILDID": str(server["buildid"]),
            f"{prefix}_STEAM_DEPOTS": depots,
            f"{prefix}_SERVER_EXE_SHA256": str(server["files"][SERVER_BINARY]["sha256"]),
        }
        for role, artifact in resolved["artifactFileNames"].items():
            env[f"{prefix}_ARTIFACT_{_env_key(role)}"] = str(artifact)
        for name, dependency in resolved["build"]["deps"].items():
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dependency.get("resolvedCoordinate", dependency["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dependency["sha256"])
        return env

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
        del out, toolchain, gradle_args
        artifacts = self.artifact_paths(resolved, version, repo_root)
        artifacts["server-plugin"].parent.mkdir(parents=True, exist_ok=True)
        for artifact in artifacts.values():
            artifact.unlink(missing_ok=True)
            artifact.with_name(artifact.name + ".meta.json").unlink(missing_ok=True)
        env = dict(os.environ)
        env["TAKARO_MAINT_REPO_ROOT"] = str(repo_root)
        if source_revision:
            env["TAKARO_SOURCE_REVISION"] = source_revision
        proc = subprocess.run(
            [
                "bash",
                str(repo_root / BUILD_SCRIPT),
                version,
                str(artifacts["server-plugin"].parent),
                "--target",
                str(resolved["id"]),
            ],
            cwd=repo_root,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        log = proc.stdout + proc.stderr
        if proc.returncode:
            output.error(log[-8000:])
            raise BuildFailed(f"{BUILD_SCRIPT} exited {proc.returncode}", target=resolved["id"])
        return BuildResult(artifacts=artifacts, log=log)

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved["preserve"])

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        match = _VERSION.search(log_line)
        if match:
            return {"gameVersion": match.group("version"), "loader": "native-linux", "loaderVersion": None}
        if "exact-build-chat-hook-installed" in log_line:
            return {"gameVersion": None, "loader": "native-linux", "loaderVersion": None}
        return None

    def runtime_env(self, resolved: dict[str, Any], takaro: dict[str, str]) -> dict[str, str]:
        # Derived separately for each ephemeral verification run. The release itself
        # carries no registration token or native bearer secret.
        token = hashlib.sha256(b"takaro-ark-native:" + takaro["TAKARO_REGISTRATION_TOKEN"].encode("utf-8")).hexdigest()
        return {
            **{str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()},
            "ARK_NATIVE_TOKEN": token,
        }

    def container_mounts(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        del resolved
        plugin = data_dir / "TakaroArk/TakaroArkNative/libtakaro-ark-native.so"
        if not plugin.is_file():
            raise ConflictError(f"{plugin} is missing; deploy the native artifact first")
        launcher = plugin.with_name("launch.sh")
        if not launcher.is_file():
            raise ConflictError(f"{launcher} is missing; deploy the complete native artifact first")
        exe = data_dir / SERVER_BINARY
        if not exe.is_file():
            raise ConflictError(f"{exe} is missing; install the exact Steam target first")
        (data_dir / ".takaro/home").mkdir(parents=True, exist_ok=True)
        return [f"{data_dir}:{SERVER_ROOT}"]

    def container_options(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        del resolved, data_dir
        return [
            "--init",
            "--memory",
            "24g",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "--workdir",
            f"{SERVER_ROOT}/ShooterGame/Binaries/Linux",
        ]

    def container_command(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        del resolved, data_dir
        return [
            f"{SERVER_ROOT}/TakaroArk/TakaroArkNative/launch.sh",
            SERVER_ROOT,
            "TheIsland?listen?SessionName=Takaro-Verify?Port=7787?QueryPort=27025",
            "-server",
            "-log",
            "-NoBattlEye",
        ]

    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        del catalog
        dest = Path(args.dest).expanduser().resolve()
        if getattr(args, "rollback", False):
            document = steam_install.rollback(target, dest=dest)
        else:
            document = steam_install.install_exact(
                target,
                dest=dest,
                preserve=self.preserve_globs(resolved),
                cache=paths.cache_dir(),
                log=paths.cache_dir() / "steam/logs" / f"{target.game}-{target.id}.log",
                dry_run=bool(getattr(args, "dry_run", False)),
            )
        output.emit("install", True, **document)
        return OK

    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        role = component["role"]
        if role not in FOLDERS:
            raise ConflictError(f"unexpected ARK component role {role}")
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        folder = FOLDERS[role]
        stage = install_dir / f".{folder}.staging"
        if stage.exists() or stage.is_symlink():
            raise ConflictError(f"stale deploy stage {stage} exists; installed connector is untouched")
        stage.mkdir(parents=True, exist_ok=False)
        try:
            with open_zip(artifact) as archive:
                names = archive.namelist()
                for name in names:
                    relative = name.rstrip("/")
                    if not relative:
                        continue
                    if relative != folder and not relative.startswith(folder + "/"):
                        raise ConflictError(f"{artifact.name} contains an entry outside {folder}/: {name}")
                    paths.safe_relative(relative, field="artifact zip entry")
                required: tuple[str, ...]
                if role == "server-plugin":
                    required = (
                        f"{folder}/libtakaro-ark-native.so",
                        f"{folder}/launch.sh",
                        f"{folder}/uninstall-manifest.json",
                        f"{folder}/takaro-target.json",
                    )
                else:
                    required = (
                        f"{folder}/Dockerfile",
                        f"{folder}/dist/index.js",
                        f"{folder}/package-lock.json",
                        f"{folder}/uninstall-manifest.json",
                        f"{folder}/takaro-target.json",
                    )
                if any(name not in names for name in required):
                    raise ConflictError(f"{artifact.name} lacks required {role} files")
                for member in archive.infolist():
                    if member.is_dir():
                        continue
                    relative_path = Path(member.filename).relative_to(folder)
                    destination = stage / relative_path
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(member) as source, destination.open("wb") as sink:
                        shutil.copyfileobj(source, sink)
            if role == "server-plugin":
                (stage / "launch.sh").chmod(0o755)  # allowlisted entrypoint only
            replace_directory(stage, install_dir / folder, subject=folder)
        finally:
            shutil.rmtree(stage, ignore_errors=True)


GAME = ArkAdapter()
