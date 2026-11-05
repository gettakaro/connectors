"""Project Zomboid's verification hooks: what the generic runner cannot know about it.

The server writes no Mojang-shaped line about itself, so every base check that reads one
is replaced here. ``connector-load`` reads a Fabric-shaped loader version and becomes
``agent-load``; the Minecraft catalogue spot-checks become ``catalog``; the base
``console`` sends ``say``, which is not a Project Zomboid console command, and becomes
``rcon``; and ``shutdown`` waits for the container to exit, which this image's entrypoint
never does on its own, so it becomes ``stop`` (the 7 Days to Die precedent).

Two checks exist only for this game. ``pinned-install`` is the proof that the SteamCMD
stub an install writes really did stop the image replacing the pinned bytes, and
``hooks-bound`` is the proof that the ByteBuddy hooks pinned to 42.20.4 signatures still
bind. What ``hooks-bound`` cannot prove is that the player-lifecycle hooks FIRE: that
needs a connected game client, which no unattended run has. The check says so in its own
detail rather than letting a reader assume otherwise.

``--takaro hosted`` is out of scope for this game: there is no ``run_hosted`` here, so a
hosted run is refused by the runner rather than half-performed.
"""

from __future__ import annotations

import asyncio
import json
import re
import subprocess
from pathlib import Path
from typing import Any

from ... import net, output
from ...verify import checks, checks_lifecycle
from ...verify.hooks import GameHooks
from ...verify.runner import docker_command
from . import GAME_JAR, STABLE_JAR, TARGET_CHECK_PREFIX

CHECK_IDS = ("agent-load", "pinned-install", "hooks-bound", "catalog", "rcon", "action", "reconnect", "stop")

# What the Project Zomboid dedicated server prints when it is up.
READY_LINE = re.compile(r"\*\*\* SERVER STARTED \*\*\*\*")

#: Base checks this connector cannot satisfy, and the check that stands in for each.
#: A run that names no ``--checks`` excludes these rather than failing them.
UNSUPPORTED_CHECKS = {
    "connector-load": ("the load line is a Minecraft connector's; `agent-load` asserts the javaagent attached"),
    "catalog-items": ("spot-checks a Minecraft item id; `catalog` spot-checks a Zomboid one"),
    "catalog-entities": ("spot-checks a Minecraft entity id; `catalog` covers Zomboid's entities"),
    "console": ("the base console check drives a Minecraft command; `rcon` drives a Zomboid one"),
    "shutdown": ("asserts an exit code this server's teardown does not give; `stop` asserts the shutdown"),
}


JAVA_TOOL_OPTIONS_LINE = re.compile(r"Picked up JAVA_TOOL_OPTIONS:.*-javaagent:.*" + re.escape(STABLE_JAR))
HOOKS_INSTALLED_LINE = re.compile(r"\[Takaro\] premain: hooks installed")
UNBOUND_HOOKS_LINE = re.compile(
    r"\[Takaro\] (?:premain: hooks installed; unbound: \[([^]]+)]"
    r"|ERROR hook ([^:]+): matcher bound nothing on \S+)"
)
TARGET_CHECK_LINE = re.compile(r"\[Takaro\]\s*target-check:\s*\{")
IDENTIFIED_LINE = re.compile(r"Identified successfully")
STUB_LINE = re.compile(r"takaro-maint: SteamCMD is disabled")
# Anything SteamCMD says while it is installing or validating an app. None of it may
# appear in a run whose install is pinned.
STEAMCMD_RAN = re.compile(r"Update state \(0x|Success! App '380870'|app_update 380870")
TICK_CONFIRMED_LINE = re.compile(r"\[Takaro\] HOOK CONFIRMED: tick")
TRANSFORM_FAILED_LINE = re.compile(r"\[Takaro\] listener: transform failed")
TICK_WATCHDOG_LINE = re.compile(r"\[Takaro\] WARN \*\*\*\*\* tick hook")
QUIT_LINE = re.compile(r"SERVER SHUTDOWN|Shutting down|znet: Shutdown|Saving Server State")

# The classes the ByteBuddy hooks carry; every one of them has to be instrumented for the
# player lifecycle to be able to fire at all. These four are transformed as they load, so
# the listener's own line is the evidence.
HOOKED_CLASSES = (
    "zombie.network.RCONServer",
    "zombie.network.GameServer",
    "zombie.network.chat.ChatServer",
    "zombie.characters.IsoPlayer",
)
# Two classes the JVM has already loaded by the time premain installs the hooks, so the
# on-load listener never speaks for them. Each proves itself another way: the watchdog
# retransforms IsoZombie and says so, and ZLogger's own advice announces itself the first
# time the server logs a line — which is a stronger proof than a transform line, because a
# hook that fires is bound beyond doubt.
LATE_HOOKED_CLASSES = {
    "zombie.characters.IsoZombie": (
        r"\[Takaro\] (?:listener: transformed|watchdog: retransformed) zombie\.characters\.IsoZombie\b"
    ),
    "zombie.core.logger.ZLogger": (
        r"\[Takaro\] (?:listener: transformed|watchdog: retransformed) zombie\.core\.logger\.ZLogger\b"
        r"|\[Takaro\] HOOK CONFIRMED: log \(ZLogger\.write\)"
    ),
}

# What share of a catalogue may be reported by script id before the display-name lookup
# itself is suspect. Build 42 leaves ~4% of its item scripts unnamed (206 of 5092 on
# 42.20.4); the ceiling is that with room, not a target.
UNNAMED_CEILING = 0.10


def _is_script_id(code: str, name: str) -> bool:
    """A name that is really the row's own id rather than something to show a player.

    A dotted id — ``Base.Wound_Neck_Bite_Male`` — repeated as the name is the connector
    saying it found none. A bare word that happens to equal its code is not: the entity
    catalogue's ``Zombie`` is both the id and what a player calls the thing.
    """
    return name.startswith("Base.") or (name == code and "." in code)


COVERAGE_NOTE = (
    "player-lifecycle hooks (connect, disconnect, chat, death, zombie-killed) are proven "
    "BOUND, not FIRED: firing needs a connected game client (human-owed; last recorded "
    "client evidence 2026-10-01 on linux-42.21.0, where zombie-killed did not fire)"
)

# The agent waits 5 s before its first reconnect and backs off x1.5 from there.
RECONNECT_BUDGET = 60.0
QUIT_BUDGET = 180.0
STOP_TIMEOUT = 120
AGENT_LOAD_BUDGET = 120.0
HOOKS_BOUND_BUDGET = 180.0


def scan_runtime_identity(adapter: Any, log_file: Path) -> dict[str, Any]:
    """The build that actually booted, from the agent's own target-check line."""
    if not log_file.is_file():
        return {}
    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if TARGET_CHECK_PREFIX not in line:
                continue
            parsed = adapter.parse_runtime_identity(line)
            if parsed:
                return dict(parsed)
    return {}


def _target_check(log_file: Path) -> tuple[tuple[int, str] | None, dict[str, Any]]:
    """The first target-check line and its payload, or an empty payload when there is none."""
    found = checks.find_line(log_file, TARGET_CHECK_LINE)
    if not found:
        return None, {}
    index = found[1].find("target-check:")
    try:
        return found, json.loads(found[1][index + len("target-check:") :].strip())
    except json.JSONDecodeError:
        return found, {}


# --------------------------------------------------------------------------- local hooks


async def after_protocol(run: Any, fake: Any, alive: Any) -> None:
    for check_id, coroutine in (
        ("agent-load", lambda: _check_agent_load(run, alive)),
        ("pinned-install", lambda: _check_pinned_install(run)),
        ("hooks-bound", lambda: _check_hooks_bound(run, alive)),
        ("catalog", lambda: _check_catalog(fake)),
        ("rcon", lambda: _check_rcon(fake)),
        ("action", lambda: _check_action(run, fake)),
        ("reconnect", lambda: _check_reconnect(run, fake, alive)),
    ):
        if run.wanted(check_id):
            run.record(await coroutine())
        else:
            run.skip(check_id, "not selected by --checks")


async def _check_agent_load(run: Any, alive: Any) -> checks.CheckResult:
    """The JVM picked the agent up, the hooks installed, and the guard accepted this server."""
    target = run.target
    declared = target.record["inputs"]["server"]["files"][GAME_JAR]["sha256"]
    with checks._Timer() as timer:
        problems: list[str] = []
        picked = await asyncio.to_thread(
            checks.wait_for_line, run.server_log, JAVA_TOOL_OPTIONS_LINE, AGENT_LOAD_BUDGET, alive
        )
        if not picked:
            problems.append(f"the JVM never logged 'Picked up JAVA_TOOL_OPTIONS' naming {STABLE_JAR}")
        installed = await asyncio.to_thread(
            checks.wait_for_line, run.server_log, HOOKS_INSTALLED_LINE, AGENT_LOAD_BUDGET, alive
        )
        if not installed:
            problems.append("the agent never logged 'premain: hooks installed'")
        unbound = await asyncio.to_thread(checks.find_line, run.server_log, UNBOUND_HOOKS_LINE)
        if unbound:
            problems.append(f"one or more agent hooks bound no method: {unbound[1].strip()[:200]}")
        found = await asyncio.to_thread(
            checks.wait_for_line, run.server_log, TARGET_CHECK_LINE, AGENT_LOAD_BUDGET, alive
        )
        payload: dict[str, Any] = {}
        if not found:
            problems.append("the agent wrote no target-check line")
        else:
            _, payload = _target_check(run.server_log)
            if not payload:
                problems.append("the target-check line is not JSON")
            else:
                expected = payload.get("expected") or {}
                runtime = payload.get("runtime") or {}
                if payload.get("result") != "ok":
                    problems.append(f"result={payload.get('result')} reasons={payload.get('reasons')}")
                if expected.get("gameJarSha256") != declared:
                    problems.append(f"expected.gameJarSha256 {expected.get('gameJarSha256')} != the catalog's")
                if runtime.get("gameJarSha256") != declared:
                    problems.append(f"the jar the JVM loaded hashes {runtime.get('gameJarSha256')}, not the pin")
                if payload.get("target") != target.id:
                    problems.append(f"target {payload.get('target')} != {target.id}")
                if payload.get("fingerprint") != target.fingerprint:
                    problems.append("the agent's fingerprint disagrees with the catalog")
    return checks.CheckResult(
        "agent-load",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "targetCheck": payload,
            "note": (
                "Project Zomboid's equivalent of `connector-load`: this connector is a "
                "-javaagent and reports its identity through its own target-check line, "
                "never through a Fabric loader version, so a run reaches level `startup`"
            ),
            "problems": problems,
        },
        {"file": run.server_log.name, "line": found[0]} if found else {"file": run.server_log.name},
    )


async def _check_pinned_install(run: Any) -> checks.CheckResult:
    """The image's own SteamCMD step ran the stub, so the pinned bytes were never touched."""
    with checks._Timer() as timer:
        problems: list[str] = []
        stub = await asyncio.to_thread(checks.find_line, run.server_log, STUB_LINE)
        if not stub:
            problems.append("the SteamCMD stub never spoke; the image's own updater may have run")
        ran = await asyncio.to_thread(checks.find_line, run.server_log, STEAMCMD_RAN)
        if ran:
            problems.append(f"SteamCMD ran during the boot: {ran[1].strip()[:200]}")
    return checks.CheckResult(
        "pinned-install",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "stubLine": stub[1].strip() if stub else None,
            "note": (
                "the image runs `steamcmd.sh +runscript install_server.scmd` on every start "
                "and has no knob to disable it; the install writes a stub the run mounts over "
                "the image's steamcmd directory"
            ),
            "problems": problems,
        },
        {"file": run.server_log.name, "line": stub[0]} if stub else {"file": run.server_log.name},
    )


async def _check_hooks_bound(run: Any, alive: Any) -> checks.CheckResult:
    """Every hooked class was transformed and the tick hook really fires."""
    with checks._Timer() as timer:
        problems: list[str] = []
        transformed: dict[str, bool] = {}
        for name in HOOKED_CLASSES:
            pattern = re.compile(r"\[Takaro\] listener: transformed " + re.escape(name) + r"\b")
            found = await asyncio.to_thread(checks.wait_for_line, run.server_log, pattern, HOOKS_BOUND_BUDGET, alive)
            transformed[name] = bool(found)
            if not found:
                problems.append(f"{name} was never transformed")
        for name, expression in LATE_HOOKED_CLASSES.items():
            found_late = await asyncio.to_thread(
                checks.wait_for_line, run.server_log, re.compile(expression), HOOKS_BOUND_BUDGET, alive
            )
            transformed[name] = bool(found_late)
            if not found_late:
                problems.append(f"{name} was neither transformed, retransformed nor seen firing")
        tick = await asyncio.to_thread(
            checks.wait_for_line, run.server_log, TICK_CONFIRMED_LINE, HOOKS_BOUND_BUDGET, alive
        )
        if not tick:
            problems.append("the tick hook (RCONServer.update) never confirmed")
        failed = await asyncio.to_thread(checks.find_line, run.server_log, TRANSFORM_FAILED_LINE)
        if failed:
            problems.append(f"a transform failed: {failed[1].strip()[:200]}")
        watchdog = await asyncio.to_thread(checks.find_line, run.server_log, TICK_WATCHDOG_LINE)
        if watchdog:
            problems.append("the tick watchdog warned that the matcher bound nothing")
        unbound = await asyncio.to_thread(checks.find_line, run.server_log, UNBOUND_HOOKS_LINE)
        if unbound:
            problems.append(f"one or more agent hooks bound no method: {unbound[1].strip()[:200]}")
    return checks.CheckResult(
        "hooks-bound",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"transformed": transformed, "coverage": COVERAGE_NOTE, "problems": problems},
        {"file": run.server_log.name, "line": tick[0]} if tick else {"file": run.server_log.name},
    )


def _catalogue_problems(entries: Any, kind: str, spot: tuple[str, str]) -> tuple[list[str], dict[str, Any]]:
    """Human display names, and an honest count of the entries Build 42 does not name.

    Most of this game's 5 000-odd item scripts carry a display name. A few hundred do not:
    wounds, blood decals and zombie damage overlays are script rows the game itself never
    shows a player, and the connector reports those by their script id because inventing a
    name for them would be a lie. So the rule is not "no id is ever a name" — it is that
    the entries a player deals with are named, and that the share of unnamed rows stays
    where this build leaves it. A display-name lookup that broke would take that share far
    past the ceiling below, and this check would fail.
    """
    problems: list[str] = []
    detail: dict[str, Any] = {"count": 0, "spotCheck": {}}
    if not isinstance(entries, list) or not entries:
        return [f"{kind}: expected a non-empty list, got {entries!r}"], detail
    detail["count"] = len(entries)
    unnamed: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict):
            problems.append(f"{kind}: entry {entry!r} is not an object")
            continue
        code, name = entry.get("code"), entry.get("name")
        if not code or not name:
            problems.append(f"{kind}: entry {entry!r} is missing code or name")
            continue
        if _is_script_id(str(code), str(name)):
            unnamed.append(str(code))
    detail["unnamed"] = {
        "count": len(unnamed),
        "share": round(len(unnamed) / len(entries), 4),
        "ceiling": UNNAMED_CEILING,
        "examples": unnamed[:5],
        "note": (
            "Build 42 gives these script rows no display name of their own — wounds, blood "
            "decals and zombie damage overlays — so the connector reports them by id; every "
            "row a player can hold is named"
        ),
    }
    if len(unnamed) / len(entries) > UNNAMED_CEILING:
        problems.append(
            f"{kind}: {len(unnamed)} of {len(entries)} entries are reported by script id "
            f"(over the {UNNAMED_CEILING:.0%} this build leaves unnamed); the display-name lookup looks broken"
        )
    code, expected = spot
    match = next((e for e in entries if isinstance(e, dict) and e.get("code") == code), None)
    detail["spotCheck"] = {"code": code, "expected": expected, "actual": match.get("name") if match else None}
    if match is None:
        problems.append(f"{kind}: {code} is missing from the catalogue")
    elif match.get("name") != expected:
        problems.append(f"{kind}: {code} is named {match.get('name')!r}, expected {expected!r}")
    elif _is_script_id(code, str(match.get("name"))):
        problems.append(f"{kind}: {code} is reported by its script id, not a display name")
    return problems, detail


async def _check_catalog(fake: Any) -> checks.CheckResult:
    """Items and entities in one check, because this game answers both from one catalogue."""
    with checks._Timer() as timer:
        problems: list[str] = []
        detail: dict[str, Any] = {}
        for action, kind, spot in (
            # Base.Axe is the firefighter's axe in Build 42, read off a real run.
            ("listItems", "items", ("Base.Axe", "Firefighter Axe")),
            ("listEntities", "entities", ("Zombie", "Zombie")),
        ):
            try:
                entries = await fake.request(action, {}, timeout=120)
            except Exception as exc:  # noqa: BLE001 - reported as a check failure
                problems.append(f"{action} failed: {exc}")
                continue
            found, rows = _catalogue_problems(entries, kind, spot)
            problems += found
            detail[kind] = rows
    detail["problems"] = problems
    return checks.CheckResult("catalog", "pass" if not problems else "fail", timer.elapsed_ms, detail)


async def _check_rcon(fake: Any) -> checks.CheckResult:
    """The console this game really has: `players`, not Minecraft's `say`."""
    with checks._Timer() as timer:
        problems: list[str] = []
        result: Any = None
        try:
            result = await fake.request("executeConsoleCommand", {"command": "players"})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"executeConsoleCommand failed: {exc}")
        if not isinstance(result, dict) or result.get("success") is not True:
            problems.append(f"executeConsoleCommand returned {result!r}, expected success true")
        else:
            raw = str(result.get("rawResult") or result.get("raw") or result.get("output") or "")
            if not re.search(r"Players connected \(0\)", raw):
                problems.append(f"the console answered {raw!r}, expected 'Players connected (0)'")
    return checks.CheckResult(
        "rcon",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "command": "players",
            "result": result,
            "note": (
                "replaces the base `console` check, which sends `say`: Project Zomboid's "
                "console has no `say` command, so the base check would prove nothing"
            ),
            "problems": problems,
        },
    )


async def _check_action(run: Any, fake: Any) -> checks.CheckResult:
    """A representative action reaches the game and the agent answers it on the wire."""
    marker = f"takaro-verify-{run.options.run_id}-action"
    with checks._Timer() as timer:
        problems: list[str] = []
        sent: Any = None
        bans: Any = None
        try:
            sent = await fake.request("sendMessage", {"message": marker})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"sendMessage failed: {exc}")
        found = await asyncio.to_thread(checks.find_line, run.server_log, re.compile(re.escape(marker)))
        if not found:
            problems.append(f"the agent's debug log never showed the request carrying '{marker}'")
        try:
            bans = await fake.request("listBans", {})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"listBans failed: {exc}")
        if bans != []:
            problems.append(f"listBans returned {bans!r}, expected [] on a fresh server")
    return checks.CheckResult(
        "action",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "action": "sendMessage",
            "message": marker,
            "result": sent,
            "listBans": bans,
            "note": (
                "no in-game observer without a connected client: the wire response and the "
                "agent's own log are the evidence that the action reached the game"
            ),
            "problems": problems,
        },
        {"file": run.server_log.name, "line": found[0]} if found else {"file": run.server_log.name},
    )


async def _check_reconnect(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """Takaro drops the socket; the agent comes back and is usable again."""
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
            checks_lifecycle.wait_for_count, run.server_log, IDENTIFIED_LINE, 2, 30, alive
        )
        if confirmations < 2:
            problems.append(f"the agent logged {confirmations} successful identify(s), expected at least 2")
    return checks.CheckResult(
        "reconnect",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "closeCode": 1001,
            "reidentifyMs": int(reidentify_ms) if reidentify_ms is not None else None,
            "identifyCountBefore": before,
            "identifyCountAfter": fake.identify_count,
            "identifiedLines": confirmations,
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

    The image's entrypoint owns the exit, so the base ``shutdown`` check (which waits for
    the container to exit on its own) cannot be used here.
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
        _reclaim(run)
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


def _reclaim(run: Any) -> None:
    """Give the run's own files back to the user who started it.

    This image runs as root, so the cache files it wrote under the run's data directory
    belong to root and the runner's cleanup could not remove them.
    """
    import os

    completed = subprocess.run(
        [
            *docker_command(),
            "run",
            "--rm",
            "--entrypoint",
            "chown",
            "-v",
            f"{run.data_dir}:/t",
            run.resolved["containerRef"],
            "-R",
            f"{os.getuid()}:{os.getgid()}",
            "/t",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        output.warn(f"could not reclaim {run.data_dir}: {completed.stderr.strip()[:200]}")


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
    after_protocol=after_protocol,
    after_shutdown=after_shutdown,
    scan_runtime_identity=scan_runtime_identity,
)
