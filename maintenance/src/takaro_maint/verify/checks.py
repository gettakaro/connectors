"""The base verification checks. Each one returns a row for the report."""

from __future__ import annotations

import asyncio
import json
import re
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .. import net
from ..commands.artifact import validate_file
from ..games.minecraft.fabric import TARGET_CHECK_PREFIX

DONE_LINE = re.compile(r'Done \(.*\)! For help, type "help"')
CATALOGUE_PREFIXES = ("minecraft:", "item.", "entity.", "block.")


@dataclass
class CheckResult:
    id: str
    status: str
    duration_ms: int
    detail: dict[str, Any] = field(default_factory=dict)
    log: dict[str, Any] | None = None

    def as_dict(self) -> dict[str, Any]:
        row: dict[str, Any] = {
            "id": self.id,
            "status": self.status,
            "durationMs": self.duration_ms,
            "detail": self.detail,
        }
        if self.log:
            row["log"] = self.log
        return row


class _Timer:
    def __enter__(self) -> _Timer:
        self.started = time.monotonic()
        return self

    def __exit__(self, *exc: object) -> None:
        self.elapsed_ms = int((time.monotonic() - self.started) * 1000)


def check_build(artifacts_dir: Path, manifest: dict[str, Any], target: Any) -> CheckResult:
    """The artifacts really are this target's, and the manifest agrees with the catalog."""
    with _Timer() as timer:
        problems: list[str] = []
        rows = [row for row in manifest["artifacts"] if row["target"] == target.id]
        if not rows:
            problems.append(f"build manifest has no artifact for target {target.id}")
        for row in rows:
            if row["fingerprint"] != target.fingerprint:
                problems.append(f"{row['file']}: manifest fingerprint != catalog fingerprint")
            problems += validate_file(artifacts_dir / row["file"], target.record, target.fingerprint)
    return CheckResult(
        "build",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"artifacts": [row["file"] for row in rows], "problems": problems},
    )


def find_line(log_file: Path, pattern: re.Pattern[str] | str) -> tuple[int, str] | None:
    """The first matching line of a log file, with its 1-based line number."""
    if not log_file.is_file():
        return None
    matcher = pattern if isinstance(pattern, re.Pattern) else re.compile(re.escape(pattern))
    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, start=1):
            if matcher.search(line):
                return number, line.rstrip("\n")
    return None


def wait_for_line(log_file: Path, pattern: re.Pattern[str], timeout: float, alive: Any) -> tuple[int, str] | None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = find_line(log_file, pattern)
        if found:
            return found
        if not alive():
            return find_line(log_file, pattern)
        time.sleep(2)
    return None


def check_startup(
    log_file: Path,
    timeout: float,
    alive: Any,
    data_dir: Path,
    ledger_inputs: list[dict[str, Any]],
    ready_line: re.Pattern[str] = DONE_LINE,
) -> CheckResult:
    """The server finished booting, and it did not replace the bytes we pinned.

    ``ready_line`` is the line that says this game's server is up; the default is the one
    a vanilla Minecraft server writes.
    """
    with _Timer() as timer:
        found = wait_for_line(log_file, ready_line, timeout, alive)
        intact: list[str] = []
        replaced: list[str] = []
        for entry in ledger_inputs:
            path = data_dir / entry["path"]
            if not path.is_file():
                replaced.append(f"{entry['path']}: missing after boot")
                continue
            digests = net.hash_file(path)
            if entry.get("sha256") and digests["sha256"] != entry["sha256"]:
                replaced.append(f"{entry['path']}: sha256 changed during boot")
            elif entry.get("sha1") and digests["sha1"] != entry["sha1"]:
                replaced.append(f"{entry['path']}: sha1 changed during boot")
            else:
                intact.append(entry["path"])
    detail: dict[str, Any] = {
        "inputsIntact": not replaced,
        "intact": intact,
        "replaced": replaced,
        "line": found[1] if found else None,
    }
    status = "pass" if found and not replaced else "fail"
    return CheckResult(
        "startup",
        status,
        timer.elapsed_ms,
        detail,
        {"file": log_file.name, "line": found[0]} if found else {"file": log_file.name},
    )


def check_connector_load(log_file: Path, target: Any, timeout: float, alive: Any) -> CheckResult:
    """The connector's own target-check line says it accepted this server."""
    pattern = re.compile(re.escape(TARGET_CHECK_PREFIX))
    with _Timer() as timer:
        found = wait_for_line(log_file, pattern, timeout, alive)
        problems: list[str] = []
        payload: dict[str, Any] = {}
        if not found:
            problems.append(f"no '{TARGET_CHECK_PREFIX}' line in {log_file.name}")
        else:
            index = found[1].find(TARGET_CHECK_PREFIX)
            try:
                payload = json.loads(found[1][index + len(TARGET_CHECK_PREFIX) :].strip())
            except json.JSONDecodeError as exc:
                problems.append(f"target-check line is not JSON: {exc}")
            if payload:
                runtime = payload.get("runtime", {})
                if payload.get("result") != "ok":
                    problems.append(f"result={payload.get('result')} reasons={payload.get('reasons')}")
                if runtime.get("gameVersion") != target.revision:
                    problems.append(f"runtime gameVersion {runtime.get('gameVersion')} != {target.revision}")
                declared_loader = target.record["inputs"]["loader"]["loaderVersion"]
                if runtime.get("loaderVersion") != declared_loader:
                    problems.append(f"runtime loaderVersion {runtime.get('loaderVersion')} != {declared_loader}")
                if runtime.get("java") != target.record["runtime"]["java"]:
                    problems.append(f"runtime java {runtime.get('java')} != {target.record['runtime']['java']}")
    return CheckResult(
        "connector-load",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"targetCheck": payload, "problems": problems},
        {"file": log_file.name, "line": found[0]} if found else {"file": log_file.name},
    )


async def check_identify(fake: Any, log_file: Path, timeout: float, alive: Any) -> CheckResult:
    with _Timer() as timer:
        problems: list[str] = []
        try:
            await asyncio.wait_for(fake.wait_for_identify(timeout), timeout + 5)
        except TimeoutError:
            problems.append("the connector never sent an identify frame")
        found = await asyncio.to_thread(wait_for_line, log_file, re.compile("Identified successfully"), 30, alive)
        if not found:
            problems.append("the connector never logged 'Identified successfully'")
    keys = sorted(fake.identified or {})
    return CheckResult(
        "identify",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"identifyPayloadKeys": keys, "problems": problems},
        {"file": log_file.name, "line": found[0]} if found else None,
    )


async def check_heartbeat(fake: Any) -> CheckResult:
    """Three protocol pings answered, plus the reachability action Takaro itself uses."""
    with _Timer() as timer:
        problems: list[str] = []
        round_trips: list[int] = []
        for _ in range(3):
            try:
                round_trips.append(int(await fake.ping(timeout=5) * 1000))
            except TimeoutError:
                problems.append("a websocket ping was not answered within 5 s")
                break
        reachable: Any = None
        try:
            reachable = await fake.request("testReachability", {})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"testReachability failed: {exc}")
        if not isinstance(reachable, dict) or reachable.get("connectable") is not True:
            problems.append(f"testReachability returned {reachable!r}, expected connectable true")
    return CheckResult(
        "heartbeat",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "pingRoundTripMs": round_trips,
            "testReachability": reachable,
            "note": "no application heartbeat; the protocol relies on websocket ping/pong",
            "problems": problems,
        },
    )


async def check_players(fake: Any) -> CheckResult:
    with _Timer() as timer:
        problems: list[str] = []
        players: Any = None
        try:
            players = await fake.request("getPlayers", {})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"getPlayers failed: {exc}")
        if not isinstance(players, list):
            problems.append(f"getPlayers returned {type(players).__name__}, expected a list")
        elif players:
            problems.append(f"an empty server reported {len(players)} players")
    return CheckResult(
        "players",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"players": players, "problems": problems},
    )


def _catalogue_problems(entries: Any, spot: tuple[str, str]) -> tuple[list[str], dict[str, Any]]:
    problems: list[str] = []
    detail: dict[str, Any] = {"count": 0, "spotCheck": {}}
    if not isinstance(entries, list) or not entries:
        return [f"expected a non-empty list, got {entries!r}"], detail
    detail["count"] = len(entries)
    for entry in entries:
        if not isinstance(entry, dict):
            problems.append(f"entry {entry!r} is not an object")
            continue
        code, name = entry.get("code"), entry.get("name")
        if not code or not name:
            problems.append(f"entry {entry!r} is missing code or name")
            continue
        if name == code:
            problems.append(f"{code}: name equals code — a display name is required")
        if any(str(name).startswith(prefix) or f" {prefix}" in str(name) for prefix in CATALOGUE_PREFIXES):
            problems.append(f"{code}: name '{name}' still looks like a translation key or registry id")
    code, expected = spot
    match = next((e for e in entries if isinstance(e, dict) and e.get("code") == code), None)
    detail["spotCheck"] = {"code": code, "expected": expected, "actual": match.get("name") if match else None}
    if match is None:
        problems.append(f"{code} is missing from the catalogue")
    elif match.get("name") != expected:
        problems.append(f"{code} is named {match.get('name')!r}, expected {expected!r}")
    return problems, detail


async def check_catalog(
    fake: Any,
    action: str,
    check_id: str,
    spot: tuple[str, str],
    extra: Callable[[list[Any]], list[str]] | None = None,
) -> CheckResult:
    """The shared catalogue check. ``extra`` adds one game's own rules over the same answer.

    A game whose codes go wrong in a way the shared rules cannot see -- a prefab short name
    that survived capitalisation, say -- passes a predicate rather than re-requesting the
    catalogue and re-implementing the rest of this.
    """
    with _Timer() as timer:
        try:
            entries = await fake.request(action, {}, timeout=120)
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            return CheckResult(check_id, "fail", 0, {"problems": [f"{action} failed: {exc}"]})
        problems, detail = _catalogue_problems(entries, spot)
        if extra is not None and isinstance(entries, list):
            problems = problems + extra(entries)
    detail["problems"] = problems
    return CheckResult(check_id, "pass" if not problems else "fail", timer.elapsed_ms, detail)


async def check_console(fake: Any, log_file: Path, marker: str, alive: Any) -> CheckResult:
    with _Timer() as timer:
        problems: list[str] = []
        result: Any = None
        try:
            result = await fake.request("executeConsoleCommand", {"command": f"say {marker}"})
        except Exception as exc:  # noqa: BLE001 - reported as a check failure
            problems.append(f"executeConsoleCommand failed: {exc}")
        if not isinstance(result, dict) or result.get("success") is not True:
            problems.append(f"executeConsoleCommand returned {result!r}, expected success true")
        found = await asyncio.to_thread(wait_for_line, log_file, re.compile(re.escape(marker)), 30, alive)
        if not found:
            problems.append(f"the server log never showed '{marker}'")
    return CheckResult(
        "console",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"command": f"say {marker}", "result": result, "problems": problems},
        {"file": log_file.name, "line": found[0]} if found else {"file": log_file.name},
    )


async def check_shutdown(fake: Any, wait_for_exit: Any) -> CheckResult:
    with _Timer() as timer:
        problems: list[str] = []
        note = "shutdown response received"
        try:
            await fake.request("shutdown", {}, timeout=30)
        except Exception as exc:  # noqa: BLE001 - the socket closing first is normal here
            note = f"the connection closed before the response arrived ({exc}); the exit code is the real gate"
        # The container is watched off the event loop: the connector's closing handshake
        # needs the fake Takaro to keep answering while the server shuts down.
        code = await asyncio.to_thread(wait_for_exit, 120)
        if code != 0:
            problems.append(f"the server container exited {code}, expected 0")
    return CheckResult(
        "shutdown",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"exitCode": code, "note": note, "problems": problems},
    )
