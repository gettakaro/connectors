"""The Valheim adapter: a Steam-delivered server, a pinned BepInEx pack, one server plugin.

Valheim differs from the other Steam game in this catalog in three ways, and each one is a
method here.

*Two pinned inputs, not one.* The server arrives as Steam depots; the mod loader arrives as
a Thunderstore package that has to be unpacked **into** that server directory before the
game will load anything. So the exact install is the depot set plus the pack, and both are
recorded in the ledger — a replaced ``BepInEx/core/BepInEx.dll`` is as much a drifted
install as a replaced ``valheim_server.x86_64``.

*One artifact role.* The connector is a dedicated-server BepInEx plugin and nothing else:
there is no client-side component. The one zip is unpacked into ``BepInEx/plugins`` on
deploy, and any other role a record names is refused.

*A runtime image that installs things.* The image used for verification can fetch the game
with SteamCMD and the pack from Thunderstore's ``latest`` on every boot. Both are switched
off by the target's ``runtime.container.env`` and kept off by the pre-installed files this
adapter puts there; ``runtime_env`` never adds a download switch of its own.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import zipfile
from collections.abc import Callable
from pathlib import Path, PurePosixPath
from typing import Any

from ... import net, output, paths
from ...catalog import ids
from ...exit_codes import OK, BuildFailed, ConflictError, IntegrityError
from ...install.ledger import read_ledger, write_ledger
from ...providers import provider_for
from ...steam import install as steam_install
from ..base import BaseAdapter, BuildResult, common_env

GAME_ID = "valheim"
REFERENCES_ROOT = "games/valheim/_data/references"
DEPS_ROOT = "games/valheim/_data/deps/bepinex"
DIST_ROOT = "games/valheim/_data/dist"
BUILD_SCRIPT = "games/valheim/scripts/build-release.sh"

#: The one folder of the Thunderstore pack whose contents belong next to the server binary.
#: Everything else in the zip (``manifest.json``, ``README.md``, ``icon.png``) is
#: Thunderstore packaging and is deliberately not installed.
PACK_FOLDER = "BepInExPack_Valheim"

#: The plugin folder the dedicated server loads, and the only path a server artifact may write.
PLUGIN_FOLDER = "TakaroValheim"

#: BepInEx caches the type metadata of every plugin assembly. A deterministic rebuild can
#: produce a same-size DLL, so the cache is dropped whenever the plugin is replaced.
CHAINLOADER_CACHE = Path("BepInEx") / "cache" / "chainloader_typeloader.dat"

#: The assembly whose hash identifies the game build a plugin was compiled against.
ASSEMBLY_VALHEIM = "valheim_server_Data/Managed/assembly_valheim.dll"

# What the server and the loader write about themselves. Both are anchored on their own
# prefix, so a line about some *other* version -- a world file's, say -- is never mistaken
# for the server's, and the two are merged by the hooks into one runtime identity.
_GAME_BANNER = re.compile(
    r"Valheim version:\s*(?:l-)?(?P<version>[0-9][0-9.]*)(?:\s*\(network version (?P<network>[0-9]+)\))?"
)
_LOADER_BANNER = re.compile(r"BepInEx (?P<loader>[0-9][0-9.]+) - valheim_server")


def _env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


def _pack_input(resolved: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    """The one ``thunderstore-package`` input this target pins, by name."""
    found = [(name, spec) for name, spec in resolved["inputs"].items() if spec.get("kind") == "thunderstore-package"]
    if len(found) != 1:
        raise ConflictError(
            f"target '{resolved['id']}' declares {len(found)} thunderstore-package inputs; exactly one is supported"
        )
    return found[0]


def pack_file_name(spec: dict[str, Any]) -> str:
    return f"{spec['namespace']}-{spec['name']}-{spec['version']}.zip"


def _safe_zip_entry(value: str, *, field: str) -> PurePosixPath:
    """An entry name out of a third-party zip, checked before anything joins it to a directory.

    ``paths.safe_relative`` is the grammar for a path a *catalog record* may name, and it
    refuses a segment starting with a dot. That is right for a record and wrong here: the
    BepInEx pack legitimately ships ``.doorstop_version``, and dropping it would make the
    install something other than the pack. So this keeps the part that matters — nothing may
    escape the staging directory — and allows a dotfile.

    An entry that does escape is an `IntegrityError`, not a usage error: nobody typed it.
    It is a third party's archive saying something about the bytes that arrived, which is
    exactly what exit 5 means, and it leaves the existing install untouched.
    """
    text = str(value)
    segments = text.split("/")
    unsafe = (
        not text
        or text.startswith("/")
        or "\\" in text
        or any(segment in ("", ".", "..") for segment in segments)
        or any("\x00" in segment for segment in segments)
        or re.fullmatch(r"[A-Za-z]:.*", text) is not None
    )
    if unsafe:
        raise IntegrityError(
            f"{field} must be a relative path inside the install directory "
            f"(no leading '/', no '..', no backslash), not {value!r}"
        )
    return PurePosixPath(text)


class ValheimAdapter(BaseAdapter):
    id = GAME_ID

    # -- description ----------------------------------------------------------
    def env(self, resolved: dict[str, Any], prefix: str) -> dict[str, str]:
        """What the scripts, the rig and CI read. No ``_JAVA``: this server ships its own runtime."""
        server = resolved["inputs"]["server"]
        pack_name, pack = _pack_input(resolved)
        depots = ";".join(f"{depot}:{server['depots'][depot]['manifest']}" for depot in sorted(server["depots"]))
        artifacts = resolved["artifactFileNames"]
        env = {
            **common_env(resolved, prefix),
            f"{prefix}_TOOLCHAIN": str(resolved["toolchainRef"]),
            f"{prefix}_REVISION": str(resolved["revision"]),
            f"{prefix}_STEAM_APP": str(server["app"]),
            f"{prefix}_STEAM_BRANCH": str(server["branch"]),
            f"{prefix}_STEAM_BUILDID": str(server["buildid"]),
            f"{prefix}_STEAM_DEPOTS": depots,
            f"{prefix}_ARTIFACT_SERVER_PLUGIN": str(artifacts["server-plugin"]),
            f"{prefix}_REFERENCES_DIR": f"{REFERENCES_ROOT}/{resolved['fp16']}",
            f"{prefix}_BEPINEX_DIR": f"{DEPS_ROOT}/{resolved['fp16']}",
            f"{prefix}_BEPINEX_PACKAGE": f"{pack['namespace']}/{pack['name']}",
            f"{prefix}_BEPINEX_PACK_VERSION": str(pack["version"]),
            f"{prefix}_BEPINEX_SHA256": str(pack["sha256"]),
            f"{prefix}_BEPINEX_FILE": pack_file_name(pack),
        }
        url = resolved.get("resolvedUrls", {}).get(pack_name)
        if url:
            env[f"{prefix}_BEPINEX_URL"] = str(url)
        if pack.get("size") is not None:
            env[f"{prefix}_BEPINEX_SIZE"] = str(pack["size"])
        for path, spec in sorted(server["files"].items()):
            if "/Managed/" not in path or not path.lower().endswith(".dll"):
                continue
            key = _env_key(Path(path).stem)
            digest = spec.get("sha256")
            if digest:
                env[f"{prefix}_REFERENCE_{key}_SHA256"] = str(digest)
                if path == ASSEMBLY_VALHEIM:
                    env[f"{prefix}_ASSEMBLY_VALHEIM_SHA256"] = str(digest)
        # The dependency URLs and hashes the build verifies before it uses them.
        for name, dep in sorted(resolved["build"]["deps"].items()):
            key = _env_key(name)
            env[f"{prefix}_DEP_{key}_URL"] = str(dep.get("resolvedCoordinate", dep["coordinate"]))
            env[f"{prefix}_DEP_{key}_SHA256"] = str(dep["sha256"])
        return env

    def preserve_globs(self, resolved: dict[str, Any]) -> list[str]:
        return list(resolved.get("preserve", []))

    def parse_runtime_identity(self, log_line: str) -> dict[str, Any] | None:
        """The game's own version banner, or the loader's. The hooks merge the two."""
        game = _GAME_BANNER.search(log_line)
        if game:
            return {"gameVersion": game.group("version"), "loader": "bepinex", "loaderVersion": None}
        loader = _LOADER_BANNER.search(log_line)
        if loader:
            return {"loader": "bepinex", "loaderVersion": loader.group("loader")}
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
        """Run the tracked release script, inside the pinned .NET SDK image.

        The ``toolchain`` argument is accepted for parity with the Gradle games and does not
        select ``host``: ``host`` is the CLI's *default*, and most machines that run this —
        CI runners, the rig, this repository's containers — have no .NET SDK, so honouring
        it would be a promise this adapter cannot keep. A developer who does have the SDK
        (plus zip, unzip, jq, rg and file) asks for an in-place build explicitly, with
        ``VALHEIM_BUILD_TOOLCHAIN=host``. ``gradle_args`` mean nothing to a script build;
        determinism comes from ``SOURCE_DATE_EPOCH`` and a clean stage.
        """
        del toolchain, gradle_args
        dist = repo_root / DIST_ROOT / resolved["fp16"]
        dist.mkdir(parents=True, exist_ok=True)
        environment = dict(os.environ)
        environment["TAKARO_MAINT_REPO_ROOT"] = str(repo_root)
        environment["VALHEIM_BUILD_TOOLCHAIN"] = os.environ.get("VALHEIM_BUILD_TOOLCHAIN") or "container"
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
        output.info(f"building {resolved['id']} {version} ({environment['VALHEIM_BUILD_TOOLCHAIN']} toolchain)")
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
        """The image's own environment. The connector reads a BepInEx config file, never the environment.

        The password is required by the game (at least five characters, and never a
        substring of the server name) and is worth nothing: the run publishes no host port.
        """
        del takaro
        return {
            **{str(k): str(v) for k, v in resolved["runtime"]["container"].get("env", {}).items()},
            "NAME": "takaro-verify",
            "WORLD": "takaro-verify",
            "PASSWORD": "takaro-maint-check",
            "PUID": str(os.getuid()),
            "PGID": str(os.getgid()),
        }

    def container_mounts(self, resolved: dict[str, Any], data_dir: Path) -> list[str]:
        """The install, plus the saves and backups the image writes outside it.

        Valheim keeps worlds in the user's home rather than in the game directory, so
        ``--reuse-world`` / ``--fresh-world`` change nothing here: a run's world lives under
        ``.takaro/runtime``, which ``preserve`` keeps across installs.
        """
        del resolved
        runtime = data_dir / ".takaro" / "runtime"
        (runtime / "saves").mkdir(parents=True, exist_ok=True)
        (runtime / "backups").mkdir(parents=True, exist_ok=True)
        return [
            f"{data_dir}:/home/steam/valheim",
            f"{runtime / 'saves'}:/home/steam/.config/unity3d/IronGate/Valheim",
            f"{runtime / 'backups'}:/home/steam/backups",
        ]

    # -- install --------------------------------------------------------------
    def install(self, catalog: Any, target: Any, resolved: dict[str, Any], args: Any) -> int | None:
        """The whole installation is one depot set plus one pinned pack, so the adapter owns it."""
        dest = Path(args.dest).expanduser().resolve()
        cache = paths.cache_dir()
        log = cache / "steam" / "logs" / f"{target.game}-{target.id}.log"

        if getattr(args, "rollback", False):
            document = steam_install.rollback(target, dest=dest)
            output.emit("install", True, **document)
            return OK

        if getattr(args, "reuse_world", False) or getattr(args, "fresh_world", False):
            output.info("Valheim keeps its worlds outside the install directory; the world flags change nothing")
        game_record = catalog.game(target.game).record
        document = steam_install.install_exact(
            target,
            dest=dest,
            preserve=self.preserve_globs(resolved),
            cache=cache,
            log=log,
            dry_run=bool(getattr(args, "dry_run", False)),
            post_install=self._post_install(resolved, game_record, cache),
        )
        if document["status"] == "installed":
            document["inputs"] = self._record_pack_in_ledger(dest, resolved)
        output.emit("install", True, **document)
        return OK

    def _post_install(
        self, resolved: dict[str, Any], game_record: dict[str, Any], cache: Path
    ) -> Callable[[Path], None]:
        """Unpack the pinned BepInEx pack into a staged install, or fail before the swap.

        Everything here runs against the staging directory, so a pack that cannot be
        downloaded, does not hash as recorded or is not the version the target pins leaves
        the install that is in service byte-identical.
        """
        name, spec = _pack_input(resolved)
        source = ids.source_of(game_record, str(spec["source"]))

        def post_install(staging: Path) -> None:
            archive_path = staging / ".takaro" / "inputs" / pack_file_name(spec)
            provider_for(str(source["provider"])).fetch_input(spec, source, archive_path, cache)
            self._unpack(archive_path, staging, spec, name)
            for folder in ("config", "plugins", "patchers"):
                (staging / "BepInEx" / folder).mkdir(parents=True, exist_ok=True)

        return post_install

    def _unpack(self, archive_path: Path, staging: Path, spec: dict[str, Any], name: str) -> None:
        with zipfile.ZipFile(archive_path) as archive:
            entries = [entry for entry in archive.namelist() if not entry.endswith("/")]
            for entry in entries:
                _safe_zip_entry(entry, field=f"inputs.{name} zip entry")
            if "manifest.json" not in entries:
                raise IntegrityError(
                    f"{archive_path.name} has no manifest.json at its root; it is not a Thunderstore package",
                    target=str(spec["version"]),
                )
            declared = str(json.loads(archive.read("manifest.json")).get("version_number") or "")
            if declared != str(spec["version"]):
                raise IntegrityError(
                    f"{archive_path.name} says it is {declared or '<unversioned>'}, "
                    f"the target pins {spec['version']}; the existing install is untouched",
                    target=str(spec["version"]),
                )
            prefix = f"{PACK_FOLDER}/"
            members = [entry for entry in entries if entry.startswith(prefix)]
            if not members:
                raise IntegrityError(f"{archive_path.name} holds no {prefix} folder", target=str(spec["version"]))
            for entry in members:
                relative = _safe_zip_entry(entry[len(prefix) :], field=f"inputs.{name} zip entry")
                destination = staging / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as handle, destination.open("wb") as out:
                    shutil.copyfileobj(handle, out)
                # The pack ships the loader's shell wrappers; the image runs the binary
                # through doorstop, but a human reading the directory expects them runnable.
                if relative.suffix == ".sh":
                    destination.chmod(0o755)
        core = staging / "BepInEx" / "core" / "BepInEx.dll"
        if not core.is_file():
            raise IntegrityError(
                f"{archive_path.name} unpacked without BepInEx/core/BepInEx.dll; the pack layout changed",
                target=str(spec["version"]),
            )
        output.info(f"installed {PACK_FOLDER} {spec['version']} from {archive_path.name}")

    def _record_pack_in_ledger(self, dest: Path, resolved: dict[str, Any]) -> list[dict[str, Any]]:
        """Add the pack's rows to the ledger the Steam install just wrote.

        The ledger is what ``ledger check`` and the verification ``stop`` check re-hash, so
        the loader has to be in it: a swapped ``BepInEx/core/BepInEx.dll`` is drift exactly
        as much as a swapped game binary.
        """
        name, spec = _pack_input(resolved)
        ledger = read_ledger(dest)
        if ledger is None:  # pragma: no cover - install_exact always writes one
            raise ConflictError(f"{dest} has no ledger after a successful install")
        data = dict(ledger.data)
        rows = list(data.get("inputs", []))
        for relative in (f".takaro/inputs/{pack_file_name(spec)}", "BepInEx/core/BepInEx.dll"):
            path = dest / relative
            digests = net.hash_file(path)
            rows.append(
                {
                    "name": f"{name}:{relative.rsplit('/', 1)[-1]}",
                    "path": relative,
                    "sha256": digests["sha256"],
                    "size": int(digests["size"]),
                }
            )
        data["inputs"] = rows
        write_ledger(dest, data)
        return rows

    # -- deploy ---------------------------------------------------------------
    def after_deploy(self, dest: Path, component: dict[str, Any], artifact: Path) -> None:
        """Unpack the server plugin where BepInEx loads it.

        ``server-plugin`` is the only role Valheim ships; any other role is refused before
        anything is written, so a stray record cannot put foreign assemblies on the
        dedicated server's plugin search path.
        """
        install_dir = dest / paths.safe_relative(component["installDir"], field="components[].installDir")
        role = str(component["role"])
        if role != "server-plugin":
            raise ConflictError(f"Valheim ships only a server-plugin artifact, not '{role}'; nothing was deployed")

        folder = install_dir / PLUGIN_FOLDER
        install_dir.mkdir(parents=True, exist_ok=True)
        # Unpacked beside the install rather than over it: a CRC error, a full disk or an
        # interrupt part-way through the extraction would otherwise leave a half-written
        # plugin folder in place of the working one, with the ledger still naming an
        # artifact that is no longer there. The staging area is outside BepInEx/plugins so
        # a crash cannot leave the chainloader a second copy of the assemblies to load.
        staging_root = dest / ".takaro" / "deploy"
        staging_root.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=f"{PLUGIN_FOLDER}.", dir=staging_root))
        try:
            with zipfile.ZipFile(artifact) as archive:
                for name in archive.namelist():
                    relative = name.rstrip("/")
                    if not relative:
                        continue
                    if not relative.startswith(f"{PLUGIN_FOLDER}/"):
                        raise ConflictError(
                            f"{artifact.name} holds '{name}', outside the single {PLUGIN_FOLDER}/ folder; "
                            "nothing was extracted"
                        )
                    paths.safe_relative(relative, field="artifact zip entry")
                try:
                    archive.extractall(staging)
                except (zipfile.BadZipFile, OSError) as exc:
                    raise ConflictError(
                        f"{artifact.name} could not be unpacked ({exc}); the installed {PLUGIN_FOLDER} is untouched"
                    ) from exc
            unpacked = staging / PLUGIN_FOLDER
            if not unpacked.is_dir():
                raise ConflictError(f"{artifact.name} unpacked without a {PLUGIN_FOLDER}/ folder; nothing was replaced")
            # The previous folder moves aside first and is only dropped once the new one is
            # in place, so the swap has no window in which neither exists.
            previous = staging / f"{PLUGIN_FOLDER}.previous"
            replaced = folder.exists()
            if replaced:
                folder.rename(previous)
            try:
                unpacked.rename(folder)
            except OSError:
                if replaced:
                    previous.rename(folder)
                raise
        finally:
            shutil.rmtree(staging, ignore_errors=True)
        cache = dest / CHAINLOADER_CACHE
        if cache.is_file():
            cache.unlink()
            output.info(f"cleared {CHAINLOADER_CACHE.as_posix()} so BepInEx re-reads the replaced plugin")
        for stale in sorted(install_dir.glob("takaro-valheim-plugin-*.zip")):
            if stale.name != artifact.name:
                stale.unlink()
        output.info(f"unpacked {PLUGIN_FOLDER} into {component['installDir']}/")


GAME = ValheimAdapter()
