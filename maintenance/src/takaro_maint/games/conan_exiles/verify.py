"""Conan Exiles' verification hooks: the pinned server boots with the native connector inside it.

The connector is a native library preloaded into the server process (``LD_PRELOAD``). It
holds the Takaro WebSocket itself, accepts only ``wss://`` and validates the chain, so the
run's fake Takaro serves TLS from a throwaway CA (``GameHooks.takaro_tls``) and negotiates
protocol 1, as hosted Takaro does. Every Takaro value reaches the library through
``ConanSandbox/Saved/Config/Takaro/takaro.json`` (mode 0600), never a docker command line.

What a run claims, with nobody online:

- ``native-load``: the library loaded at the version this checkout carries, recognised the
  pinned build by its GNU build-id and signature scan, and hooked ``ProcessEvent``. A
  library that does not know the build still connects but refuses every action, so this is
  the compatibility claim.
- ``native-identify``: the game process registers with the Game Profile fields, offers
  protocol 1 with a manifest naming the pinned build, and answers an RFC 6455 ping.
- ``native-players``, ``native-catalog``, ``native-console``, ``action``, ``game-icon``,
  ``map``: representative actions through the hook, answered by the running game.
- ``reconnect``: Takaro drops the socket; the library comes back by itself.
- ``stop``: Takaro's ``shutdown`` action ends the server with exit code 0.

Everything that needs a player (chat both ways, joins and leaves, deaths, kills, kicks, bans,
teleports, items, inventories, locations) stays on the live runbook with a real client; see
``games/conan-exiles/README.md``. The base rows below look for Minecraft lines and ids, so each
one is excluded and names the Conan check that stands in for it.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import os
import re
import shutil
import time
from pathlib import Path
from typing import Any

from ... import output, paths
from ...verify import checks, checks_lifecycle
from ...verify.hooks import GameHooks
from . import SHUTDOWN_SECONDS

CHECK_IDS = (
    "native-load",
    "native-identify",
    "native-players",
    "native-catalog",
    "native-console",
    "action",
    "game-icon",
    "map",
    "reconnect",
    "stop",
)

READY_LINE = re.compile(r"LogInit: Display: Engine is initialized\. Leaving FEngineLoop::Init\(\)")

#: Base checks this connector cannot satisfy here, and the Conan check that stands in for each.
#: A run that names no ``--checks`` excludes these rather than failing them.
UNSUPPORTED_CHECKS = {
    "connector-load": "waits for the Java connectors' target-check line; `native-load` asserts the library and pins",
    "identify": (
        "waits for 'Identified successfully' on stdout; the library logs to TakaroConanNative.log, "
        "and `native-identify` asserts the frame, that log and the Game Profile"
    ),
    "heartbeat": "`native-identify` answers the empty RFC 6455 ping Takaro sends; there is no other heartbeat",
    "players": "`native-players` asserts the same empty list plus the reachability answer",
    "catalog-items": "spot-checks a Minecraft item id; `native-catalog` spot-checks Conan's ItemTable",
    "catalog-entities": "spot-checks a Minecraft entity id; `native-catalog` covers Conan's SpawnDataTable",
    "console": "drives a Minecraft `say`; `native-console` drives the Conan engine console",
    "shutdown": "allows 120 s; this server unloads for minutes after `exit`, and `stop` budgets for that",
}

#: What only a real client proves; listed in every report so no reader takes it as covered.
MANUAL_COVERAGE = (
    "events: player-connected, player-disconnected, chat-message, player-death, entity-killed",
    "actions with a player: sendMessage delivery, getPlayer, getPlayerLocation, getPlayerInventory, "
    "giveItem, teleportPlayer, kickPlayer, banPlayer/unbanPlayer/listBans",
    "Discord chat bridge both ways and module posts",
)

BANNER_MARKERS = ("LogInit: Build:", "LogInit: Engine Version:")
SERVER_LOG = Path("ConanSandbox") / "Saved" / "Logs" / "ConanSandbox.log"
NATIVE_LOG = Path("ConanSandbox") / "Saved" / "Logs" / "TakaroConanNative.log"
TAKARO_CONFIG = Path("ConanSandbox") / "Saved" / "Config" / "Takaro" / "takaro.json"
#: The run's throwaway CA, next to the config the library reads it from.
VERIFY_CA = Path("ConanSandbox") / "Saved" / "Config" / "Takaro" / "verify-ca.pem"
VERSION_FILE = Path("games") / "conan-exiles" / "version.txt"

GAME = "conan-exiles"
CONNECTOR_NAME = "takaro-conan-exiles"

# What the library writes into TakaroConanNative.log.
LOADING_LINE = re.compile(r"Takaro Conan native (?P<version>\S+) loading")
PINS_LINE = re.compile(r"pins: build-id (?P<buildid>\S+)(?: = build (?P<build>\d+))?, scan [\d.]+ ms: (?P<verdict>.*)$")
HOOKED_LINE = re.compile(r"ProcessEvent hooked at 0x[0-9a-f]+")
NO_HOOK_LINE = re.compile(r"NO HOOK INSTALLED: (?P<reason>.*)$")
BRIDGE_LINE = re.compile(r"Takaro bridge started \((?P<state>[^)]+)\)")
IDENTIFIED_LINE = re.compile(r"takaro: identified\b.*, protocol (?P<protocol>\d+);")
CLOSED_LINE = re.compile(r"takaro: WebSocket closed \(epoch \d+\)")
STOPPED_LINE = re.compile(r"Takaro Conan native stopped")

LOAD_BUDGET = 120.0
IDENTIFY_BUDGET = 180.0
# The world loads after identify, and the game-thread reads wait for it.
WORLD_BUDGET = 300.0
REQUEST_TIMEOUT = 60.0
#: How long to wait before asking a world that is still loading again.
RETRY_SECONDS = 5.0
RECONNECT_BUDGET = 90.0
# The server needs about three minutes to unload after `exit`, at full CPU on four cores.
STOP_BUDGET = 600.0
#: How long a refused shutdown is given anyway, in case the server goes down by itself.
REFUSED_STOP_BUDGET = 30.0
#: Every answer from a library that refused the build carries this, and no later answer differs.
REFUSED_BUILD = "unsupported server build"

MIN_ITEMS = 1000
#: The share of rows that may carry their code as their name; observed about 1.2 %.
MAX_CODE_NAMED = 0.05
#: What the library answers a message nobody is online to receive.
NOBODY_ONLINE = "No online Conan players are available for chat"
MIN_ENTITIES = 1000
#: Rows whose localised names the live hard test read back from the client.
ITEM_NAMES = ("Stone", "Plant Fiber")
ENTITY_NAMES = ("Rabbit", "Hyena")
CONSOLE_COMMAND = "log LogTemp Verbose"
#: An RCON verb the library refuses in favour of the Takaro action that does the job.
REFUSED_COMMAND = "listplayers"


# --------------------------------------------------------------------------- run setup


def before_boot(run: Any, takaro_env: dict[str, str]) -> Path:
    """The library's whole configuration for this boot, before the container starts.

    Environment would win over the file, but it would also put the tokens on a docker
    command line; the file (mode 0600) keeps them off it.
    """
    config: dict[str, Any] = {
        "url": takaro_env.get("TAKARO_WS_URL", ""),
        "identityToken": takaro_env.get("TAKARO_IDENTITY_TOKEN", ""),
        "registrationToken": takaro_env["TAKARO_REGISTRATION_TOKEN"],
        "name": f"takaro-verify-{run.options.run_id}",
    }
    ca_file = getattr(run, "takaro_ca_file", None)
    if ca_file is not None:
        ca = run.data_dir / VERIFY_CA
        ca.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ca_file, ca)
        config["caFile"] = f"/conan/{VERIFY_CA.as_posix()}"
    path = run.data_dir / TAKARO_CONFIG
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({k: v for k, v in config.items() if v}, indent=2) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)
    output.info(f"wrote {TAKARO_CONFIG.as_posix()} for this run (mode 0600)")
    return path


def scan_runtime_identity(adapter: Any, log_file: Path) -> dict[str, Any]:
    """The build and the engine that actually booted, merged from the two banner lines."""
    identity: dict[str, Any] = {}
    if not log_file.is_file():
        return identity
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


def expected_version() -> str:
    """The connector version this checkout ships; the library stamps the same string."""
    return (paths.repo_root() / VERSION_FILE).read_text(encoding="utf-8").strip()


def native_log(run: Any) -> Path:
    return Path(run.data_dir) / NATIVE_LOG


def _capability(capabilities: Any, name: str) -> dict[str, Any] | None:
    if not isinstance(capabilities, list):
        return None
    return next((c for c in capabilities if isinstance(c, dict) and c.get("name") == name), None)


def latest_capabilities(fake: Any) -> Any:
    """The manifest Takaro holds now: the last replacement, else the one identify offered."""
    for update in reversed(getattr(fake, "capability_updates", [])):
        if isinstance(update, dict) and isinstance(update.get("capabilities"), list):
            return update["capabilities"]
    return (fake.identified or {}).get("capabilities")


async def _request_until(
    fake: Any, action: str, args: dict[str, Any], budget: float, accept: Any
) -> tuple[Any, list[str]]:
    """Ask until the answer is acceptable or the budget is spent; the world loads after identify.

    Returns the last answer and every distinct error seen, so a check that still fails can
    say what the library answered rather than only that it never agreed.
    """
    deadline = time.monotonic() + budget
    errors: list[str] = []
    answer: Any = None
    while True:
        try:
            answer = await fake.request(action, args, timeout=REQUEST_TIMEOUT)
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            text = f"{action} failed: {exc}"
            if text not in errors:
                errors.append(text)
            answer = None
            if REFUSED_BUILD in text:
                return answer, errors
        else:
            if accept(answer):
                return answer, errors
        if time.monotonic() >= deadline:
            return answer, errors
        await asyncio.sleep(RETRY_SECONDS)


# --------------------------------------------------------------------------- the checks


async def after_protocol(run: Any, fake: Any, alive: Any) -> None:
    for check_id, coroutine in (
        ("native-load", lambda: asyncio.to_thread(_check_load, run, alive)),
        ("native-identify", lambda: _check_identify(run, fake, alive)),
        ("native-players", lambda: _check_players(fake)),
        ("native-catalog", lambda: _check_catalog(fake)),
        ("native-console", lambda: _check_console(fake)),
        ("action", lambda: _check_action(run, fake)),
        ("game-icon", lambda: _check_game_icon(fake)),
        ("map", lambda: _check_map(fake, lambda: checks.find_line(native_log(run), NO_HOOK_LINE) is not None)),
        ("reconnect", lambda: _check_reconnect(run, fake, alive)),
    ):
        if run.wanted(check_id):
            run.record(await coroutine())
        else:
            run.skip(check_id, "not selected by --checks")


def _check_load(run: Any, alive: Any) -> checks.CheckResult:
    """The compatibility claim: this library, on this build, with its hook in place."""
    log_file = native_log(run)
    revision = str(run.target.record["revision"])
    version = expected_version()
    with checks._Timer() as timer:
        problems: list[str] = []
        bridge = checks.wait_for_line(log_file, BRIDGE_LINE, LOAD_BUDGET, alive)
        loading = checks.find_line(log_file, LOADING_LINE)
        pins = checks.find_line(log_file, PINS_LINE)
        hooked = checks.find_line(log_file, HOOKED_LINE)
        refused = checks.find_line(log_file, NO_HOOK_LINE)
        loaded_version = LOADING_LINE.search(loading[1]).group("version") if loading else None  # type: ignore[union-attr]
        pin = PINS_LINE.search(pins[1]) if pins else None
        state = BRIDGE_LINE.search(bridge[1]).group("state") if bridge else None  # type: ignore[union-attr]
        if not loading:
            problems.append(f"{log_file.name} never showed 'Takaro Conan native … loading': LD_PRELOAD did not load it")
        elif loaded_version != version:
            problems.append(f"the library is {loaded_version}, this checkout ships {version}")
        if pin is None:
            problems.append(f"{log_file.name} never reported its pin scan")
        else:
            if pin.group("build") != revision:
                problems.append(f"the library recognised build {pin.group('build')!r}, the target is {revision}")
            if pin.group("verdict").strip() != "verified":
                problems.append(f"pin scan: {pin.group('verdict').strip()}")
        if refused:
            problems.append(f"NO HOOK INSTALLED: {NO_HOOK_LINE.search(refused[1]).group('reason')}")  # type: ignore[union-attr]
        elif not hooked:
            problems.append("ProcessEvent was never hooked")
        if state != "ready":
            problems.append(f"the bridge started as {state!r}, expected 'ready'")
    return checks.CheckResult(
        "native-load",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "libraryVersion": loaded_version,
            "expectedVersion": version,
            "buildId": pin.group("buildid") if pin else None,
            "build": pin.group("build") if pin else None,
            "expectedBuild": revision,
            "pinVerdict": pin.group("verdict").strip() if pin else None,
            "bridge": state,
            "note": "a compatibility claim: on a build it does not know the library still runs and refuses actions",
            "problems": problems,
        },
        {"file": log_file.name, "line": bridge[0]} if bridge else {"file": log_file.name},
    )


async def _check_identify(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """The game process registers itself, with the Game Profile, over protocol 1."""
    log_file = native_log(run)
    revision = str(run.target.record["revision"])
    version = expected_version()
    with checks._Timer() as timer:
        problems: list[str] = []
        round_trip_ms: int | None = None
        try:
            await fake.wait_for_identify(IDENTIFY_BUDGET)
        except TimeoutError as exc:
            problems.append(str(exc))
        identity = fake.identified or {}
        for key in ("identityToken", "registrationToken"):
            if key not in identity:
                problems.append(f"the identify frame carried no {key}")
        if identity.get("game") != GAME:
            problems.append(f"identify game is {identity.get('game')!r}, expected {GAME!r}")
        connector: dict[str, Any] = identity["connector"] if isinstance(identity.get("connector"), dict) else {}
        if connector.get("name") != CONNECTOR_NAME or connector.get("version") != version:
            problems.append(f"identify connector is {connector!r}, expected {CONNECTOR_NAME} {version}")
        if 1 not in (identity.get("protocolVersions") or []):
            problems.append(f"identify offered protocols {identity.get('protocolVersions')!r}, expected 1 among them")
        profile = _capability(identity.get("capabilities"), "game.profile") or {}
        parameters: dict[str, Any] = profile["parameters"] if isinstance(profile.get("parameters"), dict) else {}
        if str(parameters.get("version")) != revision:
            problems.append(
                f"game.profile declares version {parameters.get('version')!r}, the target is build {revision} "
                "(a refused build declares none)"
            )
        line = await asyncio.to_thread(checks.wait_for_line, log_file, IDENTIFIED_LINE, 30, alive)
        protocol = IDENTIFIED_LINE.search(line[1]).group("protocol") if line else None  # type: ignore[union-attr]
        if protocol != "1":
            problems.append(f"{log_file.name} reports protocol {protocol!r} after identify, expected '1'")
        try:
            round_trip_ms = int(await fake.ping(timeout=5, payload=b"") * 1000)
        except (TimeoutError, RuntimeError) as exc:
            problems.append(f"an empty RFC 6455 ping was not answered: {exc}")
    return checks.CheckResult(
        "native-identify",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "identifyFrames": fake.identify_count,
            "identifyKeys": sorted(identity),
            "game": identity.get("game"),
            "connector": connector,
            "protocolVersions": identity.get("protocolVersions"),
            "negotiatedProtocol": getattr(fake, "negotiated_protocol", None),
            "gameProfile": parameters,
            "capabilities": sorted(
                str(c.get("name")) for c in identity.get("capabilities") or [] if isinstance(c, dict)
            ),
            "pingRoundTripMs": round_trip_ms,
            "problems": problems,
        },
        {"file": log_file.name, "line": line[0]} if line else {"file": log_file.name},
    )


async def _check_players(fake: Any) -> checks.CheckResult:
    """``testReachability``, and ``getPlayers`` read from the live GameState: nobody is online."""
    with checks._Timer() as timer:
        problems: list[str] = []
        reachable: Any = None
        try:
            reachable = await fake.request("testReachability", {}, timeout=REQUEST_TIMEOUT)
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"testReachability failed: {exc}")
        if not isinstance(reachable, dict) or reachable.get("connectable") is not True:
            problems.append(f"testReachability returned {reachable!r}, expected connectable true")
        players, errors = await _request_until(fake, "getPlayers", {}, WORLD_BUDGET, lambda a: isinstance(a, list))
        if not isinstance(players, list):
            problems += errors or [f"getPlayers returned {players!r}, expected a list"]
        elif players:
            problems.append(f"getPlayers returned {len(players)} player(s); nobody joins a verification run")
    return checks.CheckResult(
        "native-players",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "testReachability": reachable,
            "players": players,
            "note": "getPlayers walks GameState.PlayerArray through the hooked engine; an empty list is the answer",
            "problems": problems,
        },
    )


def _catalogue_problems(action: str, entries: Any, minimum: int, names: tuple[str, ...]) -> list[str]:
    if not isinstance(entries, list) or not entries:
        return [f"{action} returned {str(entries)[:200]!r}, expected a non-empty list"]
    problems: list[str] = []
    if len(entries) < minimum:
        problems.append(f"{action} returned {len(entries)} entries, expected at least {minimum}")
    malformed = next((e for e in entries if not isinstance(e, dict) or not e.get("code") or not e.get("name")), None)
    if malformed is not None:
        problems.append(f"{action} holds an entry without a code and a name: {malformed!r}")
        return problems
    # Many ItemTable rows are named after their item ('Absinthe' -> 'Absinthe'), so only a
    # catalogue where the codes came back in place of the names fails.
    same = [e for e in entries if e["name"] == e["code"]]
    if len(same) > len(entries) * MAX_CODE_NAMED:
        problems.append(f"{action}: {len(same)} of {len(entries)} names are the code itself, e.g. {same[0]['code']!r}")
    known = {str(e["name"]) for e in entries}
    missing = [name for name in names if name not in known]
    if missing:
        problems.append(f"{action} has no entry named {', '.join(missing)}")
    return problems


async def _check_catalog(fake: Any) -> checks.CheckResult:
    """Conan's ItemTable and SpawnDataTable, with the localised names the client shows."""
    with checks._Timer() as timer:
        problems: list[str] = []
        counts: dict[str, int] = {}
        samples: dict[str, list[str]] = {}
        for action, minimum, names in (
            ("listItems", MIN_ITEMS, ITEM_NAMES),
            ("listEntities", MIN_ENTITIES, ENTITY_NAMES),
        ):
            entries, errors = await _request_until(
                fake, action, {}, WORLD_BUDGET, lambda a: isinstance(a, list) and len(a) > 0
            )
            found = _catalogue_problems(action, entries, minimum, names)
            problems += (errors if found and errors else []) + found
            if isinstance(entries, list):
                counts[action] = len(entries)
                samples[action] = [str(e.get("name")) for e in entries[:5] if isinstance(e, dict)]
    return checks.CheckResult(
        "native-catalog",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "counts": counts,
            "firstNames": samples,
            "spotNames": {"listItems": list(ITEM_NAMES), "listEntities": list(ENTITY_NAMES)},
            "problems": problems,
        },
    )


async def _check_console(fake: Any) -> checks.CheckResult:
    """The engine console answers in-band; an RCON verb is refused with the Takaro action to use."""
    with checks._Timer() as timer:
        problems: list[str] = []
        result: Any = None
        refusal: str | None = None
        result, errors = await _request_until(
            fake,
            "executeConsoleCommand",
            {"command": CONSOLE_COMMAND},
            WORLD_BUDGET,
            lambda a: isinstance(a, dict) and a.get("success") is True,
        )
        if not isinstance(result, dict) or result.get("success") is not True:
            problems += errors or [f"executeConsoleCommand returned {result!r}, expected success true"]
        try:
            refused = await fake.request("executeConsoleCommand", {"command": REFUSED_COMMAND}, timeout=REQUEST_TIMEOUT)
        except Exception as exc:  # noqa: BLE001 - the refusal is the expected answer
            refusal = str(exc)
        else:
            if isinstance(refused, dict) and refused.get("success") is False:
                refusal = str(refused.get("rawResult"))
            else:
                problems.append(f"'{REFUSED_COMMAND}' returned {refused!r}, expected a refusal")
    return checks.CheckResult(
        "native-console",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "command": CONSOLE_COMMAND,
            "result": result,
            "refusedCommand": REFUSED_COMMAND,
            "refusal": refusal,
            "problems": problems,
        },
    )


async def _check_action(run: Any, fake: Any) -> checks.CheckResult:
    """``sendMessage`` end to end; with nobody online the game thread delivers it to no one."""
    marker = f"takaro-verify-{run.options.run_id}-action"
    with checks._Timer() as timer:
        problems: list[str] = []
        result: Any = None
        try:
            result = await fake.request("sendMessage", {"message": marker, "opts": {}}, timeout=REQUEST_TIMEOUT)
        except Exception as exc:  # noqa: BLE001 - the nobody-online refusal is the expected answer
            if NOBODY_ONLINE in str(exc) and "game_error" in str(exc):
                result = {"refusal": str(exc)}
            else:
                problems.append(f"sendMessage failed: {exc}")
    return checks.CheckResult(
        "action",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "action": "sendMessage",
            "message": marker,
            "result": result,
            "note": (
                "with nobody online the library answers the structured game_error from its own list of "
                "online controllers: the request path into the game, not delivery"
            ),
            "problems": problems,
        },
    )


async def _check_game_icon(fake: Any) -> checks.CheckResult:
    """``getGameIcon`` returns the PNG whose hash the identify's ``game.profile`` declared."""
    with checks._Timer() as timer:
        problems: list[str] = []
        declared = (
            (_capability((fake.identified or {}).get("capabilities"), "game.profile") or {}).get("parameters") or {}
        ).get("icon") or {}
        icon: Any = None
        size: int | None = None
        try:
            icon = await fake.request("getGameIcon", {}, timeout=REQUEST_TIMEOUT)
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"getGameIcon failed: {exc}")
        if isinstance(icon, dict):
            try:
                data = base64.b64decode(str(icon.get("data", "")), validate=True)
            except ValueError as exc:
                problems.append(f"getGameIcon data is not base64: {exc}")
                data = b""
            size = len(data)
            if not data.startswith(b"\x89PNG\r\n\x1a\n"):
                problems.append("getGameIcon data is not a PNG")
            if hashlib.sha256(data).hexdigest()[:12] != icon.get("revision"):
                problems.append("getGameIcon revision is not the first 12 hex of the PNG's sha256")
            if declared.get("revision") != icon.get("revision") or declared.get("size") != size:
                problems.append(
                    f"game.profile declared icon {declared!r}, getGameIcon served {icon.get('revision')!r}/{size}"
                )
        elif not problems:
            problems.append(f"getGameIcon returned {icon!r}, expected {{revision, data}}")
    return checks.CheckResult(
        "game-icon",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "declared": declared,
            "revision": icon.get("revision") if isinstance(icon, dict) else None,
            "bytes": size,
            "problems": problems,
        },
    )


async def _check_map(fake: Any, refused: Any = lambda: False) -> checks.CheckResult:
    """The loaded map reaches Takaro: the library reads the live GameState and replaces its manifest.

    ``refused`` says the library refused the build, which then never declares a map.
    """
    with checks._Timer() as timer:
        problems: list[str] = []
        deadline = time.monotonic() + WORLD_BUDGET
        dimensions: Any = None
        while time.monotonic() < deadline:
            parameters = (_capability(latest_capabilities(fake), "map") or {}).get("parameters") or {}
            dimensions = parameters.get("dimensions")
            if dimensions or refused():
                break
            await asyncio.sleep(RETRY_SECONDS)
        if not isinstance(dimensions, list) or not dimensions:
            problems.append(
                "no `map` capability: the library refused the build"
                if refused()
                else f"no `map` capability with a dimension within {WORLD_BUDGET:.0f} s of the boot"
            )
        replacements = len(getattr(fake, "capability_updates", []))
    return checks.CheckResult(
        "map",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "dimensions": [d.get("id") if isinstance(d, dict) else d for d in dimensions or []],
            "manifestReplacements": replacements,
            "problems": problems,
        },
    )


async def _check_reconnect(run: Any, fake: Any, alive: Any) -> checks.CheckResult:
    """Takaro drops the socket; the library comes back by itself and is usable again."""
    log_file = native_log(run)
    with checks._Timer() as timer:
        problems: list[str] = []
        reachable: Any = None
        before = fake.identify_count
        await fake.disconnect(1001, "going away")
        reidentify_ms = await checks_lifecycle.identify_within(fake, before + 1, RECONNECT_BUDGET, alive)
        if reidentify_ms is None:
            problems.append(f"no identify frame arrived within {RECONNECT_BUDGET:.0f} s of the close")
        else:
            try:
                reachable = await fake.request("testReachability", {}, timeout=REQUEST_TIMEOUT)
            except Exception as exc:  # noqa: BLE001 - reported as a check failure
                problems.append(f"testReachability failed on the reconnected socket: {exc}")
            if not isinstance(reachable, dict) or reachable.get("connectable") is not True:
                problems.append(f"testReachability returned {reachable!r}, expected connectable true")
        closed = await asyncio.to_thread(checks.wait_for_line, log_file, CLOSED_LINE, 30, alive)
        if not closed:
            problems.append(f"{log_file.name} never logged the close")
        confirmations = await asyncio.to_thread(
            checks_lifecycle.wait_for_count, log_file, IDENTIFIED_LINE, 2, 30, alive
        )
        if confirmations < 2:
            problems.append(f"the library identified {confirmations} time(s), expected at least 2")
    return checks.CheckResult(
        "reconnect",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "closeCode": 1001,
            "reidentifyMs": int(reidentify_ms) if reidentify_ms is not None else None,
            "identifyCountBefore": before,
            "identifyCountAfter": fake.identify_count,
            "identifyLines": confirmations,
            "testReachability": reachable,
            "problems": problems,
        },
        {"file": log_file.name},
    )


# --------------------------------------------------------------------------- shutdown


async def after_shutdown(run: Any, fake: Any, ws_url: str, ledger_inputs: list[dict[str, Any]]) -> None:
    """Takaro's ``shutdown`` stops the server; then keep the logs it wrote inside its own tree."""
    del ws_url, ledger_inputs
    if run.wanted("stop"):
        run.record(await _check_stop(run, fake))
    else:
        run.skip("stop", "not selected by --checks")
    for relative in (SERVER_LOG, NATIVE_LOG):
        source = run.data_dir / relative
        if not source.is_file():
            continue
        destination = run.out / relative.name
        destination.write_bytes(source.read_bytes())
        if destination not in run.extra_logs:
            run.extra_logs.append(destination)


async def _check_stop(run: Any, fake: Any) -> checks.CheckResult:
    with checks._Timer() as timer:
        problems: list[str] = []
        answer: Any = None
        try:
            answer = await fake.request("shutdown", {}, timeout=REQUEST_TIMEOUT)
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"shutdown failed: {exc}")
        budget = REFUSED_STOP_BUDGET if problems else STOP_BUDGET
        code = await asyncio.to_thread(run.container.wait_for_exit, budget)
        if code != 0:
            problems.append(f"the server container exited {code} within {budget:.0f} s, expected 0")
        stopped = checks.find_line(native_log(run), STOPPED_LINE)
        if not stopped:
            problems.append(f"{NATIVE_LOG.name} never logged 'Takaro Conan native stopped'")
    return checks.CheckResult(
        "stop",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "answer": answer,
            "exitCode": code,
            "countdownSeconds": SHUTDOWN_SECONDS,
            "budgetSeconds": budget,
            "problems": problems,
        },
        {"file": NATIVE_LOG.name, "line": stopped[0]} if stopped else {"file": NATIVE_LOG.name},
    )


#: What this game contributes to a verification run; the runner reads nothing else.
HOOKS = GameHooks(
    ready_line=READY_LINE,
    startup_timeout=900.0,
    check_ids=CHECK_IDS,
    takaro_tls=True,
    takaro_protocol=1,
    unsupported_checks=UNSUPPORTED_CHECKS,
    manual_coverage=MANUAL_COVERAGE,
    before_boot=before_boot,
    after_protocol=after_protocol,
    after_shutdown=after_shutdown,
    scan_runtime_identity=scan_runtime_identity,
)
