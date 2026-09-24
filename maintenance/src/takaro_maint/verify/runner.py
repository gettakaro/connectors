"""Driving one verification run: install, deploy, boot a container, ask it questions.

The container publishes no host ports. The fake Takaro binds the docker bridge gateway,
so the server reaches it through ``host.docker.internal`` and nothing else can.
"""

from __future__ import annotations

import asyncio
import contextlib
import datetime as dt
import importlib
import json
import os
import re
import secrets
import shlex
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

from .. import output, paths, redact
from ..catalog.loader import Catalog, Target, resolve
from ..exit_codes import UpstreamUnavailable, UsageError
from ..games import adapter_for
from ..install.ledger import read_ledger
from ..publish import read_manifest
from . import checks as base_checks
from .fake_takaro import FakeTakaro
from .hooks import GameHooks
from .report import build_report, write_report

CHECK_IDS = (
    "build",
    "startup",
    "connector-load",
    "identify",
    "heartbeat",
    "players",
    "catalog-items",
    "catalog-entities",
    "console",
    "shutdown",
)


def game_hooks(game: str) -> GameHooks:
    """What this game contributes to a run, as one declared object.

    The hooks module is looked up next to the adapter rather than at a path spelled out
    here, so a game whose package is not named after its catalog id is found too. A game
    without one verifies with the base checks alone, which is what the empty ``GameHooks``
    means; a module that ships hooks but never assembles them into ``HOOKS`` is a mistake,
    not a game with no hooks, so it is refused rather than quietly ignored.
    """
    try:
        adapter = adapter_for(game)
    except UsageError:
        return GameHooks()
    try:
        module = importlib.import_module(f"{type(adapter).__module__}.verify")
    except ModuleNotFoundError:
        return GameHooks()
    hooks = getattr(module, "HOOKS", None)
    if not isinstance(hooks, GameHooks):
        raise UsageError(f"{module.__name__} ships verification hooks but no HOOKS = GameHooks(...)")
    unsupported = set(hooks.unsupported_checks)
    own_exclusions = unsupported & set(hooks.check_ids)
    if own_exclusions:
        names = ", ".join(sorted(own_exclusions))
        raise UsageError(f"{game} excludes its own verification check(s): {names}")
    unknown = unsupported - set(CHECK_IDS)
    if unknown:
        names = ", ".join(sorted(unknown))
        raise UsageError(f"{game} excludes check(s) outside the base verification ladder: {names}")
    conditional = set(hooks.negative_check_ids) | set(hooks.hosted_check_ids)
    undeclared = conditional - set(hooks.check_ids)
    if undeclared:
        names = ", ".join(sorted(undeclared))
        raise UsageError(f"{game} marks undeclared verification check(s) as conditional: {names}")
    return hooks


def check_ids(game: str | None = None) -> tuple[str, ...]:
    """The base check ids, plus the ones the game's own hooks add."""
    hooks = game_hooks(game) if game else GameHooks()
    return (*CHECK_IDS, *hooks.check_ids)


def docker_command() -> list[str]:
    return shlex.split(os.environ.get("TAKARO_MAINT_DOCKER", "docker"))


def bridge_gateway() -> str:
    """The address a container reaches the host on. Falls back to the documented default."""
    try:
        result = subprocess.run(
            [*docker_command(), "network", "inspect", "bridge", "-f", "{{(index .IPAM.Config 0).Gateway}}"],
            capture_output=True,
            text=True,
            check=True,
        )
        gateway = result.stdout.strip()
        if gateway:
            return gateway
    except (OSError, subprocess.CalledProcessError) as exc:
        output.warn(f"could not read the docker bridge gateway ({exc}); binding 172.17.0.1")
    return "172.17.0.1"


def _command_env_secrets(argv: list[str]) -> list[str]:
    """Find secret-valued `docker run -e KEY=value` arguments for kept logs."""
    environment: dict[str, str] = {}
    for index, argument in enumerate(argv[:-1]):
        if argument in ("-e", "--env"):
            key, separator, value = argv[index + 1].partition("=")
            if separator:
                environment[key] = value
    return redact.secret_values(environment)


@dataclass
class Container:
    """One game server container, its log file and its lifecycle."""

    name: str
    argv: list[str]
    log_file: Path
    docker_log: Path
    secrets: list[str] = field(default_factory=list)
    removed: bool = field(default=False, init=False)
    _follower: subprocess.Popen[bytes] | None = field(default=None, init=False)

    def start(self) -> None:
        self.docker_log.parent.mkdir(parents=True, exist_ok=True)
        with self.docker_log.open("a", encoding="utf-8") as handle:
            # The registration token is on this command line; the kept log may not carry it.
            handle.write(redact.redact(" ".join(shlex.quote(part) for part in self.argv), self.secrets) + "\n")
        result = subprocess.run(self.argv, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise UpstreamUnavailable(f"docker run failed: {redact.redact(result.stderr.strip(), self.secrets)}")
        self.log_file.parent.mkdir(parents=True, exist_ok=True)
        log_handle = self.log_file.open("wb")
        self._follower = subprocess.Popen(
            [*docker_command(), "logs", "-f", self.name],
            stdout=log_handle,
            stderr=subprocess.STDOUT,
        )

    def inspect_port_bindings(self) -> str:
        result = subprocess.run(
            [*docker_command(), "inspect", "-f", "{{json .HostConfig.PortBindings}}", self.name],
            capture_output=True,
            text=True,
            check=False,
        )
        return result.stdout.strip()

    def alive(self) -> bool:
        # A removed container is dead by definition, and asking docker about it would
        # keep every check that polls `alive` waiting out its whole budget after an
        # interrupt has already torn the run down.
        if self.removed:
            return False
        result = subprocess.run(
            [*docker_command(), "inspect", "-f", "{{.State.Running}}", self.name],
            capture_output=True,
            text=True,
            check=False,
        )
        return result.stdout.strip() == "true"

    def wait_for_exit(self, timeout: float) -> int:
        deadline = time.monotonic() + timeout
        while not self.removed and time.monotonic() < deadline:
            result = subprocess.run(
                [*docker_command(), "inspect", "-f", "{{.State.Running}}|{{.State.ExitCode}}", self.name],
                capture_output=True,
                text=True,
                check=False,
            )
            running, _, code = result.stdout.strip().partition("|")
            if running == "false":
                try:
                    return int(code)
                except ValueError:
                    return -1
            time.sleep(2)
        return -1

    def remove(self) -> None:
        self.removed = True
        if self._follower is not None:
            self._follower.terminate()
            try:
                self._follower.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._follower.kill()
        with self.docker_log.open("a", encoding="utf-8") as handle:
            handle.write(f"rm -f {self.name}\n")
        subprocess.run([*docker_command(), "rm", "-f", self.name], capture_output=True, check=False)


#: Label keys the harness sets itself: the run id is what `--cleanup-orphans` and the CI
#: `docker rm` step filter on, the TTL is the abandoned-container safety net. Docker keeps the
#: last value of a repeated key, so a caller-supplied one would silently replace them.
RESERVED_LABELS: frozenset[str] = frozenset({"tm.run", "tm.ttl"})


def cleanup_orphans(run_id: str) -> list[str]:
    result = subprocess.run(
        [*docker_command(), "ps", "-aq", "--filter", f"label=tm.run={run_id}"],
        capture_output=True,
        text=True,
        check=False,
    )
    names = [line for line in result.stdout.split() if line]
    for name in names:
        subprocess.run([*docker_command(), "rm", "-f", name], capture_output=True, check=False)
    return names


def capture_ark_diagnostics(containers: list[Container], data_dir: Path, out: Path) -> list[Path]:
    """Keep crash evidence before Docker removal and owned-data cleanup."""
    captured_at = dt.datetime.now(dt.UTC).isoformat().replace("+00:00", "Z")
    files: list[Path] = []
    states: list[dict[str, Any]] = []
    for container in containers:
        state = subprocess.run(
            [*docker_command(), "inspect", "-f", "{{json .State}}", container.name],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        entry: dict[str, Any] = {"name": container.name, "capturedAt": captured_at}
        if state.returncode == 0:
            try:
                entry["state"] = json.loads(state.stdout)
            except json.JSONDecodeError:
                entry["inspectError"] = "Docker returned malformed State JSON"
        else:
            entry["inspectError"] = state.stderr.strip()[:500]
        states.append(entry)
        stamped = subprocess.run(
            [*docker_command(), "logs", "--timestamps", "--tail", "10000", container.name],
            capture_output=True,
            text=True,
            check=False,
            timeout=20,
        )
        log_path = out / f"{container.name}-timestamped.log"
        log_path.write_text(redact.redact(stamped.stdout + stamped.stderr, container.secrets), encoding="utf-8")
        files.append(log_path)

    saved = data_dir / "ShooterGame" / "Saved"
    saved_files: list[dict[str, Any]] = []
    copied_bytes = 0
    for folder in (saved / "Logs", saved / "Crashes"):
        if folder.is_symlink() or not folder.is_dir():
            continue
        for source in sorted(folder.rglob("*")):
            if len(saved_files) >= 64:
                break
            if source.is_symlink() or not source.is_file():
                continue
            relative = source.relative_to(saved)
            size = source.stat().st_size
            row: dict[str, Any] = {"file": str(relative), "size": size, "copied": False}
            if size <= 16 * 1024 * 1024 and copied_bytes + size <= 32 * 1024 * 1024:
                destination = out / "owned-saved-diagnostics" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
                files.append(destination)
                copied_bytes += size
                row["copied"] = True
            saved_files.append(row)
    world = saved / "SavedArks" / "TheIsland.ark"
    if world.is_file() and not world.is_symlink():
        saved_files.append({"file": "SavedArks/TheIsland.ark", "size": world.stat().st_size, "copied": False})
    evidence = out / "ark-cleanup-diagnostics.json"
    evidence.write_text(
        json.dumps({"capturedAt": captured_at, "containers": states, "ownedSavedFiles": saved_files}, indent=2) + "\n",
        encoding="utf-8",
    )
    files.append(evidence)
    return files


@dataclass
class RunOptions:
    artifacts: Path
    out: Path
    run_id: str
    labels: list[str] = field(default_factory=list)
    startup_timeout: float | None = None
    only: list[str] | None = None
    keep_on_failure: bool = False
    negative: bool = False
    takaro: str = "local"
    ark_readonly_base: Path | None = None


class TargetRun:
    """Everything one target's verification needs, and its guaranteed cleanup."""

    def __init__(self, catalog: Catalog, target: Target, options: RunOptions) -> None:
        self.catalog = catalog
        self.target = target
        self.options = options
        self.resolved = resolve(catalog, target)
        self.adapter = adapter_for(target.game)
        self.hooks = game_hooks(target.game)
        self._checks_explicit = options.only is not None
        self.out = options.out / target.id
        self.out.mkdir(parents=True, exist_ok=True)
        self.server_log = self.out / "server.log"
        self.docker_log = self.out / "docker.log"
        self.fake_log = self.out / "fake-takaro.log"
        self.data_dir = Path(tempfile.mkdtemp(prefix="takaro-verify-"))
        # One throwaway token per run, reused by every boot on this data dir.
        self.registration_token = secrets.token_urlsafe(24)
        # Something short and unique per run, for names that must not repeat across runs.
        self.nonce = secrets.token_hex(3)
        self.container: Container | None = None
        self.containers: list[Container] = []
        self.extra_logs: list[Path] = []
        self.results: list[base_checks.CheckResult] = []
        self.readonly_inputs: list[dict[str, Any]] | None = None
        self.readonly_provenance: dict[str, Any] | None = None
        self._cleaned = False

    # -- setup ----------------------------------------------------------------
    def _run_command(self, name: str, argv: list[str]) -> None:
        """Run a sub-command, keeping its JSON out of this run's single stdout document.

        The sub-command parses its own arguments, so the repo root is passed on explicitly:
        without it the process-wide root is reset and the target is resolved from a different
        catalog than the one this run was asked about.
        """
        from ..cli import main as cli_main

        record = self.out / f"{name}.json"
        with record.open("w", encoding="utf-8") as handle, contextlib.redirect_stdout(handle):
            code = cli_main(["--quiet", "--repo-root", str(paths.repo_root()), *argv])
        if code != 0:
            raise UpstreamUnavailable(f"{name} into the verification data dir exited {code}; see {record.name}")

    def install_and_deploy(self) -> dict[str, Any]:
        manifest_path = self.options.artifacts / "build-manifest.json"
        if self.options.ark_readonly_base is not None:
            if self.hooks.prepare_readonly_base is None:
                raise UsageError(f"game '{self.target.game}' has no read-only base verification mode")
            manifest = read_manifest(manifest_path)
            self.readonly_inputs, self.readonly_provenance = self.hooks.prepare_readonly_base(self, manifest)
            return manifest
        self._run_command(
            "install",
            ["install", "--game", self.target.game, "--target", self.target.id, "--dest", str(self.data_dir)],
        )
        self._run_command(
            "deploy",
            [
                "deploy",
                "--game",
                self.target.game,
                "--target",
                self.target.id,
                "--dest",
                str(self.data_dir),
                "--from",
                str(manifest_path),
            ],
        )
        return read_manifest(manifest_path)

    def takaro_env(self, ws_url: str, extra_env: dict[str, str] | None = None) -> dict[str, str]:
        """What this run tells the connector about the Takaro it should talk to."""
        return {
            "TAKARO_WS_URL": ws_url,
            "TAKARO_IDENTITY_TOKEN": f"takaro-verify-{self.options.run_id}",
            "TAKARO_REGISTRATION_TOKEN": self.registration_token,
            **(extra_env or {}),
        }

    def container_mounts(self) -> list[str]:
        """The ``-v`` arguments this game's server needs; one data dir bound at /data by default."""
        if self.options.ark_readonly_base is not None:
            return [
                f"{self.options.ark_readonly_base}:/ark-base:ro",
                f"{self.data_dir}:/ark:rw",
            ]
        return [str(mount) for mount in self.adapter.container_mounts(self.resolved, self.data_dir)]

    def ready_line(self) -> re.Pattern[str]:
        """The log line that says this game's server finished booting."""
        return self.hooks.ready_line

    @property
    def startup_timeout(self) -> float:
        """The explicit CLI budget, then the game's own budget, then the common default."""
        if self.options.startup_timeout is not None:
            return self.options.startup_timeout
        return self.hooks.startup_timeout or 300.0

    def container_argv(self, ws_url: str, *, suffix: str = "", extra_env: dict[str, str] | None = None) -> list[str]:
        takaro_env = self.takaro_env(ws_url, extra_env)
        environment = {
            "UID": str(os.getuid()),
            "GID": str(os.getgid()),
            "EULA": "TRUE",
            "ONLINE_MODE": "false",
            "VIEW_DISTANCE": "4",
            "MEMORY": "2G",
            "LEVEL": "takaro-verify",
            **self.adapter.runtime_env(self.resolved, takaro_env),
        }
        ttl = int(time.time()) + 3 * 3600
        name = f"takaro-verify-{self.target.game}-{self.target.id}-{self.options.run_id}{suffix}"
        argv = [
            *docker_command(),
            "run",
            "-d",
            "--name",
            name,
            "--label",
            f"tm.run={self.options.run_id}",
            "--label",
            f"tm.ttl={ttl}",
        ]
        for label in self.options.labels:
            argv += ["--label", label]
        argv += ["--add-host", "host.docker.internal:host-gateway", "--memory", "3g"]
        # A game that needs more than the run's own defaults appends them here -- docker
        # takes the last value of a repeated option, so these win over what is above.
        argv += [str(option) for option in self.adapter.container_options(self.resolved, self.data_dir)]
        if self.options.ark_readonly_base is not None:
            argv += ["--memory", "12g"]
        for key, value in sorted(environment.items()):
            argv += ["-e", f"{key}={value}"]
        for mount in self.container_mounts():
            argv += ["-v", mount]
        argv += [self.resolved["containerRef"]]
        # Some servers take settings the image exposes no environment variable for -- a world
        # name on the command line, say -- so a game may append its own arguments after the
        # image reference. Games that need none ship no hook and get the image's own Cmd.
        command = self.adapter.container_command(self.resolved, self.data_dir)
        if command is not None:
            argv += [str(part) for part in command]
        self.container_name = name
        return argv

    def boot(
        self,
        ws_url: str,
        *,
        suffix: str = "",
        log_name: str = "server.log",
        extra_env: dict[str, str] | None = None,
    ) -> Container:
        """Start one more container on this run's data dir.

        The caller drives its lifecycle; ``cleanup()`` removes it either way.
        """
        argv = self.container_argv(ws_url, suffix=suffix, extra_env=extra_env)
        container = Container(
            name=self.container_name,
            argv=argv,
            log_file=self.out / log_name,
            docker_log=self.docker_log,
            secrets=[self.registration_token, *(extra_env or {}).values(), *_command_env_secrets(argv)],
        )
        # Registered before it is started, not after: `docker run` has created the container
        # by the time `start()` returns, and an interrupt arriving during `start()` or the
        # port-binding inspection would otherwise find an empty list and leak it.
        self.containers.append(container)
        self.container = container
        # Anything the server has to find on disk before it starts -- a config file the
        # game reads instead of the environment, say -- is written here.
        if self.hooks.before_boot is not None:
            self.hooks.before_boot(self, self.takaro_env(ws_url, extra_env))
        container.start()
        bindings = container.inspect_port_bindings()
        with self.docker_log.open("a", encoding="utf-8") as handle:
            handle.write(f"HostConfig.PortBindings={bindings}\n")
        # Anything that can only exist once the container does -- a sidecar joining its network
        # namespace, say -- is started here, while `before_boot` still runs against the disk.
        if self.hooks.after_boot is not None:
            self.hooks.after_boot(self, container, self.takaro_env(ws_url, extra_env))
        return container

    # -- checks ---------------------------------------------------------------
    def _select_default_checks(self) -> None:
        """A run that named no ``--checks`` skips what this game cannot pass.

        The base ladder is written for the Minecraft connector, and most of it looks for
        things the other seven games do not have -- a load line only that connector
        writes, Minecraft item and entity spot values, an exit code a Unity teardown does
        not give. Left in, `takaro-maint verify --game <g>` would spend a timeout failing
        on each. Each game declares which base checks cannot apply to it and what stands
        in for them, and the exclusion happens here, once, for every game. A check the
        game implements but fails is run and reported.

        An explicit ``--checks`` is left exactly as written: naming a check is asking for
        it. ``replace`` rather than a field assignment, because one ``RunOptions`` is
        shared by every target of the command and one target's default must not narrow
        the next one's.
        """
        if self.options.only is not None:
            return
        unsupported = self.hooks.unsupported_checks
        if not unsupported:
            return
        self.options = replace(
            self.options,
            only=[
                check
                for check in check_ids(self.target.game)
                if check not in unsupported and self._wanted_by_mode(check)
            ],
        )
        for check, reason in sorted(unsupported.items()):
            output.info(f"not running {check}: {reason}")

    def _wanted_by_mode(self, check_id: str) -> bool:
        """Whether a bare run's mode selects an opt-in check."""
        if check_id in self.hooks.negative_check_ids:
            return self.options.negative
        if check_id in self.hooks.hosted_check_ids:
            return self.options.takaro == "hosted"
        return True

    def wanted(self, check_id: str) -> bool:
        if self.options.only is not None:
            return check_id in self.options.only
        return self._wanted_by_mode(check_id)

    def selected_check_ids(self) -> tuple[str, ...]:
        """Checks this invocation promised to run, including a generic negative request."""
        selected = [check for check in check_ids(self.target.game) if self.wanted(check)]
        if (
            self.options.negative
            and not self._checks_explicit
            and not self.hooks.negative_check_ids
            and "negative-wrong-target" not in selected
        ):
            selected.append("negative-wrong-target")
        return tuple(selected)

    def _record_missing_selected_checks(self) -> None:
        """Turn a hook that forgot a selected check into evidence, never a false pass."""
        recorded = {result.id for result in self.results}
        for check_id in self.selected_check_ids():
            if check_id not in recorded:
                self.record(
                    base_checks.CheckResult(
                        check_id,
                        "fail",
                        0,
                        {"problems": [f"selected check '{check_id}' produced no result"]},
                    )
                )
                recorded.add(check_id)

    def record(self, result: base_checks.CheckResult) -> None:
        self.results.append(result)
        output.info(f"  {result.status:<4} {result.id}")

    def skip(self, check_id: str, reason: str) -> None:
        self.record(base_checks.CheckResult(check_id, "skip", 0, {"reason": reason}))

    def not_selected_reason(self, check_id: str) -> str:
        """Use a game's declared reason when its default selection drops a check."""
        return self.hooks.unsupported_checks.get(check_id, "not selected by --checks")

    def _skip_after_startup_failure(self) -> None:
        """Record the unattempted ladder without turning one failed boot into many failures."""
        reason = "startup did not complete; see the startup check"
        recorded = {result.id for result in self.results}
        for check_id in (*CHECK_IDS[2:], *self.hooks.check_ids):
            if check_id not in recorded:
                self.skip(check_id, reason)
                recorded.add(check_id)
        if self.options.negative and "negative-wrong-target" not in recorded:
            self.skip("negative-wrong-target", reason)

    async def run(self) -> dict[str, Any]:
        started_at = dt.datetime.now(dt.UTC).isoformat().replace("+00:00", "Z")
        try:
            return await self._run(started_at)
        finally:
            # Install, deploy, the hosted dispatch and the fake's start run before the paths
            # below take over their own cleanup; a failure there must not leave the data dir.
            self.cleanup()

    async def _run(self, started_at: str) -> dict[str, Any]:
        self._select_default_checks()
        manifest = self.install_and_deploy()
        if self.readonly_inputs is not None:
            ledger_inputs = self.readonly_inputs
        else:
            ledger = read_ledger(self.data_dir)
            assert ledger is not None
            ledger_inputs = ledger.data["inputs"]

        if self.options.takaro == "hosted":
            if self.hooks.run_hosted is None:
                raise UsageError(f"game '{self.target.game}' ships no hosted verification")
            hosted: dict[str, Any] = await self.hooks.run_hosted(self, manifest, ledger_inputs, started_at)
            return hosted

        if self.wanted("build"):
            self.record(base_checks.check_build(self.options.artifacts, manifest, self.target))
        else:
            self.skip("build", self.not_selected_reason("build"))

        fake = FakeTakaro(host=bridge_gateway(), log_path=self.fake_log)
        port = await fake.start()
        ws_url = f"ws://host.docker.internal:{port}/"
        output.info(f"fake Takaro listening on {fake.host}:{port} (no host ports published)")

        runtime: dict[str, Any] = (
            {"readOnlyBase": self.readonly_provenance} if self.readonly_provenance is not None else {}
        )
        try:
            container = self.boot(ws_url)
            alive = container.alive
            if self.wanted("startup"):
                # Every check that polls a file or the docker CLI runs off the event loop,
                # so the fake Takaro keeps answering the connector while it does.
                self.record(
                    await asyncio.to_thread(
                        base_checks.check_startup,
                        self.server_log,
                        self.startup_timeout,
                        alive,
                        self.options.ark_readonly_base or self.data_dir,
                        ledger_inputs,
                        self.ready_line(),
                    )
                )
            else:
                self.skip("startup", self.not_selected_reason("startup"))

            startup_failed = any(result.id == "startup" and result.status == "fail" for result in self.results)
            if startup_failed:
                self._skip_after_startup_failure()
            else:
                identity = await asyncio.to_thread(self._scan_runtime_identity)
                runtime = {
                    "gameVersion": identity.get("gameVersion"),
                    "loader": identity.get("loader"),
                    "loaderVersion": identity.get("loaderVersion"),
                    "java": self.target.record["runtime"]["java"],
                }
                if self.readonly_provenance is not None:
                    runtime["readOnlyBase"] = self.readonly_provenance

                if self.wanted("connector-load"):
                    self.record(
                        await asyncio.to_thread(
                            base_checks.check_connector_load, self.server_log, self.target, 120, alive
                        )
                    )
                else:
                    self.skip("connector-load", self.not_selected_reason("connector-load"))

                if self.wanted("identify"):
                    self.record(await base_checks.check_identify(fake, self.server_log, 180, alive))
                else:
                    self.skip("identify", self.not_selected_reason("identify"))

                for check_id, coroutine in (
                    ("heartbeat", lambda: base_checks.check_heartbeat(fake)),
                    ("players", lambda: base_checks.check_players(fake)),
                    (
                        "catalog-items",
                        lambda: base_checks.check_catalog(
                            fake, "listItems", "catalog-items", ("minecraft:diamond_sword", "Diamond Sword")
                        ),
                    ),
                    (
                        "catalog-entities",
                        lambda: base_checks.check_catalog(
                            fake, "listEntities", "catalog-entities", ("minecraft:zombie", "Zombie")
                        ),
                    ),
                    (
                        "console",
                        lambda: base_checks.check_console(
                            fake, self.server_log, f"takaro-verify-{self.options.run_id}", alive
                        ),
                    ),
                ):
                    if self.wanted(check_id):
                        self.record(await coroutine())
                    else:
                        self.skip(check_id, self.not_selected_reason(check_id))

                if self.hooks.after_protocol is not None:
                    await self.hooks.after_protocol(self, fake, alive)

                if self.wanted("shutdown"):
                    self.record(await base_checks.check_shutdown(fake, container.wait_for_exit))
                else:
                    self.skip("shutdown", self.not_selected_reason("shutdown"))

                if self.hooks.after_shutdown is not None:
                    await self.hooks.after_shutdown(self, fake, ws_url, ledger_inputs)

                negative_selected = any(self.wanted(check_id) for check_id in self.hooks.negative_check_ids)
                if self.options.negative or negative_selected:
                    if self.hooks.negative is not None:
                        await self.hooks.negative(self, fake, ws_url, manifest)
                    else:
                        self.skip("negative-wrong-target", f"game '{self.target.game}' ships no negative check")
        finally:
            await fake.stop()
            self._record_missing_selected_checks()
            self.cleanup()

        report = build_report(
            target=self.target,
            game_record=self.catalog.game(self.target.game).record,
            manifest=manifest,
            artifacts_dir=self.options.artifacts,
            runtime=runtime,
            checks=[result.as_dict() for result in self.results],
            started_at=started_at,
            logs=[self.server_log, self.fake_log, self.docker_log, *self.extra_logs],
            repo_root=paths.repo_root(),
            takaro=self.options.takaro,
        )
        write_report(self.out, report)
        return report

    def _scan_runtime_identity(self) -> dict[str, Any]:
        if self.hooks.scan_runtime_identity is not None:
            return dict(self.hooks.scan_runtime_identity(self.adapter, self.server_log) or {})
        found = base_checks.find_line(self.server_log, "with Fabric Loader")
        if not found:
            return {}
        return self.adapter.parse_runtime_identity(found[1]) or {}

    def cleanup(self) -> None:
        # Called from the local path's `finally`, from the hosted hook's, from the signal handler
        # and from `run()` itself; only the first call does anything.
        if self._cleaned:
            return
        self._cleaned = True
        failed = any(result.status == "fail" for result in self.results)
        if self.target.game == "ark":
            try:
                self.extra_logs.extend(capture_ark_diagnostics(self.containers, self.data_dir, self.out))
            except (OSError, subprocess.TimeoutExpired) as exc:
                (self.out / "ark-cleanup-diagnostics-error.txt").write_text(
                    f"{type(exc).__name__}: {exc}\n", encoding="utf-8"
                )
        for container in self.containers:
            container.remove()
        if failed and self.options.keep_on_failure:
            output.warn(f"keeping the verification data dir at {self.data_dir}")
            return
        shutil.rmtree(self.data_dir, ignore_errors=True)


def run_targets(catalog: Catalog, targets: list[Target], options: RunOptions) -> list[dict[str, Any]]:
    """Run every target in turn, cleaning up even when the process is asked to stop."""
    runs: list[TargetRun] = []

    def _on_signal(signum: int, frame: Any) -> None:
        for run in runs:
            run.cleanup()
        raise KeyboardInterrupt

    previous = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
    if threading.current_thread() is threading.main_thread():
        for sig in previous:
            signal.signal(sig, _on_signal)
    try:
        reports = []
        for target in targets:
            run = TargetRun(catalog, target, options)
            runs.append(run)
            output.info(f"verifying {target.game}/{target.id}")
            reports.append(asyncio.run(run.run()))
        return reports
    finally:
        if threading.current_thread() is threading.main_thread():
            for sig, handler in previous.items():
                signal.signal(sig, handler)
