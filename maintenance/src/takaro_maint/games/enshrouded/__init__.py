"""The Enshrouded adapter: a Windows server under Proton and one native plugin.

Two things set this game apart from every other adapter here. The server is a Windows
binary that only runs under Proton inside the pinned Linux image, so the "platform" names
the deployment runtime rather than a mod loader; the connector is a ``dbghelp.dll`` proxy
the game loads, which hooks the game and holds the Takaro connection itself. And the image
this server runs in updates the game from SteamCMD at every boot unless it is stopped, so
the install lays down a pinned tree and the compose files mount a tracked override over the
image's updater program; nothing here ever asks Steam what the branch head is.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import zipfile
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import OK, BuildFailed, ConflictError
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env

GAME_ID = "enshrouded"
DIST_ROOT = "games/enshrouded/_data/dist"
BUILD_SCRIPT = "games/enshrouded/scripts/build-release.sh"
UPDATER_OVERRIDE = "games/enshrouded/server/enshrouded-updater"
IMAGE_UPDATER_PATH = "/usr/local/etc/enshrouded/enshrouded-updater"
SERVER_DIR = "/opt/enshrouded/server"

#: The one folder the plugin zip may hold.
ZIP_FOLDER = "TakaroEnshrouded"
PLUGIN_DLL = "dbghelp.dll"

#: The game's own build id, printed by the server at every start. The plugin pins its
#: signatures against exactly this number, which is why it is the target's revision.
BUILD_BANNER = re.compile(r"Game Version \(SVN\):\s*(?P<build>\d+)")

PINNED_NOTE = (
    "Managed by takaro-maint: this directory holds an exactly pinned Steam build.\n"
    "The image's own updater program is replaced by games/enshrouded/server/enshrouded-updater,\n"
    "which never runs SteamCMD here. To move to another game build, re-pin the catalog target,\n"
    "re-derive the plugin signatures and prove it with takaro-maint verify.\n"
)


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


def plugin_token(takaro_env: dict[str, str]) -> str:
    """The plugin's shared secret for one verification run.

    A pure function of the run's throwaway registration token, so every boot of the same
    run agrees on it without a secret ever reaching a docker command line or a kept log.
    """
    return hashlib.sha256(b"takaro-plugin:" + takaro_env["TAKARO_REGISTRATION_TOKEN"].encode("utf-8")).hexdigest()


class EnshroudedAdapter(BaseAdapter):
    id = GAME_ID

    #: The image the last ``container_mounts`` resolved to. The runtime-identity hook reads
    #: the Proton version out of it, and an empty string means no container was described
    #: yet -- which is a missing loader version in the report, never an attribute error.
    last_container_ref: str = ""

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the rig, the build scripts and CI read. No ``_JAVA``: there is no JVM here."""
        server = resolved["inputs"]["server"]
        depots = ";".join(f"{depot}:{server['depots'][depot]['manifest']}" for depot in sorted(server["depots"]))
        container_env = resolved["runtime"]["container"].get("env", {})
        deps = resolved["build"]["deps"]
        env = {
            **common_env(resolved, prefix),
            f"{prefix}_TOOLCHAIN": str(resolved["toolchainRef"]),
            f"{prefix}_REVISION": str(resolved["revision"]),
            f"{prefix}_GAME_BUILD": str(resolved["revision"]),
            # The hook-compatibility statement: the game build the pinned signatures were
            # derived on. Equal to the revision by construction, and named separately so a
            # reader never has to infer that the two mean the same thing.
            f"{prefix}_HOOKS_PROVEN_BUILD": str(resolved["revision"]),
            f"{prefix}_STEAM_APP": str(server["app"]),
            f"{prefix}_STEAM_BRANCH": str(server["branch"]),
            f"{prefix}_STEAM_BUILDID": str(server["buildid"]),
            f"{prefix}_STEAM_DEPOTS": depots,
            f"{prefix}_PROTON": str(container_env.get("TAKARO_PINNED_PROTON", "")),
            f"{prefix}_SERVER_EXE_SHA256": str(server["files"]["enshrouded_server.exe"]["sha256"]),
        }
        for role, artifact in sorted(resolved["artifactFileNames"].items()):
            env[f"{prefix}_ARTIFACT_{_env_key(role)}"] = str(artifact)
        for name, dep in sorted(deps.items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        """The game build the server printed. ``loaderVersion`` is Proton's, read separately."""
        match = BUILD_BANNER.search(log_line)
        if not match:
            return None
        return {"gameVersion": match.group("build"), "loader": "proton", "loaderVersion": None}

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
        """Run the tracked release script, which builds the plugin in the pinned image.

        ``toolchain`` is accepted for parity with the Gradle games and changes nothing: the
        host has neither zig nor zip, so ``host`` would be a promise this adapter cannot
        keep. ``gradle_args`` mean nothing to a script build; determinism comes from
        ``SOURCE_DATE_EPOCH`` and a clean stage, and is proven by building twice.
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
        # A build that produces nothing must be a failure, not a success reported over the
        # leftovers of the previous one, so the names this build claims are cleared before
        # the script runs.
        for stale in self.artifact_paths(resolved, version, repo_root).values():
            stale.unlink(missing_ok=True)
            stale.with_name(stale.name + ".meta.json").unlink(missing_ok=True)
        command = ["bash", str(repo_root / BUILD_SCRIPT), version, str(dist), "--target", str(resolved["id"])]
        output.info(f"building {resolved['id']} {version} (native plugin, pinned builder image)")
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
        """The image's own environment.

        The plugin connects to Takaro itself, but none of the Takaro values go onto a docker
        command line: the verification hooks write the URL, both tokens, the trusted CA and
        the diagnostics secret into ``takaro/plugin.json`` (mode 0600), which the plugin
        reads when the matching environment variables are unset.
        """
        return {
            **{str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()},
            "SERVER_NAME": f"takaro-verify {takaro['TAKARO_IDENTITY_TOKEN']}",
            "PUID": str(os.getuid()),
            "PGID": str(os.getgid()),
        }

    def container_mounts(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """The install, the deployed DLL and the override that keeps SteamCMD out of a boot.

        The DLL is bind-mounted as a file, so it has to exist before the container starts:
        docker would otherwise create a directory with that name and the game would load
        the system ``dbghelp`` instead, which is exactly the silent failure this refuses.
        """
        dll = data_dir / "takaro" / "plugin" / PLUGIN_DLL
        if not dll.is_file():
            raise ConflictError(
                f"{dll} is missing; deploy the server-plugin artifact before booting "
                "(a file bind mount whose source is absent becomes a directory)"
            )
        self.last_data_dir = data_dir
        self.last_container_ref = str(resolved["containerRef"])
        override = paths.repo_root() / UPDATER_OVERRIDE
        return [
            f"{data_dir}:{SERVER_DIR}",
            f"{dll}:{SERVER_DIR}/{PLUGIN_DLL}:ro",
            f"{override}:{IMAGE_UPDATER_PATH}:ro",
        ]

    # -- install --------------------------------------------------------------
    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        """The whole installation is one pinned Steam depot set, so the adapter owns it."""
        del catalog
        dest = Path(args.dest).expanduser().resolve()
        cache = paths.cache_dir()
        log = cache / "steam" / "logs" / f"{target.game}-{target.id}.log"

        if getattr(args, "rollback", False):
            document = steam_install.rollback(target, dest=dest)
        else:
            if getattr(args, "reuse_world", False) or getattr(args, "fresh_world", False):
                output.info(
                    "Enshrouded keeps its save game inside the install directory under savegame/, "
                    "which preserve[] carries forward; the world flags change nothing"
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
        """What a fresh depot tree still needs before the image will run it."""
        app = 2278520
        for relative in (
            f"steamapps/compatdata/{app}",
            "takaro",
            "takaro/plugin",
            "savegame",
            "logs",
            "backups",
        ):
            (staging / relative).mkdir(parents=True, exist_ok=True)
        (staging / "takaro" / "PINNED.txt").write_text(PINNED_NOTE, encoding="utf-8")

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """Place the DLL the game loads; there is nothing else to deploy."""
        role = str(component["role"])
        if role != "server-plugin":
            raise ConflictError(f"Enshrouded ships one component, server-plugin; refusing role '{role}'")
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        with zipfile.ZipFile(artifact) as archive:
            names = self._checked_names(archive, artifact, ZIP_FOLDER)
            self._place_dll(archive, artifact, install_dir, names)
        for stale in sorted(install_dir.glob("takaro-enshrouded-plugin-*.zip")):
            if stale.name != artifact.name:
                stale.unlink()

    def _checked_names(self, archive: zipfile.ZipFile, artifact: Path, folder: str) -> list[str]:
        """Every entry, refused unless it lives inside the single expected top-level folder.

        This checks zip entries rather than record-supplied install paths, so it does not use
        ``paths.safe_relative``, which requires every segment to start alphanumerically and
        is stricter than a zip entry needs to be. What has to hold here is containment --
        nothing absolute, no backslash a Windows-built archive might smuggle in, and no
        segment that climbs back out of the folder.
        """
        names: list[str] = []
        for name in archive.namelist():
            relative = name.rstrip("/")
            if not relative:
                continue
            escapes = (
                relative.startswith("/")
                or "\\" in relative
                or any(segment in ("", ".", "..") for segment in relative.split("/"))
            )
            if escapes or not relative.startswith(f"{folder}/"):
                raise ConflictError(
                    f"{artifact.name} holds '{name}', outside the single {folder}/ folder; nothing was extracted"
                )
            names.append(relative)
        return names

    def _place_dll(self, archive: zipfile.ZipFile, artifact: Path, install_dir: Path, names: list[str]) -> None:
        """The one file the game loads, written atomically so a boot never sees half of it."""
        folder = ZIP_FOLDER
        entry = f"{folder}/{PLUGIN_DLL}"
        if entry not in names:
            raise ConflictError(f"{artifact.name} holds no {entry}; nothing was extracted")
        install_dir.mkdir(parents=True, exist_ok=True)
        destination = install_dir / PLUGIN_DLL
        staged = destination.with_name(destination.name + ".tmp")
        staged.write_bytes(archive.read(entry))
        os.chmod(staged, 0o644)
        os.replace(staged, destination)
        readme = f"{folder}/README.txt"
        if readme in names:
            first = archive.read(readme).decode("utf-8", errors="replace").splitlines()
            if first:
                output.info(f"placed {PLUGIN_DLL} from {first[0].strip()}")


GAME = EnshroudedAdapter()
