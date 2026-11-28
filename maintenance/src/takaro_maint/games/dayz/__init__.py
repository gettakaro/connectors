"""The DayZ adapter: a server mod plus a small Node.js sidecar, on a Steam-pinned Linux server.

DayZ's Enforce Script can make HTTP requests but has no WebSocket client, so the connector
is two parts that ship together. The ``@TakaroIntegration`` server mod (one
``TakaroIntegration.pbo``, loaded with ``-serverMod=``) reports events and runs Takaro's
actions inside the server; the sidecar on the same host serves the mod's loopback HTTP API
and holds the Takaro WebSocket. Both artifacts are required.

The server is Steam app 223350. Steam publishes its manifests to the anonymous login, but
serves the depots only to an account that owns DayZ, so the target's declared file hashes
are self-recorded and its install needs ``STEAM_USER``. No hosted runner has such an
account, which is why the target claims ``contract`` and the workflow passes
``runtime: false``.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import BuildFailed, ConflictError
from ..base import BaseAdapter, BuildResult, common_env, open_zip

GAME_ID = "dayz"
DIST_ROOT = "games/dayz/_data/dist"
BUILD_SCRIPT = "games/dayz/scripts/build-release.sh"

#: The single top-level folder each role's archive may write. The mod folder is the name the
#: server is started with (``-serverMod=@TakaroIntegration``), so it is not renamed here.
ROLE_FOLDER = {"mod": "@TakaroIntegration", "sidecar": "TakaroDayZSidecar"}

#: Operator state inside a role's folder that an upgrade must keep. The folder itself is
#: replaced wholesale so a file removed upstream cannot survive; the mod keeps its own
#: settings under the server's profiles/ directory, outside its folder.
DEPLOY_PRESERVED: dict[str, tuple[str, ...]] = {"mod": (), "sidecar": ("TakaroConfig.txt",)}

# The server's RPT log opens with "Version 1.30.164289.70".
_VERSION = re.compile(r"^Version (?P<version>\d+\.\d+\.\d+(?:\.\d+)?)\s*$")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class DayzAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the rig, the build script and CI read."""
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
        for component in resolved["components"]:
            env[f"{prefix}_INSTALL_DIR_{_env_key(str(component['role']))}"] = str(component["installDir"])
        # The dependency URLs and hashes the build checks the sidecar's lockfile against.
        for name, dep in sorted(resolved["build"]["deps"].items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        match = _VERSION.search(log_line)
        if match:
            return {"gameVersion": match.group("version"), "loader": "enfusion", "loaderVersion": None}
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
        """Run the tracked release script; the sidecar always builds inside the pinned image.

        ``toolchain`` and ``gradle_args`` change nothing for a script build; determinism comes
        from ``SOURCE_DATE_EPOCH`` and a clean stage, and is proven by building twice.
        """
        del out, toolchain, gradle_args
        artifacts = self.artifact_paths(resolved, version, repo_root)
        dist = repo_root / DIST_ROOT / resolved["fp16"]
        dist.mkdir(parents=True, exist_ok=True)
        # A half-finished build must not be able to report over the previous build's bytes.
        for artifact in artifacts.values():
            artifact.unlink(missing_ok=True)
            artifact.with_name(artifact.name + ".meta.json").unlink(missing_ok=True)
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
        output.info(f"building {resolved['id']} {version} (node container toolchain)")
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
        """The pinned image's own environment. The release workflow boots no DayZ server."""
        del takaro
        return {str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()}

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """Unpack one role's archive into its install dir, replacing its folder."""
        role = str(component["role"])
        folder_name = ROLE_FOLDER.get(role)
        if folder_name is None:
            raise ConflictError(f"no folder is defined for the '{role}' role; nothing was extracted")
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        install_dir.mkdir(parents=True, exist_ok=True)
        folder = install_dir / folder_name

        preserved = []
        for name in DEPLOY_PRESERVED.get(role, ()):
            kept = folder / name
            if kept.is_file():
                preserved.append((name, kept.read_bytes(), kept.stat().st_mode & 0o777))

        if artifact.name.endswith(".zip"):
            self._extract_zip(artifact, folder_name, install_dir, folder)
        elif artifact.name.endswith(".tar.gz"):
            self._extract_tar(artifact, folder_name, install_dir, folder)
        else:
            raise ConflictError(f"{artifact.name} is neither a .zip nor a .tar.gz; nothing was extracted")

        for name, body, mode in preserved:
            restored = folder / name
            if restored.exists():
                continue
            restored.write_bytes(body)
            os.chmod(restored, mode)
            output.info(f"kept the existing {folder_name}/{name}")

        # An older artifact for the same role must not stay behind and look installed.
        stem = artifact.name.split("-linux-")[0]
        for stale in sorted(install_dir.glob(f"{stem}-*")):
            if stale.is_file() and stale.name != artifact.name:
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

    @staticmethod
    def _check_entry(artifact: Path, folder_name: str, name: str) -> bool:
        """True for an entry inside the one folder; raises for anything that reaches outside it."""
        relative = name.rstrip("/")
        if not relative or relative == ".":
            return False
        if relative != folder_name and not relative.startswith(f"{folder_name}/"):
            raise ConflictError(
                f"{artifact.name} holds '{name}', outside the single {folder_name}/ folder; nothing was extracted"
            )
        # The folder name itself is fixed above (it may start with '@', which the shared path
        # grammar does not allow); everything under it must be a plain relative path.
        inner = relative[len(folder_name) + 1 :]
        if inner:
            paths.safe_relative(inner, field="artifact archive entry")
        return True

    def _extract_zip(self, artifact: Path, folder_name: str, install_dir: Path, folder: Path) -> None:
        with open_zip(artifact) as archive:
            members: list[zipfile.ZipInfo] = []
            for info in archive.infolist():
                if not self._check_entry(artifact, folder_name, info.filename):
                    continue
                # A zip entry whose mode marks it as a symlink would extract as a plain file
                # holding the link target; refuse it rather than ship a surprise.
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ConflictError(f"{artifact.name} holds a link entry '{info.filename}'; nothing was extracted")
                members.append(info)
            shutil.rmtree(folder, ignore_errors=True)
            for info in members:
                target = install_dir / info.filename
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(info))

    def _extract_tar(self, artifact: Path, folder_name: str, install_dir: Path, folder: Path) -> None:
        with tarfile.open(artifact, "r:gz") as archive:
            members = archive.getmembers()
            for member in members:
                self._check_entry(artifact, folder_name, member.name)
                if member.issym() or member.islnk():
                    raise ConflictError(f"{artifact.name} holds a link entry '{member.name}'; nothing was extracted")
            shutil.rmtree(folder, ignore_errors=True)
            # Every entry was checked above; `filter="data"` is the second lock, not the first.
            archive.extractall(install_dir, members=members, filter="data")


GAME = DayzAdapter()
