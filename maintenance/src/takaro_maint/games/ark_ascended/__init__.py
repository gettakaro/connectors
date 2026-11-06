"""The ARK: Survival Ascended adapter: a Windows dedicated server with the connector as an AsaApi plugin.

The server is a plain Steam depot install (app 2430930, Windows only) and stays untouched. The
connector is ``TakaroAsa.dll``, loaded by AsaApi from ``ArkApi/Plugins/TakaroAsa``, which holds the
Takaro WebSocket. The release zip mirrors ``ShooterGame/Binaries/Win64``: the pinned AsaApi release
and the plugin folder. No server runs in a container here, so the target claims ``contract`` and
the workflow passes ``runtime: false``.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import OK, BuildFailed, ConflictError
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env, open_zip

GAME_ID = "ark-ascended"
DIST_ROOT = "games/ark-ascended/_data/dist"
BUILD_SCRIPT = "games/ark-ascended/scripts/build-release.sh"
SERVER_BINARY = "ShooterGame/Binaries/Win64/ArkAscendedServer.exe"

PLUGIN_DIR = "ArkApi/Plugins/TakaroAsa"
#: The entries a release zip must carry; without them AsaApi or the connector would not load.
REQUIRED_ENTRIES = (
    "AsaApiLoader.exe",
    "ArkApi/AsaApi.dll",
    f"{PLUGIN_DIR}/TakaroAsa.dll",
    f"{PLUGIN_DIR}/PluginInfo.json",
    f"{PLUGIN_DIR}/takaro-target.json",
)
#: Top-level names a release zip may write into ShooterGame/Binaries/Win64.
ALLOWED_TOP = (
    "ArkApi",
    "AsaApiLoader.exe",
    "AsaApiLoader.pdb",
    "PROVENANCE.txt",
    "THIRD-PARTY-NOTICES.txt",
    "config.json",
    "libcrypto-3-x64.dll",
    "libssl-3-x64.dll",
    "msdia140.dll",
    "msvcp140.dll",
)
#: Operator and connector state in the plugin folder; a release never carries or replaces them.
STATE_FILES = ("TakaroConfig.txt", "takaro.log")
STATE_DIRS = ("state",)
#: AsaApi's own settings: the zip carries the defaults, an operator's file is kept.
KEEP_IF_PRESENT = ("config.json",)

# "ARK Version: 96.9" in ShooterGame.log.
_VERSION = re.compile(r"ARK Version: (?P<version>[0-9][0-9.]*)")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class ArkAscendedAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the build script and CI read. No ``_JAVA``: nothing here is a JVM."""
        server = resolved["inputs"]["server"]
        depots = ";".join(f"{depot}:{server['depots'][depot]['manifest']}" for depot in sorted(server["depots"]))
        env = {
            **common_env(resolved, prefix),
            f"{prefix}_TOOLCHAIN": str(resolved["toolchainRef"]),
            f"{prefix}_REVISION": str(resolved["revision"]),
            f"{prefix}_PLATFORM": str(resolved["platform"]),
            f"{prefix}_STEAM_APP": str(server["app"]),
            f"{prefix}_STEAM_BRANCH": str(server["branch"]),
            f"{prefix}_STEAM_BUILDID": str(server["buildid"]),
            f"{prefix}_STEAM_DEPOTS": depots,
            f"{prefix}_SERVER_EXE_SHA256": str(server["files"][SERVER_BINARY]["sha256"]),
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
            return {"gameVersion": match.group("version"), "loader": "asaapi", "loaderVersion": None}
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
        """Run the tracked release script; it packages the DLL the Windows runner compiled.

        ``toolchain`` and ``gradle_args`` change nothing for a script build. The packaging is
        deterministic from ``SOURCE_DATE_EPOCH``; the MSVC compile happens before this, once per run.
        """
        del out, toolchain, gradle_args
        artifacts = self.artifact_paths(resolved, version, repo_root)
        dist = repo_root / DIST_ROOT / resolved["fp16"]
        dist.mkdir(parents=True, exist_ok=True)
        # A build that produces nothing must fail, not succeed over the previous build's files.
        for stale in artifacts.values():
            stale.unlink(missing_ok=True)
            stale.with_name(stale.name + ".meta.json").unlink(missing_ok=True)
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
        output.info(f"packaging {resolved['id']} {version} around the MSVC-built TakaroAsa.dll")
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
        """Nothing runs here: the Windows server is not booted by this repository."""
        del takaro
        return {str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()}

    # -- install --------------------------------------------------------------
    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        """The whole installation is one pinned Steam depot set, so the adapter owns it."""
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
                log=paths.cache_dir() / "steam" / "logs" / f"{target.game}-{target.id}.log",
                dry_run=bool(getattr(args, "dry_run", False)),
            )
        output.emit("install", True, **document)
        return OK

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """Unpack the zip over ``ShooterGame/Binaries/Win64``, file by file, keeping the operator's state.

        The zip is checked completely before anything is written: every entry stays under one of
        the allowed top-level names, the required files are present, and no entry would replace
        the operator's config, the connector's state or its log. An existing AsaApi ``config.json``
        is left alone.
        """
        role = str(component["role"])
        if role != "mod":
            raise ConflictError(f"ARK: Survival Ascended ships one component, mod; refusing role '{role}'")
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        with open_zip(artifact) as archive:
            names = [name for name in archive.namelist() if name.rstrip("/")]
            for name in names:
                relative = name.rstrip("/")
                paths.safe_relative(relative, field="artifact zip entry")
                if relative.split("/", 1)[0] not in ALLOWED_TOP:
                    raise ConflictError(f"{artifact.name} holds '{name}' outside {ALLOWED_TOP}; nothing was extracted")
                if relative.startswith(f"{PLUGIN_DIR}/"):
                    inside = relative[len(PLUGIN_DIR) + 1 :].split("/")
                    if inside[0] in STATE_DIRS or inside == [inside[0]] and inside[0] in STATE_FILES:
                        raise ConflictError(f"{artifact.name} carries operator state '{name}'; nothing was extracted")
            missing = [entry for entry in REQUIRED_ENTRIES if entry not in names]
            if missing:
                raise ConflictError(f"{artifact.name} is missing {', '.join(missing)}; nothing was extracted")
            install_dir.mkdir(parents=True, exist_ok=True)
            for member in archive.infolist():
                if member.is_dir():
                    continue
                destination = install_dir / member.filename
                if member.filename in KEEP_IF_PRESENT and destination.exists():
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                # Written beside the destination and renamed, so the server never loads half a file.
                with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as staged:
                    staged.write(archive.read(member))
                os.chmod(staged.name, 0o644)
                os.replace(staged.name, destination)
        stamp = install_dir / PLUGIN_DIR / "takaro-target.json"
        try:
            version = json.loads(stamp.read_text(encoding="utf-8")).get("connectorVersion")
        except (OSError, json.JSONDecodeError):
            version = None
        if version:
            output.info(f"unpacked TakaroAsa {version} and AsaApi into {component['installDir']}/")


GAME = ArkAscendedAdapter()
