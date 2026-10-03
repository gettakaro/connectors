"""The Project Zomboid adapter: a Steam-delivered server and a Gradle-built JVM agent.

Project Zomboid's dedicated server is three Steam depots and its own bundled Zulu JDK, so
this adapter owns its installation the way 7 Days to Die does. What it deploys is not a
mod folder but a single ``-javaagent`` jar the server JVM loads through
``JAVA_TOOL_OPTIONS``, built by a tracked shell script inside a pinned JDK container.

Two things here exist only because of the image this game runs in. Its entrypoint calls
SteamCMD (``app_update 380870 … validate``) on every start and has no environment knob to
stop it, so an install writes a SteamCMD stub the rig mounts over the image's own copy:
without it the pinned bytes would be replaced by the branch head at every boot. And the
agent is loaded from the install directory rather than the cache directory, which is only
safe once that updater is neutralised.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import zipfile
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import OK, BuildFailed
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env

GAME_ID = "zomboid"
REFERENCES_ROOT = "games/zomboid/_data/references"
DIST_ROOT = "games/zomboid/_data/dist"
BUILD_SCRIPT = "games/zomboid/scripts/build-release.sh"
GAME_JAR = "java/projectzomboid.jar"

# What the server loads and where. The versioned jar is the artifact; the stable name
# beside it is what JAVA_TOOL_OPTIONS names, so the rig's compose file never has to know
# which connector version is deployed.
AGENT_DIR = "Takaro"
STABLE_JAR = "TakaroConnector.jar"

INSTALL_DIR_IN_CONTAINER = "/home/steam/ZomboidDedicatedServer"
CACHE_DIR_IN_CONTAINER = "/home/steam/Zomboid"
STEAMCMD_DIR_IN_CONTAINER = "/home/root/.local/steamcmd"
STUB_RELATIVE = ".takaro/steamcmd-stub"

AGENT_PATH_IN_CONTAINER = f"{INSTALL_DIR_IN_CONTAINER}/{AGENT_DIR}/{STABLE_JAR}"

# The one line the runtime guard writes. AgentLog stamps every line "<ts> [Takaro] <message>",
# so this is the prefix as it appears in the server log rather than the bare message.
TARGET_CHECK_PREFIX = "[Takaro] target-check:"
_TARGET_CHECK = re.compile(r"\[Takaro\]\s*target-check:\s*(?P<payload>\{.*\})\s*$")

STUB_README = """\
Written by takaro-maint.

The Project Zomboid server image (renegademaster/zomboid-dedicated-server) runs
`steamcmd.sh +runscript install_server.scmd` on every start, which is
`app_update 380870 -beta $GAME_VERSION validate` — it would replace this exactly pinned
install with whatever the branch head has become, and the image exposes no environment
variable that turns it off.

Mounting this directory read-only over the image's own steamcmd directory
({steamcmd_dir}) puts the stub next to it on PATH, so the update step runs the stub,
prints one line and exits 0. Nothing else about the boot changes.
"""

STUB_SCRIPT = """\
#!/usr/bin/env bash
# Written by takaro-maint. This install is pinned to a catalog target; the image's own
# `steamcmd.sh +runscript install_server.scmd` (app_update 380870 … validate) must not run.
echo "takaro-maint: SteamCMD is disabled — this server is pinned to catalog target {target}"
exit 0
"""


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


class ZomboidAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the rig, the scripts and CI read; nothing here is spelled out twice."""
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
            f"{prefix}_ARTIFACT": str(resolved["artifactFileNames"]["agent"]),
            f"{prefix}_REFERENCES_DIR": f"{REFERENCES_ROOT}/{resolved['fp16']}",
            f"{prefix}_AGENT_PATH": AGENT_PATH_IN_CONTAINER,
        }
        game_jar = server["files"].get(GAME_JAR, {}).get("sha256")
        if game_jar:
            env[f"{prefix}_GAME_JAR_SHA256"] = str(game_jar)
        # The dependency URLs and hashes the Gradle build verifies before it uses them.
        for name, dep in sorted(resolved["build"]["deps"].items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        """The agent's own target-check line, when the line carries one.

        The game version is reported only when the jar the JVM loaded really is the one the
        target pins: the server writes no version banner of its own, so the bytes are the
        only evidence there is.
        """
        found = _TARGET_CHECK.search(log_line)
        if not found:
            return None
        try:
            payload = json.loads(found.group("payload"))
        except json.JSONDecodeError:
            return None
        expected = payload.get("expected") or {}
        runtime = payload.get("runtime") or {}
        matched = bool(expected.get("gameJarSha256")) and expected.get("gameJarSha256") == runtime.get("gameJarSha256")
        return {
            "gameVersion": expected.get("gameVersion") if matched else None,
            "loader": "javaagent",
            "loaderVersion": runtime.get("agentVersion"),
        }

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
        """Run the tracked release script. Gradle always runs in the pinned JDK container.

        ``toolchain`` is accepted for parity with the Minecraft games but changes nothing:
        the agent compiles against class-file v69 game classes, so a host JDK would be a
        promise this adapter cannot keep. ``gradle_args`` reach the script after ``--``.
        """
        del toolchain
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
        if gradle_args:
            command += ["--", *[str(argument) for argument in gradle_args]]
        output.info(f"building {resolved['id']} {version} (JDK container toolchain)")
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
        """The image's environment plus the agent's.

        The agent's ConfigLoader reads the environment and lets it override any
        ``TakaroConfig.txt``, so the verification run needs no file on disk. The server
        passwords below are this run's own: the harness publishes no host port, so nothing
        outside the docker bridge can reach the server they protect.
        """
        return {
            **{str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()},
            # The rig runs a Steam-visible server; a verification run is not one. Left on,
            # the server waits for the Steam master servers and shuts itself down when it
            # cannot authenticate there ("Failed to connect to Steam servers"), which has
            # nothing to do with the connector. `-nosteam` changes nothing the checks
            # touch: RCON, the hooks and the WebSocket are all internal to the JVM.
            "USE_STEAM": "false",
            "SERVER_NAME": "takaro-verify",
            "MAX_PLAYERS": "2",
            "MAX_RAM": "2048m",
            "GC_CONFIG": "ZGC",
            "ADMIN_USERNAME": "admin",
            "ADMIN_PASSWORD": "takaro-verify",
            "SERVER_PASSWORD": "takaro-verify",
            # The connector runs off the RCON server's tick, so RCON has to be up.
            "RCON_PORT": "27015",
            "RCON_PASSWORD": "takaro-verify",
            "JAVA_TOOL_OPTIONS": f"-javaagent:{AGENT_PATH_IN_CONTAINER}",
            "TAKARO_DEBUG": "true",
            "TAKARO_TARGET_POLICY": "enforce",
            **{str(k): str(v) for k, v in takaro.items()},
        }

    def container_mounts(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """The install, the server's cache directory, and the stub that stops the updater."""
        del resolved
        cache = data_dir / ".takaro" / "runtime" / "cache"
        cache.mkdir(parents=True, exist_ok=True)
        return [
            f"{data_dir}:{INSTALL_DIR_IN_CONTAINER}",
            f"{cache}:{CACHE_DIR_IN_CONTAINER}",
            f"{data_dir / STUB_RELATIVE}:{STEAMCMD_DIR_IN_CONTAINER}:ro",
        ]

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
                output.info("Project Zomboid keeps its saves in the cache directory; the world flags change nothing")
            document = steam_install.install_exact(
                target,
                dest=dest,
                preserve=self.preserve_globs(resolved),
                cache=cache,
                log=log,
                dry_run=bool(getattr(args, "dry_run", False)),
                post_install=lambda staging: self._post_install(staging, target.id),
            )
        output.emit("install", True, **document)
        return OK

    def _post_install(self, staging: Path, target_id: str) -> None:
        """What a fresh Steam tree still needs before the image will run it unchanged."""
        steam_install.restore_executables(staging)
        (staging / AGENT_DIR).mkdir(exist_ok=True)
        stub_dir = staging / STUB_RELATIVE
        stub_dir.mkdir(parents=True, exist_ok=True)
        stub = stub_dir / "steamcmd.sh"
        stub.write_text(STUB_SCRIPT.format(target=target_id), encoding="utf-8")
        stub.chmod(0o755)
        (stub_dir / "README.txt").write_text(
            STUB_README.format(steamcmd_dir=STEAMCMD_DIR_IN_CONTAINER), encoding="utf-8"
        )

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """One stable name beside the versioned jar, and no older jar left behind."""
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        stable = install_dir / STABLE_JAR
        staged = install_dir / (STABLE_JAR + ".tmp")
        staged.write_bytes(artifact.read_bytes())
        os.replace(staged, stable)
        for stale in sorted(install_dir.glob("takaro-zomboid-agent-*.jar")) + sorted(
            install_dir.glob("TakaroConnector-*.jar")
        ):
            if stale.name != artifact.name:
                stale.unlink()
        try:
            with zipfile.ZipFile(artifact) as archive:
                raw = archive.read("META-INF/MANIFEST.MF").decode("utf-8", errors="replace")
        except (KeyError, OSError, zipfile.BadZipFile):
            return
        found = re.search(r"^Implementation-Version:\s*(?P<version>.+)$", raw, re.MULTILINE)
        if found:
            output.info(f"deployed agent {found.group('version').strip()} as {component['installDir']}/{STABLE_JAR}")


GAME = ZomboidAdapter()
