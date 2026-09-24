"""Contract checks for the real-server smoke gate."""

import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SPEC = importlib.util.spec_from_file_location("vein_smoke", Path(__file__).with_name("smoke-server.py"))
assert SPEC and SPEC.loader
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


def healthy_probe():
    return {
        "health": {
            "ok": True, "takaroIdentified": True, "serverReady": True,
            "gameHttpApi": {"reachable": True, "players": 0},
            "pluginHealth": {"status": "ok", "capabilities": {"players": "ok", "items": "ok"}},
        },
        "players": 0, "items": 42,
    }


class SmokeServerTest(unittest.TestCase):
    def test_healthy_exact_build_passes_and_writes_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "appmanifest_2131400.acf"
            report = Path(tmp) / "report.json"
            manifest.write_text('"AppState" { "buildid" "25035268" }')
            probe = subprocess.CompletedProcess([], 0, json.dumps(healthy_probe()), "")
            with mock.patch.object(sys, "argv", ["smoke-server.py", "--expected-build", "25035268",
                                                 "--manifest", str(manifest), "--out", str(report)]), \
                 mock.patch.object(smoke.subprocess, "run", return_value=probe), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(smoke.main(), 0)
            self.assertEqual(json.loads(report.read_text())["outcome"], "pass")

    def test_wrong_build_fails_before_touching_server(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "appmanifest_2131400.acf"
            manifest.write_text('"AppState" { "buildid" "25023439" }')
            with mock.patch.object(sys, "argv", ["smoke-server.py", "--expected-build", "25035268",
                                                 "--manifest", str(manifest), "--timeout", "0"]), \
                 mock.patch.object(smoke.subprocess, "run") as docker, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(smoke.main(), 1)
                docker.assert_not_called()

    def test_degraded_plugin_and_unidentified_sidecar_fail(self):
        probe = healthy_probe()
        probe["health"]["takaroIdentified"] = False
        probe["health"]["pluginHealth"]["capabilities"]["players"] = "degraded"
        errors = smoke.problems(probe)
        self.assertTrue(any("identified" in error for error in errors))
        self.assertTrue(any("degraded plugin capabilities: players" in error for error in errors))

    def test_player_gate_requires_a_client_visible_to_both_game_and_plugin(self):
        probe = healthy_probe()
        self.assertTrue(smoke.problems(probe, require_player=True))
        probe["players"] = 1
        self.assertTrue(smoke.problems(probe, require_player=True))
        probe["health"]["gameHttpApi"]["players"] = 1
        self.assertEqual(smoke.problems(probe, require_player=True), [])


if __name__ == "__main__":
    unittest.main()
