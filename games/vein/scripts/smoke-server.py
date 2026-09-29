#!/usr/bin/env python3
"""Read-only smoke gate for an already-started native VEIN connector.

Run on the Docker host. The curl probe runs inside the game container, so its
loopback-only diagnostic endpoint stays private and the token is never printed.
This is a precheck; real-client and Takaro MCP evidence is required separately.
Release mode requires the durable outbox. --allow-experimental is only for the
first live transport gate and cannot qualify a release.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

DEFAULT_MANIFEST = Path("data/vein/steamapps/appmanifest_2131400.acf")
GAME = "vein"
PROBE = r'''
set -eu
test -n "${TAKARO_PLUGIN_TOKEN:-}" || { echo "diagnostic token missing" >&2; exit 1; }
for path in health players items; do
  curl -fsS --max-time 5 -H "Authorization: Bearer $TAKARO_PLUGIN_TOKEN" "http://127.0.0.1:18890/$path"
  printf '\n'
done
for path in status players; do
  curl -fsS --max-time 5 "http://127.0.0.1:8080/$path"
  printf '\n'
done
'''


def parse_probe(output: str) -> dict[str, Any]:
    lines = output.splitlines()
    if len(lines) != 5:
        raise ValueError(f"probe returned {len(lines)} JSON lines, expected five")
    health, players, items, game_status, game_players = map(json.loads, lines)
    if not isinstance(players, list) or not isinstance(items, list):
        raise ValueError("plugin players/items must be arrays")
    if not isinstance(game_players, dict) or not isinstance(game_players.get("players"), list):
        raise ValueError("game players response must contain a players array")
    return {"health": health, "players": len(players), "items": len(items),
            "gameStatus": game_status, "gamePlayers": len(game_players["players"])}


def installed_build(path: Path) -> str:
    match = re.search(r'"buildid"\s+"([0-9]+)"', path.read_text(encoding="utf-8"), re.I)
    if not match:
        raise ValueError(f"{path} has no Steam buildid")
    return match.group(1)


def problems(probe: dict[str, Any], require_player: bool = False, require_durable: bool = False) -> list[str]:
    health = probe.get("health") or {}
    native = health.get("native") or {}
    connection = native.get("connection") or {}
    queues = native.get("queues") or {}
    gate = native.get("gate") or {}
    failures: list[str] = []
    if health.get("status") != "ok":
        failures.append("plugin self-checks are not ok")
    if connection.get("identified") is not True or connection.get("state") != "connected":
        failures.append("native connection has not identified with Takaro")
    if not isinstance(connection.get("epoch"), int) or connection["epoch"] < 1:
        failures.append("native connection epoch missing")
    for name in ("inbound", "actions", "outbound", "outbox"):
        if not isinstance(queues.get(name), int) or queues[name] < 0:
            failures.append(f"native {name} queue count missing")
    for name in ("deliveryLosses", "persistenceErrors", "overloads"):
        if not isinstance(native.get(name), int) or native[name] < 0:
            failures.append(f"native {name} counter missing")
    if require_durable and gate.get("durableOutbox") is not True:
        failures.append("durable event outbox is not active")
    degraded = [name for name, state in (health.get("capabilities") or {}).items() if state == "degraded"]
    if degraded:
        failures.append("degraded plugin capabilities: " + ", ".join(sorted(degraded)))
    if not isinstance(probe.get("players"), int):
        failures.append("plugin player lookup failed")
    elif require_player and probe["players"] < 1:
        failures.append("no client visible to plugin")
    if not isinstance(probe.get("gamePlayers"), int):
        failures.append("game player lookup failed")
    elif require_player and probe["gamePlayers"] < 1:
        failures.append("no client visible to game API")
    if not isinstance(probe.get("items"), int) or probe["items"] < 1:
        failures.append("item catalogue empty or unavailable")
    if not isinstance(probe.get("gameStatus"), dict):
        failures.append("game HTTP status unavailable")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected-build", required=True)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--game", default=GAME)
    parser.add_argument("--timeout", type=int, default=420)
    parser.add_argument("--require-player", action="store_true")
    parser.add_argument("--allow-experimental", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    start = time.monotonic()
    report: dict[str, Any] = {"game": "vein", "expectedBuild": args.expected_build, "outcome": "fail"}
    attempts = 0
    transient: list[str] = []
    try:
        actual = installed_build(args.manifest)
        report["installedBuild"] = actual
        if actual != args.expected_build:
            raise ValueError(f"installed Steam build {actual} differs from expected {args.expected_build}")
        while True:
            attempts += 1
            run = subprocess.run(
                ["docker", "exec", args.game, "bash", "-c", PROBE],
                capture_output=True, text=True, check=False, timeout=25,
            )
            if run.returncode == 0:
                snapshot = parse_probe(run.stdout)
                failures = problems(snapshot, args.require_player, not args.allow_experimental)
                if not failures:
                    native = snapshot["health"]["native"]
                    report.update(outcome="pass", players=snapshot["players"], items=snapshot["items"],
                                  gamePlayers=snapshot["gamePlayers"], connection=native["connection"],
                                  queues=native["queues"], deliveryLosses=native["deliveryLosses"],
                                  persistenceErrors=native["persistenceErrors"],
                                  overloads=native["overloads"])
                    report.pop("errors", None)
                    break
                report["errors"] = failures
            else:
                report["errors"] = [f"game probe failed (exit {run.returncode}): {run.stderr.strip()[-300:]}"]
            transient.extend(report["errors"])
            if time.monotonic() - start >= args.timeout:
                break
            time.sleep(min(10, max(0, args.timeout - (time.monotonic() - start))))
    except (OSError, ValueError, json.JSONDecodeError, subprocess.TimeoutExpired) as exc:
        report["errors"] = [str(exc)]
    report["attempts"] = attempts
    if transient:
        report["startupRetries"] = transient
    report["elapsedSeconds"] = round(time.monotonic() - start, 1)
    rendered = json.dumps(report, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered, encoding="utf-8")
    sys.stdout.write(rendered)
    return 0 if report["outcome"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
