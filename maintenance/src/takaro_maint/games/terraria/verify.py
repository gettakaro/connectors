"""Terraria's verification hooks: a TShock server plus a Node bridge beside it.

What the generic runner cannot know about this game:

*Two processes.* The connector is not loaded by the server. It is a Node bridge that holds
the websocket to Takaro and drives TShock over its REST API on ``127.0.0.1:7878``, so a run
starts a second container in the server container's network namespace once the server
container exists. That is what ``after_boot`` is for.

*Two configuration files, neither of them environment variables.* TShock reads
``/tshock/config.json`` (where the REST token lives) and the bridge reads
``TakaroConfig.txt``. Both are written before the server starts, both mode 0600, and
neither value is ever printed.

*Root, and then not root.* This image has to run as root: TerrariaServerAPI opens its own
``ServerLog.txt`` in ``/server`` and TShock writes ``/server/GeoIP.dat``, neither of which is
a declared volume, so a ``--user`` run dies before it has read its configuration. The run
therefore leaves root-owned files behind in its data directory, and ``after_shutdown`` gives
them back to the calling user with a throwaway container.

*Which checks are honest here.* Four base checks cannot be satisfied by this connector, and
``before_boot`` takes them out of a run that named no ``--checks`` rather than letting them
fail: see ``UNSUPPORTED_CHECKS``. ``handshake`` is the equivalent of ``identify`` plus
``connector-load`` and says so in its detail, ``items`` replaces ``catalog-items`` with a
Terraria spot check, and ``entities`` states the limit ``catalog-entities`` would assert
against. A Terraria report therefore reaches ``startup`` and never claims ``protocol``.
A caller that names ``--checks`` gets exactly what it named, including any of the four.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import secrets
import subprocess
import time
from pathlib import Path
from typing import Any

from ... import output
from ...verify import checks, checks_lifecycle, runner
from ...verify.hooks import GameHooks

CHECK_IDS = ("handshake", "items", "entities", "action", "cooperation", "references", "reconnect")

#: Base checks this connector cannot satisfy, and the Terraria check that stands in for each.
#: A run that names no ``--checks`` would otherwise spend four timeouts failing on them.
UNSUPPORTED_CHECKS = {
    "connector-load": "the load line is in the server log but the connector is the bridge; `handshake` asserts both",
    "identify": "the bridge logs its identify in its own log, not the server's; `handshake` asserts it there",
    "catalog-items": "spot-checks a Minecraft item id; `items` spot-checks a Terraria one",
    "catalog-entities": "asserts an entity registry Terraria does not have; `entities` states that limit",
}

#: TShock's own "the listener is up" line, as the server writes it.
READY_LINE = re.compile(r"Server started")
#: What the plugin logs when TShock has loaded it. The leading text is pinned by the plugin.
LOADED_LINE = re.compile(r"Takaro Terraria Events plugin loaded")
#: What the bridge logs once Takaro has accepted its identify frame.
IDENTIFIED_LINE = re.compile(r"Identified successfully")

TERRARIA_BANNER = re.compile(r"Terraria Server v[0-9][0-9.]*")
TSHOCK_BANNER = re.compile(r"TShock [0-9][0-9.]*[^\n]*now running")

REST_PORT = 7878
HEALTH_PORT = 3020
#: The bridge backs off from 3 s; this is the budget a reconnect gets.
RECONNECT_BUDGET = 30.0
#: How long the catalogue call gets: the bridge answers listItems from a static table.
CATALOGUE_TIMEOUT = 120.0
#: The smallest catalogue this connector may answer with and still be a catalogue.
CATALOGUE_MINIMUM = 6000
#: The one item id every Terraria catalogue has to name correctly.
CATALOGUE_SPOT = ("9", "Wood")

BRIDGE_LOG = "bridge.log"

TSHOCK_CONFIG = """{
  "Settings": {
    "ServerName": "takaro-verify",
    "MaxSlots": 8,
    "RestApiEnabled": true,
    "RestApiPort": %(rest_port)d,
    "LogRest": false,
    "EnableTokenEndpointAuthentication": false,
    "RESTMaximumRequestsPerInterval": 500,
    "RESTRequestBucketDecreaseIntervalMinutes": 1,
    "ApplicationRestTokens": {
      "%(rest_token)s": {"Username": "takaro", "UserGroupName": "superadmin"}
    }
  }
}
"""


def _write_private(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    os.chmod(path, 0o600)
    return path


def render_tshock_config(data_dir: Path, rest_token: str) -> Path:
    """TShock's own config, with the REST API on and one application token.

    Only the keys this run needs are written: TShock fills its defaults for everything
    absent and rewrites the file on load, so a partial document is the smaller claim.
    """
    return _write_private(
        data_dir / "tshock" / "config.json",
        TSHOCK_CONFIG % {"rest_port": REST_PORT, "rest_token": rest_token},
    )


def render_bridge_config(data_dir: Path, takaro_env: dict[str, str], rest_token: str, run_id: str) -> Path:
    """The bridge's only configuration, written where the sidecar will look for it."""
    lines = [
        "# Written by takaro-maint verify. Values are this run's throwaways.",
        f"registrationToken={takaro_env['TAKARO_REGISTRATION_TOKEN']}",
        f"identityToken={takaro_env['TAKARO_IDENTITY_TOKEN']}",
        f"serverName=takaro-verify-{run_id}",
        "serverChatName=Takaro",
        f"takaroWsUrl={takaro_env['TAKARO_WS_URL']}",
        f"tshockBaseUrl=http://127.0.0.1:{REST_PORT}",
        f"tshockToken={rest_token}",
        f"httpPort={HEALTH_PORT}",
        "pollIntervalMs=5000",
        "logFiles=/tshock/logs",
        "commandAllowlistExact=help,/help",
        "commandAllowlistPrefixes=say,time",
        "enableShutdown=true",
        "",
    ]
    return _write_private(data_dir / "bridge" / "TakaroConfig.txt", "\n".join(lines))


# --------------------------------------------------------------------------- boot hooks


def before_boot(run: Any, takaro_env: dict[str, str]) -> None:
    """Both configuration files, before the server container exists."""
    run.rest_token = secrets.token_urlsafe(24)
    tshock = render_tshock_config(run.data_dir, run.rest_token)
    bridge = render_bridge_config(run.data_dir, takaro_env, run.rest_token, run.options.run_id)
    output.info(f"wrote {tshock.parent.name}/{tshock.name} and {bridge.parent.name}/{bridge.name} (mode 0600)")


def after_boot(run: Any, container: Any, takaro_env: dict[str, str]) -> None:
    """Start the bridge in the server container's network namespace.

    ``--network container:<name>`` is the whole reason this hook exists: it can only be
    asked for once the server container exists, and it is what puts TShock's REST API on
    the bridge's own ``127.0.0.1``.
    """
    data = run.data_dir
    image = run.resolved["build"]["deps"]["bridge-runtime"]["resolvedCoordinate"]
    name = f"{container.name}-bridge"
    ttl = int(time.time()) + 3 * 3600
    argv = [
        *runner.docker_command(),
        "run",
        "-d",
        "--name",
        name,
        "--label",
        f"tm.run={run.options.run_id}",
        "--label",
        f"tm.ttl={ttl}",
    ]
    for label in run.options.labels:
        argv += ["--label", label]
    argv += [
        "--network",
        f"container:{container.name}",
        "--memory",
        "512m",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "-e",
        "BRIDGE_CONFIG=/config/TakaroConfig.txt",
        "-e",
        "HOME=/tmp",
        "-v",
        f"{data / 'bridge' / 'TakaroTerrariaBridge'}:/bridge:ro",
        # The config lives outside /bridge: docker would have to create the mountpoint
        # inside a read-only mount otherwise, which fails before the process starts.
        "-v",
        f"{data / 'bridge' / 'TakaroConfig.txt'}:/config/TakaroConfig.txt:ro",
        "-v",
        f"{data / 'tshock' / 'logs'}:/tshock/logs:ro",
        "-w",
        "/bridge",
        str(image),
        "node",
        "dist/index.js",
    ]
    log_file = run.out / BRIDGE_LOG
    bridge = runner.Container(
        name=name,
        argv=argv,
        log_file=log_file,
        docker_log=run.docker_log,
        secrets=[run.registration_token, run.rest_token],
    )
    # Registered before it is started: `docker run` has created the container by the time
    # `start()` returns, and an interrupt during it would otherwise leak the container.
    run.containers.append(bridge)
    if log_file not in run.extra_logs:
        run.extra_logs.append(log_file)
    run.bridge = bridge
    bridge.start()
    output.info(f"bridge container {name} joined {container.name}'s network namespace")
    del takaro_env


async def after_shutdown(run: Any, fake: Any, ws_url: str, ledger_inputs: Any) -> None:
    """Give the run's data directory back to the caller.

    TShock has to run as root here (see the module docstring), so everything the server
    wrote — the world, the logs, its own config — belongs to root. The harness's own
    cleanup would silently leave it behind in the temp directory, so one throwaway
    container hands it back. It is best effort: failing to chown is not a verification
    result, and it is never allowed to fail the run.
    """
    del fake, ws_url, ledger_inputs
    completed = subprocess.run(
        [
            *runner.docker_command(),
            "run",
            "--rm",
            # The image's entrypoint is the server itself, so chown has to replace it
            # rather than be handed to it as arguments.
            "--entrypoint",
            "chown",
            "-v",
            f"{run.data_dir}:/target",
            str(run.resolved["containerRef"]),
            "-R",
            f"{os.getuid()}:{os.getgid()}",
            "/target",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        output.warn(f"could not reclaim ownership of the verification data dir: {completed.stderr.strip()[:200]}")
    else:
        output.info("reclaimed ownership of the verification data dir")


def scan_runtime_identity(adapter: Any, log_file: Path) -> dict[str, Any]:
    """The two banners, merged: the game version from one, the loader version from the other."""
    identity: dict[str, Any] = {}
    if not log_file.is_file():
        return identity
    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parsed = adapter.parse_runtime_identity(line)
            if not parsed:
                continue
            for key, value in parsed.items():
                if value is not None and identity.get(key) is None:
                    identity[key] = value
            if identity.get("gameVersion") and identity.get("loaderVersion"):
                break
    return identity


# --------------------------------------------------------------------------- local checks


async def after_protocol(run: Any, fake: Any, alive: Any) -> None:
    for check_id, coroutine in (
        ("handshake", lambda: _check_handshake(run, fake, alive)),
        ("items", lambda: _check_items(run, fake)),
        ("entities", lambda: _check_entities(run, fake)),
        ("action", lambda: _check_action(run, fake, alive)),
        ("cooperation", lambda: _check_cooperation(run, fake)),
        ("references", lambda: _check_references(run)),
        ("reconnect", lambda: _check_reconnect(run, fake, alive)),
    ):
        if run.wanted(check_id):
            run.record(await coroutine())
        else:
            run.skip(check_id, "not selected by --checks")


def _bridge_log(run: Any) -> Path:
    return Path(run.out) / BRIDGE_LOG


async def _check_handshake(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """The plugin loaded, the bridge identified, and Takaro saw the identify frame."""
    with checks._Timer() as timer:
        problems: list[str] = []
        if fake.identify_count < 1:
            problems.append("the bridge never sent an identify frame")
        identified = await asyncio.to_thread(
            checks.wait_for_line, _bridge_log(run), IDENTIFIED_LINE, checks_lifecycle.IDENTIFY_BUDGET, alive
        )
        if not identified:
            problems.append(f"{BRIDGE_LOG} never logged 'Identified successfully'")
        loaded = await asyncio.to_thread(checks.wait_for_line, run.server_log, LOADED_LINE, 60, alive)
        if not loaded:
            problems.append("the server never logged 'Takaro Terraria Events plugin loaded'")
    return checks.CheckResult(
        "handshake",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "identifyFrames": fake.identify_count,
            "pluginLine": loaded[1].strip() if loaded else None,
            "note": (
                "Terraria's equivalent of `identify` + `connector-load`: the plugin logs its load "
                "in the server log and the bridge logs 'Identified successfully' in its own"
            ),
            # The report's own record of why four base checks read `skip`: the runner can
            # only say "not selected by --checks", which does not say who did the selecting.
            "standsInFor": dict(UNSUPPORTED_CHECKS),
            "problems": problems,
        },
        {"file": BRIDGE_LOG, "line": identified[0]} if identified else {"file": BRIDGE_LOG},
    )


async def _check_items(run: Any, fake: Any) -> checks.CheckResult:
    """The item catalogue: human display names, and enough of them to be the real table."""
    del run
    with checks._Timer() as timer:
        try:
            entries = await fake.request("listItems", {}, timeout=CATALOGUE_TIMEOUT)
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            return checks.CheckResult("items", "fail", 0, {"problems": [f"listItems failed: {exc}"]})
        problems, detail = checks._catalogue_problems(entries, CATALOGUE_SPOT)
        if detail["count"] < CATALOGUE_MINIMUM:
            problems.append(f"the catalogue holds {detail['count']} items, expected at least {CATALOGUE_MINIMUM}")
    detail["problems"] = problems
    return checks.CheckResult("items", "pass" if not problems else "fail", timer.elapsed_ms, detail)


async def _check_entities(run: Any, fake: Any) -> checks.CheckResult:
    """A declared limit, asserted rather than assumed: the answer is an empty list."""
    del run
    with checks._Timer() as timer:
        problems: list[str] = []
        entries: Any = None
        try:
            entries = await fake.request("listEntities", {}, timeout=30)
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"listEntities failed: {exc}")
        if entries != []:
            problems.append(f"listEntities returned {entries!r}, expected an empty list")
    return checks.CheckResult(
        "entities",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "entities": entries,
            "note": (
                "declared limit: Terraria has no queryable NPC registry, so the bridge answers "
                "an empty list by design rather than inventing one"
            ),
            "problems": problems,
        },
    )


async def _check_action(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """A representative action: a Takaro chat message reaches the server."""
    marker = f"takaro-verify-{run.options.run_id}-action"
    with checks._Timer() as timer:
        problems: list[str] = []
        result: Any = None
        try:
            result = await fake.request("sendMessage", {"message": marker})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"sendMessage failed: {exc}")
        if not isinstance(result, dict) or result.get("success") is not True:
            problems.append(f"sendMessage returned {result!r}, expected success true")
        found = await asyncio.to_thread(checks.wait_for_line, run.server_log, re.compile(re.escape(marker)), 30, alive)
        if not found:
            problems.append(f"the server log never showed the message '{marker}'")
    return checks.CheckResult(
        "action",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"action": "sendMessage", "message": marker, "result": result, "problems": problems},
        {"file": run.server_log.name, "line": found[0]} if found else {"file": run.server_log.name},
    )


async def _check_cooperation(run: Any, fake: Any) -> checks.CheckResult:
    """The bridge drives the plugin: an unban goes through ``/takarounban`` and its marker.

    No player is needed: unbanning a name that was never banned still runs the plugin's
    command over REST and comes back as the plugin's ``TAKARO_UNBAN`` answer. A bridge that
    no longer understands this plugin's marker, or a plugin that is not loaded, falls back to
    a REST ticket unban, which answers something else and fails this check.
    """
    player = f"takaro-verify-{run.options.run_id}-nobody"
    expected = f"No active ban for {player}"
    with checks._Timer() as timer:
        problems: list[str] = []
        result: Any = None
        try:
            result = await fake.request("unbanPlayer", {"gameId": player})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"unbanPlayer failed: {exc}")
        if not isinstance(result, dict) or result.get("success") is not True:
            problems.append(f"unbanPlayer returned {result!r}, expected success true")
        elif result.get("rawResult") != expected:
            problems.append(f"unbanPlayer answered {result.get('rawResult')!r}, not the plugin's {expected!r}")
    return checks.CheckResult(
        "cooperation",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "action": "unbanPlayer",
            "player": player,
            "result": result,
            "note": "answered by the plugin's /takarounban marker, not the bridge's REST fallback",
            "problems": problems,
        },
    )


def _hash_in_container(container_name: str, path: str) -> str | None:
    completed = subprocess.run(
        [*runner.docker_command(), "exec", container_name, "sha256sum", path],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return None
    return completed.stdout.strip().split()[0] if completed.stdout.strip() else None


async def _check_references(run: Any) -> checks.CheckResult:
    """The assemblies the plugin was compiled against are the ones the container is running."""
    deps = run.resolved["build"]["deps"]
    container = run.container
    with checks._Timer() as timer:
        problems: list[str] = []
        rows: list[dict[str, Any]] = []
        for path in run.resolved["build"]["references"]:
            expected = str(deps[Path(path).name]["sha256"])
            actual = (
                await asyncio.to_thread(_hash_in_container, container.name, path) if container is not None else None
            )
            rows.append({"path": path, "expected": expected, "actual": actual})
            if actual is None:
                problems.append(f"could not hash {path} inside the container")
            elif actual != expected:
                problems.append(f"{path}: container sha256 {actual} != catalog {expected}")
    return checks.CheckResult(
        "references",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"references": rows, "problems": problems},
    )


async def _check_reconnect(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """Takaro drops the socket; the bridge comes back and is usable again."""
    with checks._Timer() as timer:
        problems: list[str] = []
        before = fake.identify_count
        await fake.disconnect(1001, "going away")
        reidentify_ms = await checks_lifecycle.identify_within(fake, before + 1, RECONNECT_BUDGET, alive)
        reachable: Any = None
        if reidentify_ms is None:
            problems.append(f"no identify frame arrived within {RECONNECT_BUDGET:.0f} s of the close")
        else:
            try:
                await fake.ping(timeout=5)
            except (TimeoutError, RuntimeError) as exc:
                problems.append(f"the reconnected socket did not answer a ping: {exc}")
            try:
                reachable = await fake.request("testReachability", {})
            except Exception as exc:  # noqa: BLE001 - reported as a check failure
                problems.append(f"testReachability failed on the reconnected socket: {exc}")
            if not isinstance(reachable, dict) or reachable.get("connectable") is not True:
                problems.append(f"testReachability returned {reachable!r}, expected connectable true")
        confirmations = await asyncio.to_thread(
            checks_lifecycle.wait_for_count, _bridge_log(run), IDENTIFIED_LINE, 2, 30, alive
        )
        if confirmations < 2:
            problems.append(f"the bridge logged {confirmations} identify confirmation(s), expected at least 2")
    return checks.CheckResult(
        "reconnect",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "closeCode": 1001,
            "reidentifyMs": int(reidentify_ms) if reidentify_ms is not None else None,
            "identifyCountBefore": before,
            "identifyCountAfter": fake.identify_count,
            "identifyConfirmations": confirmations,
            "testReachability": reachable,
            "budgetSeconds": RECONNECT_BUDGET,
            "problems": problems,
        },
        {"file": BRIDGE_LOG},
    )


def health_snapshot(container_name: str) -> dict[str, Any]:
    """The bridge's own health document, read from inside its container.

    Used by the rig evidence run rather than by a check: it is the only place the bridge
    states what it thinks of Takaro and of TShock at the same time.
    """
    completed = subprocess.run(
        [
            *runner.docker_command(),
            "exec",
            container_name,
            "node",
            "-e",
            f"fetch('http://127.0.0.1:{HEALTH_PORT}/health').then(r=>r.json()).then(j=>console.log(JSON.stringify(j)))",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0 or not completed.stdout.strip():
        return {}
    try:
        return dict(json.loads(completed.stdout.strip()))
    except json.JSONDecodeError:
        return {}


#: What this game contributes to a verification run; the runner reads nothing else.
HOOKS = GameHooks(
    ready_line=READY_LINE,
    # First boot generates the world; the measured pin took 447 seconds.
    startup_timeout=900.0,
    check_ids=CHECK_IDS,
    unsupported_checks=UNSUPPORTED_CHECKS,
    before_boot=before_boot,
    after_boot=after_boot,
    after_protocol=after_protocol,
    after_shutdown=after_shutdown,
    scan_runtime_identity=scan_runtime_identity,
)
