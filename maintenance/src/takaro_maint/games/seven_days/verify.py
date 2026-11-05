"""7 Days to Die's verification hooks: what the generic runner cannot know about this game.

Three things differ from a Minecraft run and each one is a hook here. The server says it
is up with its own line, not Mojang's. The mod is configured by a file in the server's
working directory, not by the environment, so the run writes it before the container
starts. And the container never exits on its own -- its entrypoint tails the console log
-- so the shutdown the base check performs is replaced by one that asks the mod to quit,
watches for the server's own quit lines and then stops the container.

``identify`` and ``connector-load`` stay out of a 7D2D run: both look for lines this mod
does not write. ``handshake`` is its equivalent and says so in its detail, which is why a
7D2D report reaches ``startup`` and never claims ``protocol``.
"""

from __future__ import annotations

import asyncio
import os
import re
import subprocess
from pathlib import Path
from typing import Any

from ... import net, output
from ...verify import checks, checks_lifecycle
from ...verify.hooks import GameHooks
from ...verify.runner import docker_command
from . import checks_prismacore

#: Command families, one module each (``checks_<family>.py``): its check ids, what it puts
#: on disk before the boot, and its checks, run after the lifecycle checks below.
FAMILIES = (checks_prismacore,)

CHECK_IDS = ("handshake", "action", "reconnect", "stop") + tuple(
    check_id for family in FAMILIES for check_id in family.CHECK_IDS
)

READY_LINE = re.compile(r"INF StartGame done")

#: Base checks this connector cannot satisfy, and the check that stands in for each.
#: A run that names no ``--checks`` excludes these rather than failing them.
UNSUPPORTED_CHECKS = {
    "connector-load": ("the load line is a Minecraft connector's; `handshake` asserts the mod loaded here"),
    "identify": ("the mod identifies in its own log line; `handshake` asserts the frame Takaro received"),
    "catalog-items": ("spot-checks a Minecraft item id; 7D2D ships no item catalogue over this protocol"),
    "catalog-entities": ("spot-checks a Minecraft entity id; 7D2D ships no entity catalogue over this protocol"),
    "shutdown": ("asserts an exit code this server's teardown does not give; `stop` asserts the shutdown"),
}

HANDSHAKE_LINE = re.compile(r"\[Takaro\] \*INFO\* WebSocket connection confirmed")
LOADED_LINE = re.compile(r"\[MODS\]\s+Loaded Mod: Takaro \((?P<version>[^)]+)\)")
QUIT_LINE = re.compile(r"INF Preparing quit|\[NET\] ServerShutdown")
BANNER_MARKERS = ("Last played version:", "GamePref.GameVersion")

# The mod waits ReconnectIntervalSeconds (30) before its first attempt and backs off from
# there, so the 20 s the lifecycle checks allow a Minecraft connector is far too short.
RECONNECT_BUDGET = 120.0
QUIT_BUDGET = 120.0
STOP_TIMEOUT = 120

CONFIG_RELATIVE = Path("Takaro") / "Config.xml"

CONFIG_TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<!-- Written by takaro-maint verify; the mod reads <server cwd>/Takaro/Config.xml. -->
<Takaro>
  <WebSocket>
    <Url>{url}</Url>
    <IdentityToken>{identity}</IdentityToken>
    <RegistrationToken>{registration}</RegistrationToken>
    <Enabled>true</Enabled>
    <ReconnectIntervalSeconds>30</ReconnectIntervalSeconds>
  </WebSocket>
</Takaro>
"""


def render_config(data_dir: Path, takaro_env: dict[str, str]) -> Path:
    """The mod's only configuration, written where the server will look for it."""
    path = data_dir / CONFIG_RELATIVE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        CONFIG_TEMPLATE.format(
            url=takaro_env["TAKARO_WS_URL"],
            identity=takaro_env["TAKARO_IDENTITY_TOKEN"],
            registration=takaro_env["TAKARO_REGISTRATION_TOKEN"],
        ),
        encoding="utf-8",
    )
    os.chmod(path, 0o600)
    return path


def before_boot(run: Any, takaro_env: dict[str, str]) -> None:
    render_config(run.data_dir, takaro_env)
    output.info(f"wrote {CONFIG_RELATIVE.as_posix()} for this run (mode 0600)")
    for family in FAMILIES:
        family.before_boot(run)


def scan_runtime_identity(adapter: Any, log_file: Path) -> dict[str, Any]:
    """The build that actually booted, from the server's own version banner."""
    if not log_file.is_file():
        return {}
    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not any(marker in line for marker in BANNER_MARKERS):
                continue
            parsed = adapter.parse_runtime_identity(line)
            if parsed and parsed.get("gameVersion"):
                return dict(parsed)
    return {}


# --------------------------------------------------------------------------- local hooks


async def after_protocol(run: Any, fake: Any, alive: Any) -> None:
    if run.wanted("handshake"):
        run.record(await _check_handshake(run, fake, alive))
    else:
        run.skip("handshake", "not selected by --checks")
    if run.wanted("action"):
        run.record(await _check_action(run, fake, alive))
    else:
        run.skip("action", "not selected by --checks")
    if run.wanted("reconnect"):
        run.record(await _check_reconnect(run, fake, alive))
    else:
        run.skip("reconnect", "not selected by --checks")
    for family in FAMILIES:
        await family.run_checks(run, fake, alive)


async def _check_handshake(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """The mod loaded, identified, and logged Takaro's acceptance of the identify frame."""
    with checks._Timer() as timer:
        problems: list[str] = []
        identified = fake.identify_count >= 1
        if not identified:
            problems.append("the connector never sent an identify frame")
        confirmed = await asyncio.to_thread(
            checks.wait_for_line, run.server_log, HANDSHAKE_LINE, checks_lifecycle.IDENTIFY_BUDGET, alive
        )
        if not confirmed:
            problems.append("the server log never confirmed the inbound identifyResponse frame")
        loaded = await asyncio.to_thread(checks.find_line, run.server_log, LOADED_LINE)
        version = None
        if loaded is None:
            problems.append("the server never logged 'Loaded Mod: Takaro'")
        else:
            found = LOADED_LINE.search(loaded[1])
            version = found.group("version") if found else None
    return checks.CheckResult(
        "handshake",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "identifyFrames": fake.identify_count,
            "modVersion": version,
            "note": (
                "7D2D's equivalent of `identify`: this mod logs Takaro's inbound "
                "identifyResponse, never 'Identified successfully'"
            ),
            "problems": problems,
        },
        {"file": run.server_log.name, "line": confirmed[0]} if confirmed else {"file": run.server_log.name},
    )


async def _check_action(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """A second, different representative action: a Takaro chat message reaches the server."""
    marker = f"takaro-verify-{run.options.run_id}-action"
    with checks._Timer() as timer:
        problems: list[str] = []
        result: Any = None
        try:
            result = await fake.request("sendMessage", {"message": marker})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"sendMessage failed: {exc}")
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


async def _check_reconnect(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """Takaro drops the socket; the mod comes back and is usable again."""
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
            checks_lifecycle.wait_for_count, run.server_log, HANDSHAKE_LINE, 2, 30, alive
        )
        if confirmations < 2:
            problems.append(f"the server confirmed {confirmations} handshake(s), expected at least 2")
    return checks.CheckResult(
        "reconnect",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "closeCode": 1001,
            "reidentifyMs": int(reidentify_ms) if reidentify_ms is not None else None,
            "identifyCountBefore": before,
            "identifyCountAfter": fake.identify_count,
            "handshakeConfirmations": confirmations,
            "testReachability": reachable,
            "budgetSeconds": RECONNECT_BUDGET,
            "problems": problems,
        },
        {"file": run.server_log.name},
    )


async def after_shutdown(run: Any, fake: Any, ws_url: str, ledger_inputs: list[dict[str, Any]]) -> None:
    del ws_url
    if not run.wanted("stop"):
        run.skip("stop", "not selected by --checks")
        return
    run.record(await _check_stop(run, fake, ledger_inputs))


async def _check_stop(run: Any, fake: Any, ledger_inputs: list[dict[str, Any]]) -> checks.CheckResult:
    """Takaro asks the server to quit; the container stops cleanly and the bytes are unchanged.

    The image's entrypoint tails the console log, so the container outlives the game process
    and the base ``shutdown`` check (which waits for an exit) cannot be used here.
    """
    with checks._Timer() as timer:
        problems: list[str] = []
        note = "shutdown response received"
        try:
            await fake.request("shutdown", {}, timeout=30)
        except Exception as exc:  # noqa: BLE001 - the socket closing first is normal here
            note = f"the connection closed before the response arrived ({exc}); the quit lines are the real gate"
        container = run.container
        alive = container.alive if container is not None else (lambda: False)
        quit_line = await asyncio.to_thread(checks.wait_for_line, run.server_log, QUIT_LINE, QUIT_BUDGET, alive)
        if not quit_line:
            problems.append(f"the server never logged its quit within {QUIT_BUDGET:.0f} s")
        code = -1
        if container is not None:
            completed = await asyncio.to_thread(
                subprocess.run,
                [*docker_command(), "stop", "-t", str(STOP_TIMEOUT), container.name],
                capture_output=True,
                text=True,
                check=False,
            )
            if completed.returncode != 0:
                problems.append(f"docker stop failed: {completed.stderr.strip()}")
            code = await asyncio.to_thread(container.wait_for_exit, float(STOP_TIMEOUT))
            if code != 0:
                problems.append(f"the server container exited {code}, expected 0")
        intact, changed = _rehash(run.data_dir, ledger_inputs)
        if changed:
            problems.append("the pinned inputs changed during the run: " + "; ".join(changed))
    return checks.CheckResult(
        "stop",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "exitCode": code,
            "note": note,
            "inputsIntactAfterStop": not changed,
            "intact": intact,
            "problems": problems,
        },
        {"file": run.server_log.name, "line": quit_line[0]} if quit_line else {"file": run.server_log.name},
    )


def _rehash(data_dir: Path, ledger_inputs: list[dict[str, Any]]) -> tuple[list[str], list[str]]:
    intact: list[str] = []
    changed: list[str] = []
    for entry in ledger_inputs:
        path = data_dir / entry["path"]
        if not path.is_file():
            changed.append(f"{entry['path']} is missing")
            continue
        if entry.get("sha256") and net.hash_file(path)["sha256"] != entry["sha256"]:
            changed.append(f"{entry['path']} sha256 changed")
        else:
            intact.append(entry["path"])
    return intact, changed


#: What this game contributes to a verification run; the runner reads nothing else.
HOOKS = GameHooks(
    ready_line=READY_LINE,
    check_ids=CHECK_IDS,
    unsupported_checks=UNSUPPORTED_CHECKS,
    before_boot=before_boot,
    after_protocol=after_protocol,
    after_shutdown=after_shutdown,
    scan_runtime_identity=scan_runtime_identity,
)
