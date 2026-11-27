"""The Hytale adapter: one Java plugin jar in the dedicated server's ``mods/`` folder.

The server is Hypixel's own ``HytaleServer.jar`` plus its ``Assets.zip``, installed by the
operator with Hypixel's downloader, which needs a Hytale account; the server then needs an
account login of its own before players can join. Nothing here installs or boots it. The
connector is the plugin jar, which holds the Takaro WebSocket in the server process. It
compiles against ``com.hypixel.hytale:Server`` from Hypixel's Maven repository, fetched by
SHA-256 for the build only. No server runs in a container here, so the target claims
``contract`` and the workflow passes ``runtime: false``.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path
from typing import Any

from ... import output
from ...exit_codes import BuildFailed, ConflictError, UsageError
from ..base import BaseAdapter, BuildResult, common_env

GAME_ID = "hytale"
DIST_ROOT = "games/hytale/_data/dist"
BUILD_SCRIPT = "games/hytale/scripts/build-release.sh"

# What the server logs at boot: "Booting up HytaleServer - Version: 0.7.7, Revision: 12fc4656...".
_VERSION = re.compile(r"HytaleServer - Version: (?P<version>[0-9][0-9A-Za-z.+-]*)")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class HytaleAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the build script and CI read."""
        env = {
            **common_env(resolved, prefix),
            f"{prefix}_TOOLCHAIN": str(resolved["toolchainRef"]),
            f"{prefix}_REVISION": str(resolved["revision"]),
            f"{prefix}_PLATFORM": str(resolved["platform"]),
        }
        for role, name in sorted(resolved["artifactFileNames"].items()):
            env[f"{prefix}_ARTIFACT_{_env_key(role)}"] = str(name)
        for name, dep in sorted(resolved["build"]["deps"].items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_COORDINATE"] = str(dep["coordinate"])
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        match = _VERSION.search(log_line)
        if match:
            return {"gameVersion": match.group("version"), "loader": "hytale", "loaderVersion": None}
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
        """Run the tracked release script; it compiles with Maven inside the pinned image.

        ``toolchain`` and ``gradle_args`` change nothing for a script build; determinism comes
        from ``SOURCE_DATE_EPOCH`` and a repacked jar, and is proven by building twice.
        """
        del out, toolchain, gradle_args
        artifacts = self.artifact_paths(resolved, version, repo_root)
        dist = repo_root / DIST_ROOT / resolved["fp16"]
        dist.mkdir(parents=True, exist_ok=True)
        # A build that produces nothing must fail, not succeed over the previous build's files.
        for stale in artifacts.values():
            stale.unlink(missing_ok=True)
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
        output.info(f"building {resolved['id']} {version} (Maven in the pinned image)")
        completed = subprocess.run(
            command, cwd=str(repo_root), capture_output=True, text=True, env=environment, check=False
        )
        log = completed.stdout + completed.stderr
        if completed.returncode != 0:
            output.error(log[-8000:])
            raise BuildFailed(f"{BUILD_SCRIPT} exited {completed.returncode}", target=str(resolved["id"]))
        return BuildResult(artifacts=artifacts, log=log)

    # -- runtime --------------------------------------------------------------
    def runtime_env(self, resolved: dict[str, Any], takaro: dict[str, str]) -> dict[str, str]:
        """Nothing runs here: the server is not booted by the release workflow."""
        del takaro
        return {str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()}

    # -- install --------------------------------------------------------------
    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        """Refused: a Hytale server install needs the operator's Hytale account."""
        del catalog, target, resolved, args
        raise UsageError(
            "a Hytale server is installed with Hypixel's downloader, which needs a Hytale account, "
            "and the server needs an account login before players can join; install it yourself and "
            "use `deploy` to place the plugin jar in its mods/ folder"
        )

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """The jar loads as it lands; refuse anything that is not the one plugin jar."""
        del dest
        role = str(component["role"])
        if role != "plugin":
            raise ConflictError(f"Hytale ships one component, plugin; refusing role '{role}'")
        if artifact.suffix != ".jar":
            raise ConflictError(f"{artifact.name} is not a plugin jar")


GAME = HytaleAdapter()
