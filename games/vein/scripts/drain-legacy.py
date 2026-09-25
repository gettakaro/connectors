#!/usr/bin/env python3
"""Quiescent legacy VEIN sidecar drain before a native upgrade.

The legacy binary has no atomic game/event admission fence. This tool therefore
requires an externally verified, expiring new-player admission fence and zero
online players. It records the final ring/cursor comparison, stops the sidecar,
checks for a ring delta, and stops the game only when the comparison is clean.
It reports the unprovable final read-to-stop interval; operators must not call
this an exact barrier or discard the archived ring data.
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

GAME = "takaro-dev-vein"
SIDECAR = "takaro-dev-vein-sidecar"
LOG_KEYS = {"VEIN_LOG_LOGIN_RE", "VEIN_LOG_JOIN_RE", "VEIN_LOG_LEAVE_RE",
            "VEIN_LOG_CHAT_RE", "VEIN_LOG_READY_RE"}
PACKAGED_FIXTURES = Path(__file__).with_name("native_log_legacy_fixtures.json")
SOURCE_FIXTURES = Path(__file__).resolve().parents[1] / "mod/tests/native_log_legacy_fixtures.json"


def command(*args: str, timeout: int = 15) -> str:
    result = subprocess.run(args, text=True, capture_output=True, timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError(f"{' '.join(args[:3])} failed ({result.returncode}): {result.stderr[-300:]}")
    return result.stdout


def game_json(url: str, *, authenticated: bool = False) -> Any:
    if authenticated:
        # The token expands inside the game container and is never copied to
        # the host command line or evidence files.
        script = 'curl -fsS --max-time 8 -H "Authorization: Bearer $TAKARO_PLUGIN_TOKEN" "$1"'
        raw = command("docker", "exec", GAME, "bash", "-c", script, "bash", url)
    else:
        raw = command("docker", "exec", GAME, "curl", "-fsS", "--max-time", "8", url)
    return json.loads(raw)


def check_fence(path: Path, checker: Path, game_port: int = 7807) -> dict[str, Any]:
    proof = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(proof, dict) or not proof.get("method") or not proof.get("restoreCommand"):
        raise RuntimeError("fence proof must record method and restoreCommand")
    if proof.get("gamePort") != game_port:
        raise RuntimeError(f"fence proof must identify VEIN game port {game_port}")
    expiry = dt.datetime.fromisoformat(str(proof.get("expiresAtUtc", "")).replace("Z", "+00:00"))
    now = dt.datetime.now(dt.timezone.utc)
    if expiry <= now or expiry > now + dt.timedelta(minutes=30):
        raise RuntimeError("admission fence expiry must be in the next 30 minutes")
    if not checker.is_file() or not checker.stat().st_mode & 0o111:
        raise RuntimeError("fence checker must be an executable file")
    command(str(checker.resolve()), timeout=10)
    return proof


def check_grammar_report(report_path: Path, fixtures_path: Path) -> dict[str, Any]:
    """Bind a one-off legacy-JS/PCRE2 comparison to live sidecar settings."""
    raw = command("docker", "inspect", "--format", "{{json .Config.Env}}", SIDECAR)
    values = json.loads(raw)
    if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
        raise RuntimeError("cannot read legacy sidecar environment for grammar check")
    overrides = {key: value for item in values for key, separator, value in [item.partition("=")]
                 if separator and key in LOG_KEYS and value.strip()}
    canonical = json.dumps(overrides, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    config_hash = hashlib.sha256(canonical).hexdigest()
    fixture_hash = hashlib.sha256(fixtures_path.read_bytes()).hexdigest()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if not isinstance(report, dict) or report.get("version") != 1:
        raise RuntimeError("grammar report has an unsupported schema")
    if report.get("configSha256") != config_hash or report.get("fixtureSha256") != fixture_hash:
        raise RuntimeError("grammar report does not match live overrides or bundled fixtures")
    keys = sorted(overrides)
    if report.get("configuredKeys") != keys:
        raise RuntimeError("grammar report configured keys differ from live sidecar")
    expected_status = "pass" if keys else "none"
    if report.get("status") != expected_status:
        raise RuntimeError(f"grammar report must have status {expected_status}")
    if keys and any((report.get("keys") or {}).get(key, {}).get("status") != "pass" for key in keys):
        raise RuntimeError("grammar report has a failing configured expression")
    generated = dt.datetime.fromisoformat(str(report.get("generatedAtUtc", "")).replace("Z", "+00:00"))
    age = dt.datetime.now(dt.timezone.utc) - generated
    if age < dt.timedelta(seconds=-60) or age > dt.timedelta(minutes=30):
        raise RuntimeError("grammar report must have been generated within 30 minutes")
    return {"status": expected_status, "configuredKeys": keys, "configSha256": config_hash,
            "fixtureSha256": fixture_hash, "generatedAtUtc": report["generatedAtUtc"]}


def snapshot() -> dict[str, Any]:
    health = game_json("http://127.0.0.1:18891/health")
    ring = game_json("http://127.0.0.1:18890/events?since=0", authenticated=True)
    game_status = game_json("http://127.0.0.1:8080/status")
    game_players = game_json("http://127.0.0.1:8080/players")
    plugin_players = game_json("http://127.0.0.1:18890/players", authenticated=True)
    if not all(isinstance(value, dict) for value in (health, ring, game_status, game_players)):
        raise RuntimeError("legacy health, ring, game status or game players response has an invalid shape")
    if not isinstance(game_players.get("players"), list):
        raise RuntimeError("legacy game players response has no players array")
    if not isinstance(plugin_players, list):
        raise RuntimeError("plugin players response is not an array")
    return {"utc": dt.datetime.now(dt.timezone.utc).isoformat(), "health": health,
            "ring": ring, "gameStatus": game_status,
            "gamePlayers": game_players, "pluginPlayers": plugin_players}


def clean(value: dict[str, Any]) -> tuple[bool, list[str]]:
    health, ring = value["health"], value["ring"]
    reasons: list[str] = []
    if not health.get("takaroIdentified"):
        reasons.append("legacy sidecar is not identified")
    for key in ("pendingEvents", "unconfirmedEvents"):
        if health.get(key) != 0:
            reasons.append(f"{key} is not zero")
    cursor = health.get("eventCursor")
    scan = health.get("eventScanCursor")
    latest = ring.get("latestSeq")
    if not all(isinstance(item, int) and item >= 0 for item in (cursor, scan, latest)):
        reasons.append("cursor, scan cursor or latest ring sequence missing")
    elif cursor != scan or scan != latest:
        reasons.append(f"cursor/scan/ring differ: {cursor}/{scan}/{latest}")
    if not ring.get("bootId"):
        reasons.append("ring boot identity missing")
    # VEIN's /players can retain offline/known names as strings after a
    # client leaves. /status.onlinePlayers is the game's live-session oracle;
    # archive both raw responses, but only the live set gates admission.
    online = value["gameStatus"].get("onlinePlayers")
    if not isinstance(online, dict) or online:
        reasons.append("game status online player set is not empty")
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


def main() -> int:
    global GAME, SIDECAR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--fence-proof", type=Path, required=True)
    parser.add_argument("--fence-check", type=Path, required=True)
    parser.add_argument("--grammar-report", type=Path, required=True)
    parser.add_argument("--grammar-fixtures", type=Path,
                        default=PACKAGED_FIXTURES if PACKAGED_FIXTURES.exists() else SOURCE_FIXTURES)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--interval", type=int, default=5)
    parser.add_argument("--snapshot-only", action="store_true")
    parser.add_argument("--game-container", default=GAME)
    parser.add_argument("--sidecar-container", default=SIDECAR)
    parser.add_argument("--game-port", type=int, default=7807)
    args = parser.parse_args()
    GAME, SIDECAR = args.game_container, args.sidecar_container
    if args.samples < 3 or args.interval < 5:
        parser.error("at least three samples spaced by at least five seconds are required")
    if not 1 <= args.game_port <= 65535:
        parser.error("game port must be between 1 and 65535")
    args.evidence_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    if args.evidence_dir.stat().st_mode & 0o077:
        parser.error("evidence directory must be private (chmod 700)")
    report: dict[str, Any] = {"outcome": "incomplete", "exactBarrier": False, "samples": []}
    sidecar_stopped = False
    game_stopped = False
    try:
        report["fence"] = check_fence(args.fence_proof, args.fence_check, args.game_port)
        if command("docker", "inspect", "--format", "{{.State.Running}}", SIDECAR).strip() != "true":
            raise RuntimeError("legacy sidecar container is not running")
        report["grammar"] = check_grammar_report(args.grammar_report, args.grammar_fixtures)
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
            report["recovery"] = "Game is stopped; do not start the old sidecar alone. Resolve this failure before starting either connector."
        return 1
    finally:
        write(args.evidence_dir / "drain-report.json", report)


def snapshot_without_sidecar() -> dict[str, Any]:
    value = game_json("http://127.0.0.1:18890/events?since=0", authenticated=True)
    if not isinstance(value, dict):
        raise RuntimeError("post-sidecar ring response is not an object")
    return value


if __name__ == "__main__":
    sys.exit(main())
