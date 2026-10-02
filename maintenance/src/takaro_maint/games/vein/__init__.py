"""Exact Steam target for VEIN's native Linux connector."""

from __future__ import annotations

import os
import re
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import OK, BuildFailed, ConflictError
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env, replace_directory

GAME_ID = "vein"
BUILD_SCRIPT = "games/vein/scripts/build-release.sh"
SERVER_BINARY = "Vein/Binaries/Linux/VeinServer-Linux-Test"
_VERSION = re.compile(r"LogNetVersion: Set ProjectVersion to (?P<version>[0-9][A-Za-z0-9.]+)")


class VeinAdapter(BaseAdapter):
    id = GAME_ID

    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        server = resolved["inputs"]["server"]
        return {
            **common_env(resolved, prefix),
            f"{prefix}_STEAM_APP": str(server["app"]),
            f"{prefix}_STEAM_BRANCH": str(server["branch"]),
            f"{prefix}_STEAM_BUILDID": str(server["buildid"]),
            f"{prefix}_SERVER_BINARY_SHA256": str(server["files"][SERVER_BINARY]["sha256"]),
            f"{prefix}_ARTIFACT_PLUGIN": str(resolved["artifactFileNames"]["plugin"]),
        }

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        version = _VERSION.search(log_line)
        if version:
            return {"gameVersion": version.group("version"), "loader": "unreal", "loaderVersion": None}
        return None

    def artifact_paths(self, resolved: dict[str, Any], version: str, repo_root: Path) -> dict[str, Path]:
        component = resolved["components"][0]
        return {
            "plugin": repo_root
            / "games/vein/_data/dist"
            / resolved["fp16"]
            / component["artifact"].replace("{version}", version)
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
        target = self.artifact_paths(resolved, version, repo_root)["plugin"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.unlink(missing_ok=True)
        environment = dict(os.environ)
        if source_revision:
            environment["TAKARO_SOURCE_REVISION"] = source_revision
        completed = subprocess.run(
            ["bash", str(repo_root / BUILD_SCRIPT), version, str(target.parent)],
            cwd=repo_root,
            capture_output=True,
            text=True,
            env=environment,
            check=False,
        )
        log = completed.stdout + completed.stderr
        if completed.returncode:
            output.error(log[-8000:])
            raise BuildFailed(f"{BUILD_SCRIPT} exited {completed.returncode}", target=str(resolved["id"]))
        packaged = target.parent / "takaro-vein-plugin.tar.gz"
        if not packaged.is_file():
            raise BuildFailed(f"{BUILD_SCRIPT} produced no plugin archive", target=str(resolved["id"]))
        os.replace(packaged, target)
        return BuildResult(artifacts={"plugin": target}, log=log)

    def runtime_env(self, resolved: dict[str, Any], takaro: dict[str, str]) -> dict[str, str]:
        del takaro
        return {str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()}

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
                log=paths.cache_dir() / "steam/logs/vein-install.log",
                dry_run=bool(getattr(args, "dry_run", False)),
            )
        output.emit("install", True, **document)
        return OK

    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        install_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=install_dir) as temporary:
            staged = Path(temporary)
            with tarfile.open(artifact, "r:gz") as archive:
                for member in archive.getmembers():
                    name = member.name.rstrip("/")
                    if name != "TakaroVein" and not name.startswith("TakaroVein/"):
                        raise ConflictError(f"unexpected archive entry {member.name}")
                    paths.safe_relative(name, field="artifact archive entry")
                    if member.issym() or member.islnk():
                        raise ConflictError(f"archive link entry {member.name} is refused")
                archive.extractall(staged, filter="data")
            if not (staged / "TakaroVein/libtakaro-vein.so").is_file():
                raise ConflictError("archive has no native plugin")
            replace_directory(staged / "TakaroVein", install_dir / "TakaroVein", subject="VEIN plugin")


GAME = VeinAdapter()
