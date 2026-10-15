"""Catalog build and deployment for the native CS2 CounterStrikeSharp plugin."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from ... import paths
from ...exit_codes import BuildFailed, ConflictError
from ..base import BaseAdapter, BuildResult, common_env, open_zip, replace_directory


class CounterStrike2Adapter(BaseAdapter):
    id = "counter-strike2"

    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        return {**common_env(resolved, prefix), f"{prefix}_REVISION": str(resolved["revision"])}

    def artifact_paths(self, resolved: dict[str, Any], version: str, repo_root: Path) -> dict[str, Path]:
        artifact = resolved["components"][0]["artifact"].replace("{version}", version)
        return {"plugin": repo_root / "games/counter-strike2/_data/dist" / resolved["fp16"] / artifact}

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
        artifact = artifacts["plugin"]
        artifact.parent.mkdir(parents=True, exist_ok=True)
        artifact.unlink(missing_ok=True)
        artifact.with_name(artifact.name + ".meta.json").unlink(missing_ok=True)
        env = dict(os.environ)
        if source_revision:
            env["TAKARO_SOURCE_REVISION"] = source_revision
        proc = subprocess.run(
            ["bash", str(repo_root / "games/counter-strike2/scripts/build-release.sh"),
             version, str(artifact.parent), "--target", str(resolved["id"])],
            cwd=repo_root, env=env, text=True, capture_output=True, check=False,
        )
        log = proc.stdout + proc.stderr
        if proc.returncode:
            raise BuildFailed(f"CS2 release build exited {proc.returncode}: {log[-3000:]}", target=resolved["id"])
        return BuildResult(artifacts=artifacts, log=log)

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved["preserve"])

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        if "Takaro CS2 identified as gameServerId=" in log_line:
            return {"gameVersion": None, "loader": "counterstrikesharp", "loaderVersion": "1.0.376"}
        return None

    def runtime_env(self, resolved: dict[str, Any], takaro: dict[str, str]) -> dict[str, str]:
        return {**resolved["runtime"]["container"].get("env", {}), **takaro}

    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        if component["role"] != "plugin":
            raise ConflictError("unexpected CS2 component role")
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        stage = install_dir / ".TakaroCs2.staging"
        if stage.exists() or stage.is_symlink():
            raise ConflictError(f"stale deploy stage {stage}")
        stage.mkdir(parents=True, exist_ok=False)
        try:
            with open_zip(artifact) as archive:
                names = archive.namelist()
                required = {"TakaroCs2/TakaroCs2.dll", "TakaroCs2/Takaro.Cs2.Core.dll", "TakaroCs2/TakaroCs2.example.json"}
                if not required.issubset(names):
                    raise ConflictError(f"{artifact.name} lacks required plugin files")
                for member in archive.infolist():
                    name = member.filename.rstrip("/")
                    if not name.startswith("TakaroCs2/"):
                        raise ConflictError(f"{artifact.name} contains an entry outside TakaroCs2/: {name}")
                    paths.safe_relative(name, field="artifact zip entry")
                    if member.is_dir():
                        continue
                    relative = Path(name).relative_to("TakaroCs2")
                    output = stage / relative
                    output.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(member) as source, output.open("wb") as sink:
                        shutil.copyfileobj(source, sink)
            replace_directory(stage, install_dir / "TakaroCs2", subject="TakaroCs2")
        finally:
            shutil.rmtree(stage, ignore_errors=True)


GAME = CounterStrike2Adapter()
