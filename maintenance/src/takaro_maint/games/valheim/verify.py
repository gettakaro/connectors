"""Valheim's verification hooks: what the generic runner cannot know about this game.

What a Valheim report may claim, and what it may not:

* ``level`` reaches ``startup``. The Minecraft-shaped ``identify``, ``connector-load``,
  ``catalog-*``, ``console`` and ``shutdown`` checks look for lines this plugin does not
  write, so they are deliberately not selected; ``handshake``, ``items``, ``entities``,
  ``action``, ``reconnect`` and ``stop`` below are this game's equivalents and say so in
  their details. ``items`` and ``entities`` run by default and fail until the plugin
  translates its catalogue names, so a bare verification reports that limitation.
* **Player-driven events are not covered by any of this.** A ``verify`` run boots a
  dedicated server in a container with no game client connected, so chat, deaths, kills and
  server chat to players are never exercised. Nothing in a report produced here is evidence
  that they work; that proof comes from a live client run.

Three things differ from a Minecraft run and each one is a hook here. The server says it is
up with its own line. The plugin is configured by a BepInEx ``.cfg`` in the game directory,
not by the environment, so the run writes it before the container starts. And the container
outlives the game process, so the base ``shutdown`` check is replaced by one that asks the
plugin to quit, watches for the server's own quit lines and then stops the container.
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

CHECK_IDS = ("handshake", "items", "entities", "action", "reconnect", "stop")

#: The server registers with Steam once the world is up, so this line is what "the server
#: is serving" looks like in the log.
READY_LINE = re.compile(r"Game server connected")

#: Base checks this connector cannot satisfy, and the check that stands in for each.
#: A run that names no ``--checks`` excludes these rather than failing them.
UNSUPPORTED_CHECKS = {
    "connector-load": ("the load line is a Minecraft connector's; `handshake` asserts the plugin loaded here"),
    "identify": ("the plugin identifies over the socket; `handshake` asserts the frame Takaro received"),
    "catalog-items": ("spot-checks a Minecraft item id; `items` spot-checks a Valheim one"),
    "catalog-entities": ("spot-checks a Minecraft entity id; `entities` spot-checks a Valheim one"),
    "console": ("the base console check drives a Minecraft command; `action` drives a Valheim one"),
    "shutdown": ("asserts an exit code this server's teardown does not give; `stop` asserts the shutdown"),
}

HANDSHAKE_LINE = re.compile(r"Takaro Valheim identified as gameServerId=")
LOADED_LINE = re.compile(r"Loading \[Takaro Valheim (?P<version>[^\]]+)\]")
STARTED_LINE = re.compile(r"Takaro Valheim connector started\.")
LIST_ITEMS_LINE = re.compile(r"Takaro Valheim listItems returned (?P<count>[0-9]+) item prefab")
QUIT_LINE = re.compile(r"Takaro Valheim executing scheduled shutdown|Application\.Quit|World saved|Shutting down")
BANNER_MARKERS = ("Valheim version:", "BepInEx ")

#: The plugin reconnects with an exponential backoff capped at 60 s, so the 20 s the
#: lifecycle checks allow a Minecraft connector is far too short.
#: How long the image's log supervisor may take to tail a line the plugin has already
#: answered over the socket.
CATALOGUE_LOG_BUDGET = 60.0

RECONNECT_BUDGET = 150.0
QUIT_BUDGET = 120.0
STOP_TIMEOUT = 120
ITEM_SPOT = ("SwordBronze", "Bronze Sword")
ENTITY_SPOT = ("Greydwarf_Elite", "Greydwarf Brute")

#: The game has already quit by the time the container is stopped, so what is left is the
#: image's supervisor. It exits on the signal ``docker stop`` sends rather than trapping it,
#: which is 128 + SIGTERM. Anything else means the supervisor died some other way, which is
#: what this check is for.
CLEAN_EXIT_CODES = frozenset({0, 143})

CONFIG_RELATIVE = Path("BepInEx") / "config" / "com.takaro.valheim.cfg"

CONFIG_TEMPLATE = """## Written by takaro-maint verify; BepInEx reads <game dir>/{relative}.
[Takaro]

## The Takaro websocket this server talks to.
takaroWsUrl = {url}

## Identity of this game server.
identityToken = {identity}
registrationToken = {registration}
serverName = takaro-verify

logLevel = Information
enableLogEvents = true

## Console commands Takaro is allowed to run.
commandAllowlistExact = help
commandAllowlistPrefixes =
"""


def render_config(data_dir: Path, takaro_env: dict[str, str]) -> Path:
    """The plugin's only configuration, written where BepInEx will look for it."""
    path = data_dir / CONFIG_RELATIVE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        CONFIG_TEMPLATE.format(
            relative=CONFIG_RELATIVE.as_posix(),
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


def scan_runtime_identity(adapter: Any, log_file: Path) -> dict[str, Any]:
    """The build and the loader that actually booted, merged from their two banners."""
    if not log_file.is_file():
        return {}
    identity: dict[str, Any] = {}
    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not any(marker in line for marker in BANNER_MARKERS):
                continue
            parsed = adapter.parse_runtime_identity(line)
            if not parsed:
                continue
            for key, value in parsed.items():
                if value is not None and identity.get(key) is None:
                    identity[key] = value
            if identity.get("gameVersion") and identity.get("loaderVersion"):
                break
    return identity


# --------------------------------------------------------------------------- catalogue


def _catalogue_problems(entries: Any, request: str) -> list[str]:
    """What a catalogue response has to look like before a human name can be claimed."""
    problems: list[str] = []
    if not isinstance(entries, list) or not entries:
        return [f"{request} returned {entries!r}, expected a non-empty list"]
    for entry in entries[:200]:
        if not isinstance(entry, dict) or not entry.get("code") or not entry.get("name"):
            problems.append(f"{request} returned an entry without a code and a name: {entry!r}")
            break
    return problems


#: A prefab code that is not already a word a player would read: it joins words without a
#: space (``SwordBronze``) or with an underscore (``Greydwarf_Elite``). A single plain word
#: like ``Boar`` is excluded, because for those the code really is the display name.
_COMPOUND_CODE = re.compile(r"_|(?<=[a-z0-9])[A-Z]")


def _looks_like_a_dev_name(name: str, code: str | None = None) -> bool:
    """A localisation key or a class name rather than something a player would read."""
    if name.startswith("$") or re.match(r"^(item|enemy|piece|location)_", name):
        return True
    # Handing the code straight back is not a translation, unless the code happens to be
    # the word itself: `Boar` is what a player reads, `SwordBronze` is not.
    return code is not None and name == code and bool(_COMPOUND_CODE.search(code))


def _named(entries: Any, code: str) -> str | None:
    for entry in entries or []:
        if isinstance(entry, dict) and entry.get("code") == code:
            return str(entry.get("name"))
    return None


# --------------------------------------------------------------------------- local hooks


async def after_protocol(run: Any, fake: Any, alive: Any) -> None:
    for check_id, coroutine in (
        ("handshake", lambda: _check_handshake(run, fake, alive)),
        ("items", lambda: _check_catalogue(run, fake, alive, "listItems", "items", ITEM_SPOT)),
        ("entities", lambda: _check_catalogue(run, fake, alive, "listEntities", "entities", ENTITY_SPOT)),
        ("action", lambda: _check_action(run, fake)),
        ("reconnect", lambda: _check_reconnect(run, fake, alive)),
    ):
        if run.wanted(check_id):
            run.record(await coroutine())
        else:
            run.skip(check_id, "not selected by --checks")


async def _check_handshake(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """The plugin loaded, started, identified, and Takaro accepted the identify frame."""
    with checks._Timer() as timer:
        problems: list[str] = []
        if fake.identify_count < 1:
            problems.append("the connector never sent an identify frame")
        confirmed = await asyncio.to_thread(
            checks.wait_for_line, run.server_log, HANDSHAKE_LINE, checks_lifecycle.IDENTIFY_BUDGET, alive
        )
        if not confirmed:
            problems.append("the server log never confirmed the identify with 'identified as gameServerId='")
        loaded = await asyncio.to_thread(checks.find_line, run.server_log, LOADED_LINE)
        version = None
        if loaded is None:
            problems.append("BepInEx never logged 'Loading [Takaro Valheim …]'")
        else:
            found = LOADED_LINE.search(loaded[1])
            version = found.group("version") if found else None
        if await asyncio.to_thread(checks.find_line, run.server_log, STARTED_LINE) is None:
            problems.append("the plugin never logged 'Takaro Valheim connector started.'")
    return checks.CheckResult(
        "handshake",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "identifyFrames": fake.identify_count,
            "pluginVersion": version,
            "note": (
                "Valheim's equivalent of `identify`: this plugin logs "
                "'identified as gameServerId=', never 'Identified successfully'"
            ),
            "problems": problems,
        },
        {"file": run.server_log.name, "line": confirmed[0]} if confirmed else {"file": run.server_log.name},
    )


async def _check_catalogue(
    run: Any, fake: Any, alive: Any, request: str, check_id: str, spot: tuple[str, str]
) -> checks.CheckResult:
    """The catalogue answers, and says what it answers with."""
    with checks._Timer() as timer:
        entries: Any = None
        problems: list[str] = []
        try:
            entries = await fake.request(request, {})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"{request} failed: {exc}")
        problems += _catalogue_problems(entries, request)
        rows = [entry for entry in entries or [] if isinstance(entry, dict)]
        names = [str(entry.get("name")) for entry in rows]
        offenders = [
            f"{entry.get('code')} -> {entry.get('name')}"
            for entry in rows
            if _looks_like_a_dev_name(str(entry.get("name")), str(entry.get("code")))
        ]
        human = bool(names) and not offenders
        if offenders:
            problems.append(
                f"{check_id}: {len(offenders)} of {len(names)} names are translation keys or class names, "
                f"e.g. {', '.join(offenders[:3])}"
            )
        spot_code, expected_name = spot
        actual_name = _named(entries, spot_code)
        if actual_name is None:
            problems.append(f"{check_id}: {spot_code} is missing from the catalogue")
        elif actual_name != expected_name:
            problems.append(f"{check_id}: {spot_code} is named {actual_name!r}, expected {expected_name!r}")
        logged = None
        if check_id == "items" and not problems:
            # The response comes back over the websocket; the line reaches server.log only
            # once the image's log supervisor has tailed the game's own file, which is a
            # moment later. Waiting for it is the difference between reading the count and
            # racing it.
            found = await asyncio.to_thread(
                checks.wait_for_line, run.server_log, LIST_ITEMS_LINE, CATALOGUE_LOG_BUDGET, alive
            )
            if found is None:
                problems.append("the server never logged 'listItems returned N item prefab(s)'")
            else:
                match = LIST_ITEMS_LINE.search(found[1])
                logged = int(match.group("count")) if match else None
                if logged is not None and logged != len(entries):
                    problems.append(f"the server logged {logged} prefabs, the response carried {len(entries)}")
    return checks.CheckResult(
        check_id,
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "request": request,
            "count": len(entries) if isinstance(entries, list) else None,
            "loggedCount": logged,
            "spotCheck": {"code": spot_code, "expected": expected_name, "actual": actual_name},
            "humanNames": human,
            "problems": problems,
        },
        {"file": run.server_log.name},
    )


async def _check_action(run: Any, fake: Any) -> checks.CheckResult:
    """The representative server-owned action: the one console command the default allow-list permits."""
    with checks._Timer() as timer:
        problems: list[str] = []
        result: Any = None
        try:
            result = await fake.request("executeConsoleCommand", {"command": "help"})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"executeConsoleCommand failed: {exc}")
        if not isinstance(result, dict) or result.get("success") is not True:
            problems.append(f"executeConsoleCommand returned {result!r}, expected success true")
    return checks.CheckResult(
        "action",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "action": "executeConsoleCommand",
            "command": "help",
            "rawResult": result,
            "note": (
                "the only command the default allow-list permits; chat and broadcast need a "
                "connected game client and are not exercised by this run"
            ),
            "problems": problems,
        },
        {"file": run.server_log.name},
    )


async def _check_reconnect(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """Takaro drops the socket; the plugin comes back and is usable again."""
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
    """Takaro asks the server to quit; the container stops cleanly and the pinned bytes are unchanged.

    The image's entrypoint supervises the game process, so the container can outlive it and
    the base ``shutdown`` check (which waits for an exit) cannot be used here. The re-hash
    covers the BepInEx pack too: its rows are in the ledger.
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
            if code not in CLEAN_EXIT_CODES:
                problems.append(f"the server container exited {code}, expected one of {sorted(CLEAN_EXIT_CODES)}")
        intact, changed = _rehash(run.data_dir, ledger_inputs)
        if changed:
            problems.append("the pinned inputs changed during the run: " + "; ".join(changed))
    return checks.CheckResult(
        "stop",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "exitCode": code,
            "exitCodeMeaning": "clean" if code in CLEAN_EXIT_CODES else "unexpected",
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
