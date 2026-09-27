"""Exact-build ARK verification using the shipped native and Generic sidecar artifacts.

A bare run can prove startup, native health, Generic identify, empty roster, outbound
request acknowledgement, and reconnect. Inbound chat and player-specific location or
inventory need a real client; those checks fail with that limitation rather than report
empty synthetic data as success. No Minecraft-specific base command is reused.
"""

from __future__ import annotations

import asyncio
import ctypes
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

from ... import net
from ...verify import checks
from ...verify.hooks import GameHooks
from ...verify.runner import Container, docker_command
from .readonly_base import prepare as prepare_readonly_base

CHECK_IDS = (
    "native-health",
    "sidecar-identify",
    "ark-heartbeat",
    "roster",
    "location",
    "chat-in",
    "chat-out",
    "reconnect",
    "inventory",
    "catalog",
    "entities",
    "ark-empty-broadcast",
    "ark-targeted-offline",
    "ark-console",
    "ark-saveworld",
    "native-shutdown",
    "owned-save-reload",
    "event",
    "stop",
    "negative-wrong-target",
)
READY_LINE = re.compile(r"ARK_NATIVE_DIAG .*main-loop-tick count=1")
SHUTDOWN_SAVE_LINE = re.compile(r"native-shutdown-synchronous-save-completed-before-ack")
SHUTDOWN_REQUEST_LINE = re.compile(r"ARK_NATIVE_SHUTDOWN native-exit-requested$")
SAVEWORLD_COMPLETE_LINE = re.compile(r"World Save Complete[.] Took [0-9.]+$")
SHUTDOWN_MARKER_TIMEOUT = 8.0
_IN_ACCESS = 0x00000001
_IN_CLOSE_NOWRITE = 0x00000010
_IN_OPEN = 0x00000020
_IN_DELETE_SELF = 0x00000400
_IN_MOVE_SELF = 0x00000800
_IN_Q_OVERFLOW = 0x00004000
_INOTIFY_EVENT = struct.Struct("iIII")
_OWNED_PATH_PROBE_SCRIPT = (
    "for exe in /proc/[0-9]*/exe; do "
    "pid=${exe#/proc/}; pid=${pid%/exe}; "
    'target=$(readlink -f "$exe" 2>/dev/null) || continue; '
    'if [ "${target##*/}" = ShooterGameServer ]; then '
    'printf \'game %s %s\\n\' "$pid" "$target"; fi; '
    "done; printf 'saved %s\\n' \"$(readlink -f /ark/ShooterGame/Saved)\""
)
UNSUPPORTED_CHECKS = {
    "connector-load": "ARK native /health is checked by native-health",
    "identify": "the Generic sidecar identify frame is checked by sidecar-identify",
    "heartbeat": "ARK sidecar WebSocket ping/pong and reachability are checked by ark-heartbeat",
    "players": "ARK roster is checked by roster",
    "catalog-items": "ARK item catalog is checked by catalog",
    "catalog-entities": "ARK entity catalog is checked by entities",
    "console": "ARK console support is checked by ark-console",
    "shutdown": "ARK native shutdown acknowledgement and process exit are checked by native-shutdown",
}
SIDECAR_FOLDER = Path("TakaroArk/TakaroArkSidecar")
NATIVE_PORT = 18891
SIDECAR_PORT = 18892
CONSOLE_SAFETY_PROBES = (
    ("Exit", "Use the dedicated shutdown action"),
    ("  qUiT extra", "Use the dedicated shutdown action"),
    ("DoExit", "Use the dedicated shutdown action"),
    ("cheat DoExit", "Use the dedicated shutdown action"),
    ("GetAll Foo; Quit", "Use the dedicated shutdown action"),
    ("\u00a0Exit", "Unsupported command whitespace"),
)


def native_token(registration_token: str) -> str:
    return hashlib.sha256(b"takaro-ark-native:" + registration_token.encode()).hexdigest()


def start_sidecar(run: Any, fake: Any, *, suffix: str = "") -> Container:
    source = run.data_dir / SIDECAR_FOLDER
    required = ("Dockerfile", "dist/index.js", "package-lock.json")
    if any(not (source / name).is_file() for name in required):
        raise RuntimeError("the shipped ARK sidecar folder is incomplete")
    image = f"takaro-ark-sidecar:tm-{run.options.run_id}"
    build = subprocess.run(
        [
            *docker_command(),
            "build",
            "-f",
            str(source / "Dockerfile"),
            "-t",
            image,
            "--label",
            f"tm.run={run.options.run_id}",
            str(source),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    (run.out / "sidecar-build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
    if build.returncode:
        raise RuntimeError("shipped ARK sidecar image build failed; see sidecar-build.log")
    takaro = run.takaro_env(f"ws://{fake.host}:{fake.port}/")
    token = native_token(takaro["TAKARO_REGISTRATION_TOKEN"])
    name = f"{run.container.name}-sidecar"
    argv = [
        *docker_command(),
        "run",
        "-d",
        "--name",
        name,
        "--label",
        f"tm.run={run.options.run_id}",
        "--label",
        f"tm.ttl={int(time.time()) + 10800}",
        "--network",
        f"container:{run.container.name}",
        "--memory",
        "512m",
    ]
    for label in run.options.labels:
        argv += ["--label", label]
    env = {
        "TAKARO_WS_URL": takaro["TAKARO_WS_URL"],
        "TAKARO_IDENTITY_TOKEN": takaro["TAKARO_IDENTITY_TOKEN"],
        "TAKARO_REGISTRATION_TOKEN": takaro["TAKARO_REGISTRATION_TOKEN"],
        "TAKARO_SERVER_NAME": f"takaro-verify-{run.options.run_id}",
        "ARK_NATIVE_URL": f"http://127.0.0.1:{NATIVE_PORT}",
        "ARK_NATIVE_TOKEN": token,
        "TAKARO_CURSOR_FILE": "/data/event-cursor.json",
        "SIDECAR_HEALTH_PORT": str(SIDECAR_PORT),
    }
    for key, value in sorted(env.items()):
        argv += ["-e", f"{key}={value}"]
    sidecar_data = run.data_dir / ".takaro/runtime/sidecar-data"
    sidecar_data.mkdir(parents=True, exist_ok=True)
    argv += ["-v", f"{sidecar_data}:/data", image]
    container = Container(
        name=name,
        argv=argv,
        log_file=run.out / f"sidecar{suffix}.log",
        docker_log=run.docker_log,
        secrets=[takaro["TAKARO_REGISTRATION_TOKEN"], token],
    )
    run.containers.append(container)
    run.extra_logs.append(container.log_file)
    container.start()
    return container


def _get_json(container: str, url: str, *, native: bool = False) -> Any:
    # Run inside the sidecar namespace. Token stays in that process environment and is
    # never interpolated into the docker argv or logs.
    script = (
        (
            "fetch(process.argv[1],{headers:{Authorization:'Bearer '+process.env.ARK_NATIVE_TOKEN}})"
            ".then(async r=>{if(!r.ok)throw Error('HTTP '+r.status);console.log(await r.text())})"
            ".catch(e=>{console.error(e.message);process.exitCode=1})"
        )
        if native
        else (
            "fetch(process.argv[1]).then(async r=>{if(!r.ok)throw Error('HTTP '+r.status);"
            "console.log(await r.text())}).catch(e=>{console.error(e.message);process.exitCode=1})"
        )
    )
    proc = subprocess.run(
        [*docker_command(), "exec", container, "node", "-e", script, url], capture_output=True, text=True, check=False
    )
    if proc.returncode:
        raise RuntimeError(f"sidecar namespace HTTP request failed: {proc.stderr.strip()[:160]}")
    return json.loads(proc.stdout)


def _native_console_probe(container: str, command: str) -> dict[str, Any]:
    """Observe the native HTTP status/body for a rejected termination token."""
    script = (
        "fetch('http://127.0.0.1:18891/console',{method:'POST',"
        "headers:{Authorization:'Bearer '+process.env.ARK_NATIVE_TOKEN,"
        "'Content-Type':'text/plain; charset=utf-8'},body:process.argv[1]})"
        ".then(async r=>console.log(JSON.stringify({status:r.status,body:await r.json()})))"
        ".catch(e=>{console.error(e.message);process.exitCode=1})"
    )
    proc = subprocess.run(
        [*docker_command(), "exec", container, "node", "-e", script, command],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    if proc.returncode:
        raise RuntimeError(f"native console safety probe failed: {proc.stderr.strip()[:160]}")
    result = json.loads(proc.stdout)
    if not isinstance(result, dict):
        raise RuntimeError("native console safety probe returned no structured response")
    return result


def _native_message_probe(container: str, path: str, message: str) -> dict[str, Any]:
    """Read an authenticated native message status without exposing the token in argv."""
    script = (
        "fetch('http://127.0.0.1:18891'+process.argv[1],{method:'POST',"
        "headers:{Authorization:'Bearer '+process.env.ARK_NATIVE_TOKEN,"
        "'Content-Type':'text/plain; charset=utf-8'},body:process.argv[2]})"
        ".then(async r=>console.log(JSON.stringify({status:r.status,body:await r.json()})))"
        ".catch(e=>{console.error(e.message);process.exitCode=1})"
    )
    proc = subprocess.run(
        [*docker_command(), "exec", container, "node", "-e", script, path, message],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    if proc.returncode:
        raise RuntimeError(f"native message probe failed: {proc.stderr.strip()[:160]}")
    result = json.loads(proc.stdout)
    if not isinstance(result, dict):
        raise RuntimeError("native message probe returned no structured response")
    return result


async def _fresh_saveworld_events(fake: Any, baseline: int, *, timeout: float = 10.0) -> dict[str, str | None]:
    """Observe ordered native log events delivered to Takaro after this request began."""
    deadline = time.monotonic() + timeout
    while True:
        lines = [
            event["data"]["msg"]
            for event in fake.events[baseline:]
            if isinstance(event, dict)
            and event.get("type") == "log"
            and isinstance(event.get("data"), dict)
            and isinstance(event["data"].get("msg"), str)
        ]
        start = next((i for i, line in enumerate(lines) if "Saving world..." in line), None)
        complete = next(
            (
                line
                for i, line in enumerate(lines)
                if start is not None and i > start and SAVEWORLD_COMPLETE_LINE.search(line)
            ),
            None,
        )
        if complete or time.monotonic() >= deadline:
            return {"start": lines[start] if start is not None else None, "complete": complete}
        await asyncio.sleep(0.2)


def _wait_json(container: str, url: str, *, native: bool = False, timeout: float = 30) -> Any:
    deadline = time.monotonic() + timeout
    while True:
        try:
            return _get_json(container, url, native=native)
        except (RuntimeError, json.JSONDecodeError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(1)


def _result(check_id: str, problems: list[str], started: float, **detail: Any) -> checks.CheckResult:
    return checks.CheckResult(
        check_id,
        "fail" if problems else "pass",
        int((time.monotonic() - started) * 1000),
        {**detail, "problems": problems},
    )


def _fresh_shutdown_markers(log_file: Path, device: int, inode: int, offset: int) -> dict[str, str | None]:
    """Find both shutdown markers only in bytes appended after this request began."""
    deadline = time.monotonic() + SHUTDOWN_MARKER_TIMEOUT
    while True:
        stat = log_file.stat()
        if (stat.st_dev, stat.st_ino) != (device, inode) or stat.st_size < offset:
            raise RuntimeError("server log rotated or truncated during native shutdown")
        with log_file.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if (opened.st_dev, opened.st_ino) != (device, inode) or opened.st_size < offset:
                raise RuntimeError("server log rotated or truncated during native shutdown")
            stream.seek(offset)
            lines = stream.read().decode("utf-8", errors="replace").splitlines()
        save = next((line for line in lines if SHUTDOWN_SAVE_LINE.search(line)), None)
        request_line = next((line for line in lines if SHUTDOWN_REQUEST_LINE.search(line)), None)
        if (save and request_line) or time.monotonic() >= deadline:
            return {"save": save, "request": request_line}
        time.sleep(0.2)


def _native_protocol_ready(health: dict[str, Any], build: str) -> bool:
    capabilities = health.get("capabilities")
    return (
        health.get("status") == "ok"
        and health.get("build") == build
        and bool(health.get("bootId"))
        and isinstance(capabilities, dict)
        and all(capabilities.get(name) == "ok" for name in ("roster", "items", "entities"))
    )


def _owned_runtime_paths_valid(lines: list[str]) -> bool:
    games = [line.split(" ", 2) for line in lines if line.startswith("game ")]
    saved = [line.removeprefix("saved ") for line in lines if line.startswith("saved ")]
    return (
        len(games) == 1
        and len(games[0]) == 3
        and games[0][1].isdigit()
        and int(games[0][1]) > 1
        and games[0][2] == "/ark/ShooterGame/Binaries/Linux/ShooterGameServer"
        and saved == ["/ark/ShooterGame/Saved"]
        and len(lines) == 2
    )


class _OwnedSaveReadWatch:
    """Watch the exact owned save inode while only the second game boot can read it."""

    def __init__(self, save: Path) -> None:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        libc.inotify_init1.argtypes = [ctypes.c_int]
        libc.inotify_init1.restype = ctypes.c_int
        libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        libc.inotify_add_watch.restype = ctypes.c_int
        fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        if fd < 0:
            raise OSError(ctypes.get_errno(), "inotify_init1 failed")
        self.fd = fd
        mask = _IN_ACCESS | _IN_OPEN | _IN_CLOSE_NOWRITE | _IN_DELETE_SELF | _IN_MOVE_SELF
        self.wd = libc.inotify_add_watch(fd, os.fsencode(save), mask)
        if self.wd < 0:
            error = ctypes.get_errno()
            os.close(fd)
            raise OSError(error, f"cannot watch owned save {save}")

    def close(self) -> None:
        os.close(self.fd)

    def read_evidence(self) -> dict[str, Any]:
        masks: list[int] = []
        while True:
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                break
            offset = 0
            while offset < len(chunk):
                wd, mask, _, name_length = _INOTIFY_EVENT.unpack_from(chunk, offset)
                offset += _INOTIFY_EVENT.size + name_length
                if wd == self.wd or mask & _IN_Q_OVERFLOW:
                    masks.append(mask)
        invalid = any(mask & (_IN_DELETE_SELF | _IN_MOVE_SELF | _IN_Q_OVERFLOW) for mask in masks)
        stage = 0
        for mask in masks:
            if stage == 0 and mask & _IN_OPEN:
                stage = 1
            elif stage == 1 and mask & _IN_ACCESS:
                stage = 2
            elif stage == 2 and mask & _IN_CLOSE_NOWRITE:
                stage = 3
        return {"openedReadClosed": stage == 3 and not invalid, "eventMasks": masks, "invalidated": invalid}


async def _wait_native_protocol_ready(
    container: str, build: str, alive: Any, *, timeout: float = 60, poll_interval: float = 1
) -> dict[str, Any]:
    """Wait for zero-player protocol data; chat needs a real controller and is separate."""
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    url = f"http://127.0.0.1:{NATIVE_PORT}/health"
    while True:
        try:
            response = await asyncio.to_thread(_get_json, container, url, native=True)
            last = response if isinstance(response, dict) else {"probeError": f"unexpected health: {response!r}"}
        except (RuntimeError, ValueError) as exc:
            last = {"probeError": str(exc)}
        if _native_protocol_ready(last, build) or (last.get("build") is not None and last.get("build") != build):
            return last
        if time.monotonic() >= deadline or not alive():
            return last
        await asyncio.sleep(poll_interval)


async def after_protocol(run: Any, fake: Any, alive: Any) -> None:
    selected = [name for name in CHECK_IDS if name not in ("negative-wrong-target", "stop") and run.wanted(name)]
    sidecar: Container | None = None
    launch_error: str | None = None
    if selected:
        try:
            sidecar = await asyncio.to_thread(start_sidecar, run, fake)
        except Exception as exc:
            launch_error = str(exc)
    for check_id in CHECK_IDS:
        if check_id in ("event", "native-shutdown", "owned-save-reload", "stop", "negative-wrong-target"):
            continue
        if not run.wanted(check_id):
            run.skip(check_id, "not selected by --checks")
            continue
        started = time.monotonic()
        problems: list[str] = []
        detail: dict[str, Any] = {}
        try:
            if sidecar is None:
                raise RuntimeError(f"shipped sidecar did not start: {launch_error}")
            if check_id == "native-health":
                health = await _wait_native_protocol_ready(sidecar.name, str(run.target.record["revision"]), alive)
                detail["health"] = health
                if run.options.ark_readonly_base is not None:
                    if run.container is None:
                        raise RuntimeError("read-only ARK game container was not started")
                    paths = await asyncio.to_thread(
                        subprocess.run,
                        [
                            *docker_command(),
                            "exec",
                            run.container.name,
                            "bash",
                            "-lc",
                            _OWNED_PATH_PROBE_SCRIPT,
                        ],
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    detail["readOnlyPaths"] = paths.stdout.splitlines()
                    if paths.returncode or not _owned_runtime_paths_valid(paths.stdout.splitlines()):
                        problems.append("ARK process executable or Saved path escapes the owned runtime tree")
                if not _native_protocol_ready(health, str(run.target.record["revision"])):
                    problems.append("native health did not reach pinned build and zero-player protocol readiness")
                detail["clientDependentCapabilities"] = ["chat", "sendMessage"]
            elif check_id == "sidecar-identify":
                await fake.wait_for_identify(120)
                health = await asyncio.to_thread(_wait_json, sidecar.name, f"http://127.0.0.1:{SIDECAR_PORT}/health")
                detail["health"] = health
                if health.get("takaroIdentified") is not True:
                    problems.append("sidecar did not report identified")
            elif check_id == "ark-heartbeat":
                heartbeat = await checks.check_heartbeat(fake)
                detail.update({key: value for key, value in heartbeat.detail.items() if key != "problems"})
                problems.extend(heartbeat.detail.get("problems", []))
            elif check_id == "roster":
                result = await fake.request("getPlayers", {})
                detail["players"] = result
                if not isinstance(result, list):
                    problems.append("getPlayers did not return a list")
                reach = await fake.request("testReachability", {})
                detail["reachability"] = reach
                if not isinstance(reach, dict) or reach.get("connectable") is not True:
                    problems.append("testReachability did not confirm native reachability")
            elif check_id == "location":
                players = await fake.request("getPlayers", {})
                if not players:
                    problems.append("a real connected player is required to verify finite location")
                else:
                    player = players[0]
                    position = await fake.request("getPlayerLocation", {"gameId": player["gameId"]})
                    detail["position"] = position
                    if not isinstance(position, dict) or any(
                        isinstance(position.get(k), bool)
                        or not isinstance(position.get(k), (float, int))
                        or not math.isfinite(position[k])
                        for k in ("x", "y", "z")
                    ):
                        problems.append("location did not contain numeric x/y/z")
            elif check_id == "inventory":
                players = await fake.request("getPlayers", {})
                if not players:
                    problems.append("a real connected player is required to verify inventory")
                else:
                    inventory = await fake.request("getPlayerInventory", {"gameId": players[0]["gameId"]})
                    detail["count"] = len(inventory) if isinstance(inventory, list) else None
                    if not isinstance(inventory, list):
                        problems.append("inventory did not return a list")
            elif check_id in ("catalog", "entities"):
                method = "listItems" if check_id == "catalog" else "listEntities"
                items = await fake.request(method, {})
                detail["count"] = len(items) if isinstance(items, list) else None
                if (
                    not isinstance(items, list)
                    or not items
                    or any(
                        not isinstance(item, dict)
                        or not isinstance(item.get("code"), str)
                        or not item["code"].strip()
                        or not isinstance(item.get("name"), str)
                        or not item["name"].strip()
                        for item in items
                    )
                ):
                    problems.append(f"native {method} catalog is missing or malformed")
            elif check_id == "ark-empty-broadcast":
                native_players = await asyncio.to_thread(
                    _get_json, sidecar.name, f"http://127.0.0.1:{NATIVE_PORT}/players", native=True
                )
                takaro_players = await fake.request("getPlayers", {})
                detail["nativePlayersBefore"] = native_players
                detail["takaroPlayersBefore"] = takaro_players
                if native_players != [] or takaro_players != []:
                    problems.append("zero-recipient broadcast requires a verified empty native and Takaro roster")
                else:
                    marker = f"ARK isolated empty broadcast {run.options.run_id}"
                    native_result = await asyncio.to_thread(_native_message_probe, sidecar.name, "/message", marker)
                    detail["native"] = native_result
                    if native_result != {"status": 200, "body": {"success": True}}:
                        problems.append("native empty broadcast did not acknowledge game-thread completion")
                    response = await fake.request("sendMessage", {"message": marker})
                    detail["generic"] = response
                    if response != {}:
                        problems.append("Generic empty broadcast did not acknowledge the native no-op")
                    detail["nativePlayersAfter"] = await asyncio.to_thread(
                        _get_json, sidecar.name, f"http://127.0.0.1:{NATIVE_PORT}/players", native=True
                    )
                    if detail["nativePlayersAfter"] != []:
                        problems.append("empty broadcast changed the zero-player roster")
            elif check_id == "ark-targeted-offline":
                target = "76561198009999999"
                native_players = await asyncio.to_thread(
                    _get_json, sidecar.name, f"http://127.0.0.1:{NATIVE_PORT}/players", native=True
                )
                detail["nativePlayers"] = native_players
                if not isinstance(native_players, list) or any(
                    isinstance(player, dict) and player.get("gameId") == target for player in native_players
                ):
                    problems.append("offline target was not verified absent from native roster")
                else:
                    result = await asyncio.to_thread(
                        _native_message_probe, sidecar.name, f"/players/{target}/message", "offline target must fail"
                    )
                    detail["native"] = result
                    if result != {"status": 503, "body": {"error": "no native recipient or queue full"}}:
                        problems.append("native targeted message did not reject the offline recipient")
            elif check_id == "chat-out":
                marker = f"Takaro ARK verify {run.options.run_id}"
                detail["message"] = marker
                response = await fake.request("sendMessage", {"message": marker})
                detail["response"] = response
                if response != {}:
                    problems.append("sendMessage did not return the Generic success payload")
            elif check_id == "chat-in":
                await asyncio.sleep(1)
                events = [
                    event
                    for event in fake.events
                    if isinstance(event, dict)
                    and event.get("type") == "chat-message"
                    and isinstance(event.get("data"), dict)
                    and event["data"].get("player")
                    and event["data"].get("msg")
                ]
                detail["chatEvents"] = len(events)
                if not events:
                    problems.append("no real client chat event reached Takaro; external client input required")
            elif check_id == "ark-console":
                baseline = await asyncio.to_thread(
                    _get_json, sidecar.name, f"http://127.0.0.1:{NATIVE_PORT}/health", native=True
                )
                boot_id = baseline.get("bootId") if isinstance(baseline, dict) else None
                if not boot_id:
                    problems.append("native bootId was unavailable before console safety probes")
                safety: list[dict[str, Any]] = []
                for command, error_message in CONSOLE_SAFETY_PROBES:
                    rejection = await asyncio.to_thread(_native_console_probe, sidecar.name, command)
                    health_after = await asyncio.to_thread(
                        _get_json, sidecar.name, f"http://127.0.0.1:{NATIVE_PORT}/health", native=True
                    )
                    still_alive = bool(run.container and await asyncio.to_thread(run.container.alive))
                    row = {
                        "command": command,
                        "rejection": rejection,
                        "bootIdAfter": health_after.get("bootId") if isinstance(health_after, dict) else None,
                        "serverAlive": still_alive,
                    }
                    safety.append(row)
                    if rejection != {
                        "status": 400,
                        "body": {"success": False, "rawResult": "", "errorMessage": error_message},
                    }:
                        problems.append(f"native console did not structurally reject {command!r}")
                    if not still_alive or row["bootIdAfter"] != boot_id:
                        problems.append(f"native boot changed or stopped after console rejection {command!r}")
                detail["safetyProbes"] = safety
                detail["bootIdBefore"] = boot_id
                result = await fake.request(
                    "executeConsoleCommand", {"command": "GetAll ShooterPlayerState PlayerName"}
                )
                detail["result"] = result
                if (
                    not isinstance(result, dict)
                    or result.get("success") is not True
                    or not isinstance(result.get("rawResult"), str)
                    or result.get("errorMessage") is not None
                ):
                    problems.append("native GetAll player-name query was not reported handled with a valid result")
            elif check_id == "ark-saveworld":
                if run.container is None:
                    raise RuntimeError("game container was not started")
                save = run.data_dir / "ShooterGame/Saved/SavedArks/TheIsland.ark"
                before = save.stat() if save.is_file() else None
                event_baseline = len(fake.events)
                detail["eventIndexBefore"] = event_baseline
                health_before = await asyncio.to_thread(
                    _get_json, sidecar.name, f"http://127.0.0.1:{NATIVE_PORT}/health", native=True
                )
                result = await fake.request("executeConsoleCommand", {"command": "SaveWorld"})
                detail["result"] = result
                if result != {"success": True, "rawResult": "", "errorMessage": None}:
                    problems.append("Generic SaveWorld was not acknowledged as a completed native save")
                markers = await _fresh_saveworld_events(fake, event_baseline)
                detail["freshSaveMarkers"] = markers
                if not markers["start"] or not markers["complete"]:
                    problems.append("fresh native SaveWorld log events were not delivered to Generic Takaro")
                after = save.stat() if save.is_file() else None
                detail["ownedSave"] = {
                    "sizeBefore": before.st_size if before else None,
                    "mtimeNsBefore": before.st_mtime_ns if before else None,
                    "sizeAfter": after.st_size if after else None,
                    "mtimeNsAfter": after.st_mtime_ns if after else None,
                    "sha256After": await asyncio.to_thread(net.sha256_file, save) if after else None,
                }
                if not after or after.st_size <= 0 or (before and after.st_mtime_ns <= before.st_mtime_ns):
                    problems.append("owned world save was not freshly written by SaveWorld")
                health_after = await asyncio.to_thread(
                    _get_json, sidecar.name, f"http://127.0.0.1:{NATIVE_PORT}/health", native=True
                )
                detail["bootIdBefore"] = health_before.get("bootId") if isinstance(health_before, dict) else None
                detail["bootIdAfter"] = health_after.get("bootId") if isinstance(health_after, dict) else None
                if (
                    not await asyncio.to_thread(run.container.alive)
                    or not detail["bootIdBefore"]
                    or (detail["bootIdAfter"] != detail["bootIdBefore"])
                ):
                    problems.append("game stopped or native boot changed after SaveWorld")
            elif check_id == "reconnect":
                before = fake.identify_count
                await fake.disconnect(1001, "going away")
                await fake.wait_for_identify(90, minimum=before + 1)
                reach = await fake.request("testReachability", {})
                detail["identifyCount"] = fake.identify_count
                if not isinstance(reach, dict) or reach.get("connectable") is not True:
                    problems.append("reconnected sidecar cannot reach native")
        except Exception as exc:  # report a failed capability, never a fake pass
            problems.append(str(exc))
        run.record(_result(check_id, problems, started, **detail))


async def _reload_owned_save(run: Any, fake: Any, ws_url: str, ledger_inputs: list[dict[str, Any]]) -> None:
    started = time.monotonic()
    problems: list[str] = []
    detail: dict[str, Any] = {}
    try:
        first_shutdown = next((row for row in run.results if row.id == "native-shutdown"), None)
        first_health = next((row for row in run.results if row.id == "native-health"), None)
        if first_shutdown is None or first_shutdown.status != "pass":
            raise RuntimeError("owned save reload requires a passing native-shutdown check")
        if first_health is None or first_health.status != "pass":
            raise RuntimeError("owned save reload requires a passing first native-health check")
        first_boot_id = first_health.detail.get("health", {}).get("bootId")
        if not isinstance(first_boot_id, str) or not first_boot_id:
            raise RuntimeError("first native boot ID is missing")
        detail["firstBootId"] = first_boot_id
        save = run.data_dir / "ShooterGame/Saved/SavedArks/TheIsland.ark"
        if save.is_symlink() or not save.resolve().is_relative_to(run.data_dir.resolve()):
            raise RuntimeError("reload save is not contained in the owned runtime tree")
        before = await asyncio.to_thread(net.hash_file, save)
        if before["size"] <= 0:
            raise RuntimeError("owned save is empty before reload")
        detail["saveBeforeReload"] = {**before, "mtimeNs": save.stat().st_mtime_ns}

        # The old sidecar's network namespace must not supply a stale identify or health.
        if run.container is None:
            raise RuntimeError("first game container is missing")
        old_sidecar_name = f"{run.container.name}-sidecar"
        old_sidecar = next((item for item in run.containers if item.name == old_sidecar_name), None)
        if old_sidecar is None:
            raise RuntimeError("first sidecar container is missing")
        await asyncio.to_thread(old_sidecar.remove)
        identified_before = fake.identify_count

        # Hash before arming the watch; no runner code reads this file until the
        # second game has booted. OPEN+ACCESS+CLOSE_NOWRITE on this exact inode
        # therefore belongs to the isolated second server's world-load window.
        watch = _OwnedSaveReadWatch(save)
        try:
            run.extra_logs.append(run.out / "server-reload.log")
            second = await asyncio.to_thread(run.boot, ws_url, suffix="-reload", log_name="server-reload.log")
            startup = await asyncio.to_thread(
                checks.check_startup,
                run.out / "server-reload.log",
                run.startup_timeout,
                second.alive,
                run.options.ark_readonly_base or run.data_dir,
                ledger_inputs,
                run.ready_line(),
            )
            detail["reloadStartup"] = startup.detail
            if startup.status != "pass":
                raise RuntimeError("second game startup failed")
            second_sidecar = await asyncio.to_thread(start_sidecar, run, fake, suffix="-reload")
            await fake.wait_for_identify(120, minimum=identified_before + 1)
            health = await _wait_native_protocol_ready(
                second_sidecar.name, str(run.target.record["revision"]), second.alive
            )
            detail["reloadHealth"] = health
            if not _native_protocol_ready(health, str(run.target.record["revision"])):
                raise RuntimeError("second native boot did not reach pinned zero-player readiness")
            second_boot_id = health.get("bootId")
            if second_boot_id == first_boot_id:
                raise RuntimeError("second native boot reused the first boot ID")
            detail["secondBootId"] = second_boot_id
            detail["readEvidence"] = watch.read_evidence()
            if not detail["readEvidence"]["openedReadClosed"]:
                raise RuntimeError("second game boot did not open and read the exact owned save")
        finally:
            watch.close()

        after = await asyncio.to_thread(net.hash_file, save)
        detail["saveAfterReload"] = {**after, "mtimeNs": save.stat().st_mtime_ns}
        if after != before:
            raise RuntimeError("owned save changed during reload before a new shutdown save")

        reload_log = run.out / "server-reload.log"
        log_stat = reload_log.stat()
        ack = await fake.request("shutdown", {})
        detail["reloadShutdownAcknowledgement"] = ack
        if ack != {}:
            raise RuntimeError("second Generic shutdown was not acknowledged")
        exit_code = await asyncio.to_thread(second.wait_for_exit, 45)
        detail["reloadExitCode"] = exit_code
        markers = await asyncio.to_thread(
            _fresh_shutdown_markers, reload_log, log_stat.st_dev, log_stat.st_ino, log_stat.st_size
        )
        detail["reloadShutdownMarkers"] = markers
        if exit_code != 134 or not all(markers.values()):
            raise RuntimeError("second native shutdown lacked fresh save/exit-request markers or exit 134")
        saved_again = await asyncio.to_thread(net.hash_file, save)
        detail["saveAfterSecondShutdown"] = {**saved_again, "mtimeNs": save.stat().st_mtime_ns}
        if saved_again["size"] <= 0:
            raise RuntimeError("second shutdown left an empty owned save")
    except Exception as exc:
        problems.append(str(exc))
    run.record(_result("owned-save-reload", problems, started, **detail))


async def after_shutdown(run: Any, fake: Any, ws_url: str, ledger_inputs: list[dict[str, Any]]) -> None:
    if run.wanted("event"):
        events = [
            event
            for event in fake.events
            if isinstance(event, dict)
            and event.get("type") in ("chat-message", "player-connected", "player-disconnected")
        ]
        problems = [] if events else ["no real client chat/join/leave event reached Takaro"]
        run.record(
            checks.CheckResult("event", "fail" if problems else "pass", 0, {"count": len(events), "problems": problems})
        )
    else:
        run.skip("event", "not selected by --checks")
    if run.wanted("native-shutdown"):
        started = time.monotonic()
        shutdown_problems: list[str] = []
        shutdown_detail: dict[str, Any] = {}
        try:
            if run.container is None:
                raise RuntimeError("game container was not started")
            log_stat = run.server_log.stat()
            log_identity = (log_stat.st_dev, log_stat.st_ino, log_stat.st_size)
            ack = await fake.request("shutdown", {})
            shutdown_detail["acknowledgement"] = ack
            if ack != {}:
                shutdown_problems.append("Generic shutdown did not return the native acknowledgement payload")
            else:
                exit_code = await asyncio.to_thread(run.container.wait_for_exit, 45)
                shutdown_detail["exitCode"] = exit_code
                # This exact build requests exit after a flushed ACK. With the
                # ARK process behind Docker init, the engine aborts with 134.
                # Fresh markers distinguish that path from an unrelated abort.
                markers = await asyncio.to_thread(_fresh_shutdown_markers, run.server_log, *log_identity)
                for label, marker in markers.items():
                    shutdown_detail[f"{label}Marker"] = marker
                    if not marker:
                        shutdown_problems.append(f"native shutdown {label} completion marker is missing")
                if exit_code != 134:
                    shutdown_problems.append(f"native shutdown exit status {exit_code} != expected 134")
                if getattr(run, "data_dir", None) is not None:
                    save = run.data_dir / "ShooterGame/Saved/SavedArks/TheIsland.ark"
                    saved = save.stat() if save.is_file() else None
                    shutdown_detail["ownedSave"] = {
                        "path": "ShooterGame/Saved/SavedArks/TheIsland.ark",
                        "size": saved.st_size if saved else None,
                        "sha256": await asyncio.to_thread(net.sha256_file, save) if saved else None,
                        "mtimeNs": saved.st_mtime_ns if saved else None,
                    }
                    if not saved or saved.st_size <= 0:
                        shutdown_problems.append("native shutdown left no TheIsland.ark in the owned Saved tree")
        except Exception as exc:
            shutdown_problems.append(str(exc))
        run.record(_result("native-shutdown", shutdown_problems, started, **shutdown_detail))
    else:
        run.skip("native-shutdown", "not selected by --checks")
    if run.wanted("owned-save-reload"):
        await _reload_owned_save(run, fake, ws_url, ledger_inputs)
    else:
        run.skip("owned-save-reload", "not selected by --checks")
    if run.wanted("stop"):
        started = time.monotonic()
        stop_problems: list[str] = []
        if run.container is None:
            stop_problems.append("game container was not started")
        else:
            proc = await asyncio.to_thread(
                subprocess.run,
                [*docker_command(), "stop", "-t", "30", run.container.name],
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode:
                stop_problems.append(f"docker stop failed: {proc.stderr.strip()[:160]}")
        for entry in ledger_inputs:
            path = (run.options.ark_readonly_base or run.data_dir) / entry["path"]
            if not path.is_file() or net.hash_file(path).get("sha256") != entry.get("sha256"):
                stop_problems.append(f"pinned input changed or disappeared: {entry['path']}")
            if run.options.ark_readonly_base is not None:
                owned = run.data_dir / entry["path"]
                if (
                    owned.is_symlink()
                    or not owned.is_file()
                    or net.hash_file(owned).get("sha256") != entry.get("sha256")
                ):
                    stop_problems.append(f"owned pinned input changed or disappeared: {entry['path']}")
        run.record(_result("stop", stop_problems, started))
    else:
        run.skip("stop", "not selected by --checks")


async def _negative_impl(run: Any, fake: Any, ws_url: str, manifest: dict[str, Any]) -> None:
    del fake, ws_url, manifest
    if not run.wanted("negative-wrong-target"):
        run.skip("negative-wrong-target", "not selected by --checks")
        return
    started = time.monotonic()
    problems: list[str] = []
    evidence: dict[str, Any] = {}
    source = run.data_dir / "TakaroArk/TakaroArkNative"
    plugin, launcher = source / "libtakaro-ark-native.so", source / "launch.sh"
    if not plugin.is_file() or not launcher.is_file():
        problems.append("the installed native artifact is incomplete")
    else:
        evidence["artifactSha256"] = net.hash_file(plugin)["sha256"]
        expected_size = run.target.record["inputs"]["server"]["files"]["ShooterGame/Binaries/Linux/ShooterGameServer"][
            "size"
        ]
        with tempfile.TemporaryDirectory(prefix="takaro-ark-wrong-target-") as temporary:
            root = Path(temporary)
            binary = root / "server/ShooterGame/Binaries/Linux/ShooterGameServer"
            binary.parent.mkdir(parents=True)
            shutil.copy2("/bin/sleep", binary)
            with binary.open("r+b") as stream:
                stream.truncate(expected_size)
            fixture_plugin = root / "plugin"
            fixture_plugin.mkdir()
            shutil.copy2(plugin, fixture_plugin / plugin.name)
            fixture_launcher = fixture_plugin / "launch.sh"
            shutil.copy2(launcher, fixture_launcher)
            fixture_launcher.chmod(0o755)
            evidence["fixtureSha256"] = net.hash_file(binary)["sha256"]
            evidence["fixtureSize"] = expected_size
            launcher_proc = subprocess.run(
                [str(fixture_launcher), str(root / "server"), "TheIsland?listen", "-server"],
                env={**os.environ, "ARK_NATIVE_TOKEN": "isolated-fixture"},
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            launcher_output = launcher_proc.stdout + launcher_proc.stderr
            (run.out / "negative-wrong-target-launcher.log").write_text(launcher_output, encoding="utf-8")
            evidence["launcherExit"] = launcher_proc.returncode
            if launcher_proc.returncode != 7 or launcher_output != "Unknown ARK executable; native connector refused\n":
                problems.append("shipped launcher did not reject the wrong executable hash")

            # Direct preload tests the library's own constructor guard. The
            # executable has the expected byte size and basename but a wrong
            # hash; the isolated container cannot reach the live game port.
            script = (
                "set -eu\n"
                "ARK_NATIVE_TOKEN=isolated-fixture LD_PRELOAD=/fixture/plugin/libtakaro-ark-native.so "
                "/fixture/server/ShooterGame/Binaries/Linux/ShooterGameServer 2 &\n"
                "pid=$!\n"
                "for sample in 1 2 3 4 5; do\n"
                "  sleep 0.25\n"
                "  if (echo >/dev/tcp/127.0.0.1/18891) 2>/dev/null; "
                "then echo unexpected_native_listener; exit 1; fi\n"
                "done\n"
                'echo native_listener=absent\nwait "$pid"\necho fixture_exit=0\n'
            )
            image = run.resolved["containerRef"]
            direct = subprocess.run(
                [
                    *docker_command(),
                    "run",
                    "--rm",
                    "--pull",
                    "never",
                    "--network",
                    "none",
                    "--memory",
                    "512m",
                    "-v",
                    f"{root}:/fixture:ro",
                    image,
                    "bash",
                    "-c",
                    script,
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            direct_output = direct.stdout + direct.stderr
            (run.out / "negative-wrong-target-preload.log").write_text(direct_output, encoding="utf-8")
            evidence["preloadExit"] = direct.returncode
            evidence["network"] = "none"
            if direct.returncode or direct.stdout != "native_listener=absent\nfixture_exit=0\n":
                problems.append("native preload guard did not return safely without a listener")
    evidence["problems"] = problems
    run.record(
        checks.CheckResult(
            "negative-wrong-target",
            "pass" if not problems else "fail",
            int((time.monotonic() - started) * 1000),
            evidence,
        )
    )


async def negative(run: Any, fake: Any, ws_url: str, manifest: dict[str, Any]) -> None:
    if not run.wanted("negative-wrong-target"):
        run.skip("negative-wrong-target", "not selected by --checks")
        return
    try:
        await _negative_impl(run, fake, ws_url, manifest)
    except (OSError, subprocess.TimeoutExpired, KeyError, ValueError) as exc:
        run.record(
            checks.CheckResult(
                "negative-wrong-target",
                "fail",
                0,
                {
                    "problems": [f"isolated wrong-target fixture failed: {type(exc).__name__}: {exc}"],
                },
            )
        )


HOOKS = GameHooks(
    ready_line=READY_LINE,
    startup_timeout=900,
    check_ids=CHECK_IDS,
    negative_check_ids=("negative-wrong-target",),
    unsupported_checks=UNSUPPORTED_CHECKS,
    after_protocol=after_protocol,
    after_shutdown=after_shutdown,
    prepare_readonly_base=prepare_readonly_base,
    negative=negative,
)
