"""Contract checks for the native runtime smoke gate."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("native_smoke", Path(__file__).with_name("smoke-server.py"))
assert SPEC and SPEC.loader
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


def healthy():
    return {
        "health": {
            "status": "ok", "capabilities": {"players": "ok"},
            "native": {
                "connection": {"state": "connected", "identified": True, "epoch": 1},
                "queues": {"inbound": 0, "actions": 0, "outbound": 0, "outbox": 0},
                "deliveryLosses": 0, "persistenceErrors": 0, "overloads": 0,
                "gate": {"experimental": True, "durableOutbox": False},
            },
        },
        "players": 0, "gamePlayers": 0, "items": 42, "gameStatus": {"onlinePlayers": 0},
    }


class NativeSmokeTest(unittest.TestCase):
    def test_probe_uses_game_container_curl_and_no_sidecar_or_node(self):
        self.assertIn("TAKARO_PLUGIN_TOKEN", smoke.PROBE)
        self.assertIn("curl", smoke.PROBE)
        self.assertNotIn("sidecar", smoke.PROBE)

    def test_probe_parses_game_players_object(self):
        rows = [healthy()["health"], [], [{"code": "example"}],
                {"onlinePlayers": 0}, {"players": []}]
        parsed = smoke.parse_probe("\n".join(json.dumps(row) for row in rows) + "\n")
        self.assertEqual(parsed["items"], 1)
        self.assertEqual(parsed["gamePlayers"], 0)

    def test_transport_gate_and_final_gate_are_distinct(self):
        self.assertEqual(smoke.problems(healthy()), [])
        self.assertIn("durable event outbox is not active", smoke.problems(healthy(), require_durable=True))

    def test_identify_queue_and_client_failures(self):
        probe = healthy()
        probe["health"]["native"]["connection"]["identified"] = False
        probe["health"]["native"]["queues"]["actions"] = None
        errors = smoke.problems(probe, require_player=True)
        self.assertTrue(any("identified" in error for error in errors))
        self.assertTrue(any("actions queue" in error for error in errors))
        self.assertTrue(any("client" in error for error in errors))

    def test_manifest_build_parse(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "appmanifest.acf"
            manifest.write_text('"AppState" { "buildid" "25035268" }')
            self.assertEqual(smoke.installed_build(manifest), "25035268")
            manifest.write_text('"AppState" {}')
            with self.assertRaisesRegex(ValueError, "no Steam buildid"):
                smoke.installed_build(manifest)


if __name__ == "__main__":
    unittest.main()
