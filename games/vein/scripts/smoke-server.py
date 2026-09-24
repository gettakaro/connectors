#!/usr/bin/env python3
"""Read-only runtime smoke for an already-started, isolated VEIN dev rig.

Runs on the Docker host. It checks the installed Steam build and probes the sidecar
from inside its container, where the plugin's private loopback API is reachable.
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

MANIFEST = Path(__file__).resolve().parents[3] / "dev-servers/_data/vein-dev/steamapps/appmanifest_2131400.acf"
SIDECAR = "takaro-dev-vein-sidecar"
# No token leaves the sidecar: Node reads it from its own environment and prints counts only.
PROBE = r"""
const health = await fetch('http://127.0.0.1:18891/health', {signal: AbortSignal.timeout(5000)});
if (!health.ok) throw Error(`sidecar health HTTP ${health.status}`);
const snapshot = await health.json();
const token = process.env.TAKARO_PLUGIN_TOKEN;
if (!token) throw Error('sidecar has no plugin token');
async function count(path) {
  const response = await fetch(`http://127.0.0.1:18890${path}`, {
    headers: {Authorization: `Bearer ${token}`}, signal: AbortSignal.timeout(5000)
  });
  if (!response.ok) throw Error(`plugin ${path} HTTP ${response.status}`);
  const rows = await response.json();
  if (!Array.isArray(rows)) throw Error(`plugin ${path} did not return an array`);
  return rows.length;
}
const players = await count('/players');
const items = await count('/items');
process.stdout.write(JSON.stringify({health: snapshot, players, items}));
"""


def installed_build(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    match = re.search(r'"buildid"\s+"([0-9]+)"', text, re.IGNORECASE)
    if not match:
        raise ValueError(f"{path} has no Steam buildid")
    return match.group(1)


def problems(probe: dict[str, Any], *, require_player: bool = False) -> list[str]:
    health = probe.get("health") or {}
    plugin = health.get("pluginHealth") or {}
    failures = []
    if health.get("ok") is not True:
        failures.append("sidecar health is not ok")
    if health.get("takaroIdentified") is not True:
        failures.append("sidecar has not identified with Takaro")
    if health.get("serverReady") is not True:
        failures.append("game server is not ready")
    if (health.get("gameHttpApi") or {}).get("reachable") is not True:
        failures.append("game HTTP API is unreachable")
    if plugin.get("status") != "ok":
        failures.append("plugin self-checks are not ok")
    degraded = [name for name, state in (plugin.get("capabilities") or {}).items() if state == "degraded"]
    if degraded:
        failures.append("degraded plugin capabilities: " + ", ".join(sorted(degraded)))
    if not isinstance(probe.get("players"), int):
        failures.append("player lookup did not succeed")
    elif require_player and probe["players"] < 1:
        failures.append("no player is visible to the plugin")
    game_players = (health.get("gameHttpApi") or {}).get("players")
    if require_player and (not isinstance(game_players, int) or game_players < 1):
        failures.append("no player is visible to the game HTTP API")
    if not isinstance(probe.get("items"), int) or probe["items"] == 0:
        failures.append("item catalog is empty or unavailable")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected-build", required=True, help="Steam build id from the maintenance issue")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--sidecar", default=SIDECAR)
    parser.add_argument("--timeout", type=int, default=420)
    parser.add_argument("--require-player", action="store_true", help="require a real client in-world")
    parser.add_argument("--out", type=Path, help="write the final JSON report")
    args = parser.parse_args()
    start = time.monotonic()
    report: dict[str, Any] = {"game": "vein", "expectedBuild": args.expected_build, "outcome": "fail"}
    try:
        actual = installed_build(args.manifest)
        report["installedBuild"] = actual
        if actual != args.expected_build:
            raise ValueError(f"installed Steam build {actual} differs from expected {args.expected_build}")
        while True:
            run = subprocess.run(
                ["docker", "exec", args.sidecar, "node", "--input-type=module", "-e", PROBE],
                capture_output=True, text=True, check=False, timeout=20,
            )
            if run.returncode == 0:
                snapshot = json.loads(run.stdout)
                failures = problems(snapshot, require_player=args.require_player)
                if not failures:
                    report.update(outcome="pass", players=snapshot["players"], items=snapshot["items"],
                                  pluginStatus="ok", takaroIdentified=True)
                    break
                report["errors"] = failures
            else:
                report["errors"] = [f"sidecar probe failed (exit {run.returncode}): {run.stderr.strip()[-300:]}"]
            if time.monotonic() - start >= args.timeout:
                break
            time.sleep(min(10, max(0, args.timeout - (time.monotonic() - start))))
    except (OSError, ValueError, json.JSONDecodeError, subprocess.TimeoutExpired) as exc:
        report["errors"] = [str(exc)]
    report["elapsedSeconds"] = round(time.monotonic() - start, 1)
    rendered = json.dumps(report, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered, encoding="utf-8")
    sys.stdout.write(rendered)
    return 0 if report["outcome"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
