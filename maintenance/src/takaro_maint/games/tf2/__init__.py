"""The Team Fortress 2 adapter: a SourceMod plugin inside the dedicated server (srcds).

The server is a plain Steam depot install (app 232250, Linux or Windows) and stays untouched;
MetaMod:Source and SourceMod are the operator's, not shipped here. The connector is the
SourceMod plugin ``takaro_tf2.smx`` plus the two ProjectSky extensions it needs (websocket and
json), which hold the Takaro WebSocket in the server process. The release zip mirrors the
server's ``tf/`` folder. No server runs in a container here, so the target claims
``contract`` and the workflow passes ``runtime: false``.
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

GAME_ID = "tf2"
DIST_ROOT = "games/tf2/_data/dist"
BUILD_SCRIPT = "games/tf2/scripts/build-release.sh"

#: The entries a release zip must carry; without them SourceMod would not load the connector.
REQUIRED_ENTRIES = (
    "addons/sourcemod/plugins/takaro_tf2.smx",
    "addons/sourcemod/takaro-target.json",
)
#: What a release zip may write into tf/: its own SourceMod folders, its identity and its config
#: template. Anything else (server.cfg, SourceMod's admin config, another plugin) is refused.
ALLOWED_PREFIXES = (
    "addons/sourcemod/plugins/takaro_tf2.smx",
    "addons/sourcemod/extensions/",
    "addons/sourcemod/scripting/takaro_tf2",
    "addons/sourcemod/licenses/",
    "addons/sourcemod/takaro-target.json",
    "cfg/sourcemod/takaro_tf2.cfg",
)
#: Operator state: the config holding the registration token and the connector's ban ledger.
#: A deploy never replaces a config that already exists, and a release never carries the ledger.
OPERATOR_CONFIG = "cfg/sourcemod/takaro_tf2.cfg"
STATE_FILES = ("addons/sourcemod/data/takaro_tf2_bans.txt",)

# What the srcds `version` command prints: "Exe version 11138802 (tf)" (the steam.inf PatchVersion).
_VERSION = re.compile(r"Exe version (?P<version>[0-9]+) \(tf\)")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class Tf2Adapter(BaseAdapter):
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
            return {"gameVersion": match.group("version"), "loader": "sourcemod", "loaderVersion": None}
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
        """Run the tracked release script; it compiles inside the pinned image.

        ``toolchain`` and ``gradle_args`` change nothing for a script build; determinism comes
        from ``SOURCE_DATE_EPOCH`` and a clean stage, and is proven by building twice.
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
        output.info(f"building {resolved['id']} {version} (SourceMod spcomp in the pinned image)")
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
        """Unpack the zip over ``tf/``, file by file, keeping the operator's config and bans.

        The zip is checked completely before anything is written: every entry is one of the
        connector's own paths (``ALLOWED_PREFIXES``), the required files are present, and no entry
        is connector state.
        An existing ``takaro_tf2.cfg`` holds the registration token and is never replaced.
        """
        role = str(component["role"])
        if role != "plugin":
            raise ConflictError(f"TF2 ships one component, plugin; refusing role '{role}'")
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        with open_zip(artifact) as archive:
            names = [name for name in archive.namelist() if name.rstrip("/")]
            for name in names:
                relative = name.rstrip("/")
                paths.safe_relative(relative, field="artifact zip entry")
                if name.endswith("/"):
                    continue
                if not relative.startswith(ALLOWED_PREFIXES):
                    raise ConflictError(
                        f"{artifact.name} holds '{name}', which a TF2 release never ships; nothing was extracted"
                    )
                if relative in STATE_FILES:
                    raise ConflictError(f"{artifact.name} carries connector state '{name}'; nothing was extracted")
            missing = [entry for entry in REQUIRED_ENTRIES if entry not in names]
            if missing:
                raise ConflictError(f"{artifact.name} is missing {', '.join(missing)}; nothing was extracted")
            install_dir.mkdir(parents=True, exist_ok=True)
            for member in archive.infolist():
                if member.is_dir():
                    continue
                destination = install_dir / member.filename
                if member.filename == OPERATOR_CONFIG and destination.exists():
                    output.info(f"kept the existing {OPERATOR_CONFIG}")
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                # Written beside the destination and renamed, so the server never loads half a file.
                with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as staged:
                    staged.write(archive.read(member))
                os.chmod(staged.name, 0o644)
                os.replace(staged.name, destination)
        stamp = install_dir / "addons/sourcemod/takaro-target.json"
        try:
            version = json.loads(stamp.read_text(encoding="utf-8")).get("connectorVersion")
        except (OSError, json.JSONDecodeError):
            version = None
        if version:
            output.info(f"unpacked takaro_tf2 {version} into {component['installDir']}/")


GAME = Tf2Adapter()
