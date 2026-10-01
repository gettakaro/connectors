#!/usr/bin/env python3
"""Drain a 0.2.x Dragonwilds sidecar and hand its state to the native connector.

The legacy plugin has no atomic game/event admission fence. This tool therefore
requires an externally verified, expiring new-player admission fence and zero
online players. It records the final ring/cursor comparison, stops the sidecar,
checks for a ring delta, and stops the game only when the comparison is clean.
It reports the unprovable final read-to-stop interval; operators must not call
this an exact barrier or discard the archived ring data.

With --legacy-state-dir and --native-state-dir it then copies the sidecar's
event-cursor.json, online-players.json, known-players.json and timed-bans.json
into the native connector's state directory, after checking each file's shape.
--import-only does just that copy, for a game and sidecar that are already stopped.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

GAME = "dragonwilds"
SIDECAR = "dragonwilds-takaro"
PLUGIN_PORT = 18890
SIDECAR_HEALTH_PORT = 18891
LEGACY_FILES = ("event-cursor.json", "online-players.json", "known-players.json", "timed-bans.json")
# Files only the native connector writes. Their presence means the native connector has
# already run against this directory, and the legacy files would no longer be read as-is.
NATIVE_ONLY_FILES = ("event-outbox.json", "ban-intent.json")

# A plain HTTP/1.1 GET over bash's /dev/tcp: the game image is not assumed to carry curl.
# The plugin token expands inside the game container and is never copied to the host
# command line or evidence files.
HTTP_GET = r'''
exec 3<>"/dev/tcp/127.0.0.1/$1" || exit 7
if [ "$3" = auth ]; then
  printf 'GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer %s\r\nConnection: close\r\n\r\n' "$2" "$TAKARO_PLUGIN_TOKEN" >&3
else
  printf 'GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' "$2" >&3
fi
timeout 8 cat <&3
'''


def command(*args: str, timeout: int = 15) -> str:
    # Bytes, decoded by hand: text mode would turn the HTTP "\r\n" separators into "\n".
    result = subprocess.run(args, capture_output=True, timeout=timeout, check=False)
    stdout = result.stdout.decode("utf-8", errors="replace")
    if result.returncode:
        stderr = result.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(f"{' '.join(args[:3])} failed ({result.returncode}): {stderr[-300:]}")
    return stdout


def parse_http(raw: str) -> Any:
    """The JSON body of one HTTP/1.1 response, or an error naming the status."""
    head, separator, body = raw.partition("\r\n\r\n")
    if not separator:
        raise RuntimeError("HTTP response has no header terminator")
    lines = head.split("\r\n")
    parts = lines[0].split(" ", 2)
    if len(parts) < 2 or not parts[0].startswith("HTTP/1."):
        raise RuntimeError(f"not an HTTP response: {lines[0][:80]!r}")
    if parts[1] != "200":
        raise RuntimeError(f"HTTP {parts[1]}: {body[:200]!r}")
    headers = {key.strip().lower(): value.strip() for key, _, value in (line.partition(":") for line in lines[1:])}
    if headers.get("transfer-encoding", "").lower() == "chunked":
        decoded, rest = [], body
        while True:
            size_line, _, rest = rest.partition("\r\n")
            size = int(size_line.split(";")[0] or "0", 16)
            if size == 0:
                break
            decoded.append(rest[:size])
            rest = rest[size + 2:]
        body = "".join(decoded)
    return json.loads(body)


def game_json(port: int, path: str, *, authenticated: bool = False) -> Any:
    raw = command("docker", "exec", GAME, "bash", "-c", HTTP_GET, "bash", str(port), path,
                  "auth" if authenticated else "none")
    return parse_http(raw)


def check_fence(path: Path, checker: Path, game_port: int = 7777) -> dict[str, Any]:
    proof = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(proof, dict) or not proof.get("method") or not proof.get("restoreCommand"):
        raise RuntimeError("fence proof must record method and restoreCommand")
    if proof.get("gamePort") != game_port:
        raise RuntimeError(f"fence proof must identify Dragonwilds game port {game_port}")
    expiry = dt.datetime.fromisoformat(str(proof.get("expiresAtUtc", "")).replace("Z", "+00:00"))
    now = dt.datetime.now(dt.timezone.utc)
    if expiry <= now or expiry > now + dt.timedelta(minutes=30):
        raise RuntimeError("admission fence expiry must be in the next 30 minutes")
    if not checker.is_file() or not checker.stat().st_mode & 0o111:
        raise RuntimeError("fence checker must be an executable file")
    command(str(checker.resolve()), timeout=10)
    return proof


def snapshot() -> dict[str, Any]:
    health = game_json(SIDECAR_HEALTH_PORT, "/health")
    ring = game_json(PLUGIN_PORT, "/events?since=0", authenticated=True)
    plugin_players = game_json(PLUGIN_PORT, "/players", authenticated=True)
    if not isinstance(health, dict) or not isinstance(ring, dict):
        raise RuntimeError("legacy sidecar health or plugin ring response has an invalid shape")
    if not isinstance(plugin_players, list):
        raise RuntimeError("plugin players response is not an array")
    return {"utc": dt.datetime.now(dt.timezone.utc).isoformat(), "health": health,
            "ring": ring, "pluginPlayers": plugin_players}


def clean(value: dict[str, Any]) -> tuple[bool, list[str]]:
    health, ring = value["health"], value["ring"]
    reasons: list[str] = []
    if not health.get("takaroIdentified"):
        reasons.append("legacy sidecar is not identified")
    if health.get("pendingEvents") != 0:
        reasons.append("pendingEvents is not zero")
    cursor = health.get("eventCursor")
    scan = health.get("eventScanCursor")
    latest = ring.get("latestSeq")
    if not all(isinstance(item, int) and not isinstance(item, bool) and item >= 0 for item in (cursor, scan, latest)):
        reasons.append("cursor, scan cursor or latest ring sequence missing")
    elif cursor != scan or scan != latest:
        reasons.append(f"cursor/scan/ring differ: {cursor}/{scan}/{latest}")
    if not ring.get("bootId"):
        reasons.append("ring boot identity missing")
    # Dragonwilds has no built-in status API; the plugin's live player list is the oracle.
    if value.get("pluginPlayers") != []:
        reasons.append("plugin player list is not empty")
    return not reasons, reasons


def write(path: Path, value: dict[str, Any]) -> None:
    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()
    temporary = path.with_name(path.name + ".tmp")
    descriptor = os.open(temporary, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


# -- state import ---------------------------------------------------------------------------


def _players(value: Any, name: str) -> None:
    if not isinstance(value, list):
        raise RuntimeError(f"{name} is not a JSON array")
    for row in value:
        if not isinstance(row, dict) or not isinstance(row.get("gameId"), str) or not row["gameId"]:
            raise RuntimeError(f"{name} holds a row without a gameId")
        if not isinstance(row.get("name"), str):
            raise RuntimeError(f"{name} holds a row without a name")


def validate_legacy(name: str, value: Any) -> None:
    """The exact shapes the 0.2.x sidecar wrote. Anything else is refused, never guessed."""
    if name == "event-cursor.json":
        if not isinstance(value, dict):
            raise RuntimeError("event-cursor.json is not a JSON object")
        seq = value.get("seq")
        if not isinstance(seq, int) or isinstance(seq, bool) or seq < 0:
            raise RuntimeError("event-cursor.json has no non-negative integer seq")
        if "bootId" in value and (not isinstance(value["bootId"], str) or not value["bootId"]):
            raise RuntimeError("event-cursor.json has an invalid bootId")
    elif name in ("online-players.json", "known-players.json"):
        _players(value, name)
    elif name == "timed-bans.json":
        if not isinstance(value, list):
            raise RuntimeError("timed-bans.json is not a JSON array")
        for row in value:
            if not isinstance(row, dict) or not isinstance(row.get("gameId"), str) or not row["gameId"]:
                raise RuntimeError("timed-bans.json holds a ban without a gameId")
            expires = row.get("expiresAt")
            if not isinstance(expires, str):
                raise RuntimeError("timed-bans.json holds a ban without expiresAt")
            dt.datetime.fromisoformat(expires.replace("Z", "+00:00"))
            if "reason" in row and row["reason"] is not None and not isinstance(row["reason"], str):
                raise RuntimeError("timed-bans.json holds a ban with a non-text reason")
    else:
        raise RuntimeError(f"{name} is not a legacy state file")


def import_state(legacy: Path, native: Path) -> dict[str, Any]:
    """Copy the four legacy files into the native state directory, refusing any conflict.

    Nothing is written until every file has been read and checked, so a refusal leaves the
    native directory exactly as it was.
    """
    if legacy.resolve() == native.resolve():
        raise RuntimeError("legacy and native state directories are the same; the native connector reads them in place")
    if not legacy.is_dir():
        raise RuntimeError(f"legacy state directory {legacy} does not exist")
    native.mkdir(parents=True, exist_ok=True)
    for name in NATIVE_ONLY_FILES:
        if (native / name).exists():
            raise RuntimeError(f"{native / name} exists: the native connector already ran here; refusing to import")
    plan: list[tuple[str, bytes, int]] = []
    report: dict[str, Any] = {"from": str(legacy), "to": str(native), "files": {}}
    for name in LEGACY_FILES:
        source = legacy / name
        if not source.is_file():
            report["files"][name] = {"status": "absent"}
            continue
        data = source.read_bytes()
        try:
            value = json.loads(data)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"{source} is not valid JSON: {exc}") from exc
        validate_legacy(name, value)
        target = native / name
        digest = hashlib.sha256(data).hexdigest()
        if target.exists():
            if target.read_bytes() == data:
                report["files"][name] = {"status": "identical", "sha256": digest}
                continue
            raise RuntimeError(f"{target} exists with different content; refusing to overwrite it")
        plan.append((name, data, source.stat().st_mode & 0o777))
        report["files"][name] = {"status": "imported", "sha256": digest, "bytes": len(data)}
    for name, data, mode in plan:
        temporary = native / f".{name}.import"
        descriptor = os.open(temporary, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, mode or 0o644)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.chmod(temporary, mode or 0o644)
            os.replace(temporary, native / name)
        finally:
            temporary.unlink(missing_ok=True)
    directory = os.open(native, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return report


# -- main ----------------------------------------------------------------------------------


def main() -> int:
    global GAME, SIDECAR
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--fence-proof", type=Path)
    parser.add_argument("--fence-check", type=Path)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--interval", type=int, default=5)
    parser.add_argument("--snapshot-only", action="store_true")
    parser.add_argument("--import-only", action="store_true",
                        help="only copy legacy state; the game and the sidecar must already be stopped")
    parser.add_argument("--game-container", default=GAME)
    parser.add_argument("--sidecar-container", default=SIDECAR)
    parser.add_argument("--game-port", type=int, default=7777)
    parser.add_argument("--legacy-state-dir", type=Path, help="host path of the 0.2.x sidecar's state directory")
    parser.add_argument("--native-state-dir", type=Path, help="host path mounted as the native TAKARO_STATE_DIR")
    args = parser.parse_args()
    GAME, SIDECAR = args.game_container, args.sidecar_container
    importing = args.legacy_state_dir is not None or args.native_state_dir is not None
    if importing and (args.legacy_state_dir is None or args.native_state_dir is None):
        parser.error("--legacy-state-dir and --native-state-dir go together")
    if args.import_only and not importing:
        parser.error("--import-only needs --legacy-state-dir and --native-state-dir")
    if not args.import_only and (args.fence_proof is None or args.fence_check is None):
        parser.error("--fence-proof and --fence-check are required for a drain")
    if args.samples < 3 or args.interval < 5:
        parser.error("at least three samples spaced by at least five seconds are required")
    if not 1 <= args.game_port <= 65535:
        parser.error("game port must be between 1 and 65535")
    args.evidence_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    if args.evidence_dir.stat().st_mode & 0o077:
        parser.error("evidence directory must be private (chmod 700)")

    if args.import_only:
        report: dict[str, Any] = {"outcome": "incomplete"}
        try:
            report["import"] = import_state(args.legacy_state_dir, args.native_state_dir)
            report["outcome"] = "imported"
            return 0
        except Exception as exc:
            report["error"] = str(exc)
            return 1
        finally:
            write(args.evidence_dir / "import-report.json", report)
            summary = {key: report[key] for key in ("outcome", "error") if key in report}
            print(json.dumps(summary), file=sys.stderr if report.get("error") else sys.stdout)

    report = {"outcome": "incomplete", "exactBarrier": False, "samples": []}
    sidecar_stopped = False
    game_stopped = False
    try:
        report["fence"] = check_fence(args.fence_proof, args.fence_check, args.game_port)
        if command("docker", "inspect", "--format", "{{.State.Running}}", SIDECAR).strip() != "true":
            raise RuntimeError("legacy sidecar container is not running")
        game_identity = command("docker", "inspect", "--format", "{{.Id}} {{.State.Pid}} {{.State.StartedAt}}", GAME).strip()
        report["gameIdentity"] = game_identity
        for index in range(args.samples):
            check_fence(args.fence_proof, args.fence_check, args.game_port)
            current = snapshot()
            report["samples"].append(current)
            write(args.evidence_dir / f"sample-{index + 1}.json", current)
            okay, reasons = clean(current)
            if not okay:
                raise RuntimeError("legacy drain conditions failed: " + "; ".join(reasons))
            if index and (current["ring"]["bootId"], current["ring"]["latestSeq"]) != (
                report["samples"][0]["ring"]["bootId"], report["samples"][0]["ring"]["latestSeq"]
            ):
                raise RuntimeError("game ring changed during drain stability window")
            if index + 1 < args.samples:
                time.sleep(args.interval)
        if args.snapshot_only:
            report["outcome"] = "snapshot-only"
            return 0

        command("docker", "stop", SIDECAR, timeout=30)
        sidecar_stopped = True
        after = snapshot_without_sidecar()
        write(args.evidence_dir / "post-sidecar-ring.json", after)
        before_ring = report["samples"][-1]["ring"]
        if (after.get("bootId"), after.get("latestSeq")) != (before_ring.get("bootId"), before_ring.get("latestSeq")):
            report["undeliveredRingDelta"] = after
            raise RuntimeError("ring advanced after sidecar stopped; cutover aborted")
        check_fence(args.fence_proof, args.fence_check, args.game_port)
        command("docker", "stop", "--time", "120", GAME, timeout=140)
        game_stopped = True
        report["gameStoppedUtc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        command("docker", "rm", SIDECAR, timeout=30)
        report["sidecarRemovedUtc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        report["outcome"] = "quiescent-stopped"
        report["caveat"] = "No game-side atomic event barrier; final read-to-stop interval cannot be proven empty"
        if importing:
            report["import"] = import_state(args.legacy_state_dir, args.native_state_dir)
            report["outcome"] = "quiescent-stopped-imported"
        return 0
    except Exception as exc:
        report["error"] = str(exc)
        if sidecar_stopped and not game_stopped:
            try:
                command("docker", "start", SIDECAR, timeout=30)
                report["sidecarRestartedUtc"] = dt.datetime.now(dt.timezone.utc).isoformat()
            except Exception as restart_error:
                report["sidecarRestartError"] = str(restart_error)
        elif game_stopped:
            report["recovery"] = ("Game is stopped; do not start the old sidecar alone. Resolve this failure "
                                  "before starting either connector.")
        return 1
    finally:
        write(args.evidence_dir / "drain-report.json", report)
        summary = {key: report[key] for key in ("outcome", "error", "recovery") if key in report}
        print(json.dumps(summary), file=sys.stderr if report.get("error") else sys.stdout)


def snapshot_without_sidecar() -> dict[str, Any]:
    value = game_json(PLUGIN_PORT, "/events?since=0", authenticated=True)
    if not isinstance(value, dict):
        raise RuntimeError("post-sidecar ring response is not an object")
    return value


if __name__ == "__main__":
    sys.exit(main())
