"""Failure-path checks for the legacy handoff tool; never starts Docker."""

import importlib.util
import datetime as dt
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SPEC = importlib.util.spec_from_file_location("legacy_drain", Path(__file__).with_name("drain-legacy.py"))
assert SPEC and SPEC.loader
drain = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(drain)


def sample():
    return {
        "health": {"takaroIdentified": True, "pendingEvents": 0,
                   "unconfirmedEvents": 0, "eventCursor": 55, "eventScanCursor": 55},
        "ring": {"bootId": "same-boot", "latestSeq": 55, "events": []},
        "gameStatus": {"onlinePlayers": {}},
        "gamePlayers": {"players": []}, "pluginPlayers": [],
    }


class DrainLegacyTest(unittest.TestCase):
    def test_clean_requires_same_cursor_and_empty_players(self):
        self.assertEqual(drain.clean(sample()), (True, []))
        changed = sample()
        changed["ring"]["latestSeq"] = 56
        changed["gameStatus"]["onlinePlayers"] = {"76561198000000001": "FixturePlayer"}
        self.assertEqual(drain.clean(changed)[0], False)

    def test_cached_offline_game_player_does_not_block_clean_drain(self):
        cached = sample()
        cached["gamePlayers"]["players"] = ["FixturePlayer"]
        self.assertEqual(drain.clean(cached), (True, []))
        cached["pluginPlayers"] = [{"gameId": "76561198000000001"}]
        self.assertEqual(drain.clean(cached)[0], False)

    def test_missing_live_player_field_rejects_drain(self):
        missing = sample()
        missing["gameStatus"] = {"uptime": 10}
        self.assertEqual(drain.clean(missing)[0], False)

    def run_main(self, post, fail_on=None):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "evidence"
            argv = ["drain-legacy.py", "--evidence-dir", str(evidence),
                    "--fence-proof", str(Path(directory) / "fence.json"),
                    "--fence-check", str(Path(directory) / "check"),
                    "--grammar-report", str(Path(directory) / "grammar.json")]
            calls = []

            def command(*args, **_kwargs):
                calls.append(args)
                if args == fail_on:
                    raise RuntimeError("injected Docker failure")
                if args[:3] == ("docker", "inspect", "--format"):
                    return "true" if args[-1] == drain.SIDECAR else "container-id 123 started"
                return ""

            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(drain, "check_fence", return_value={"method": "fixture"}), \
                 mock.patch.object(drain, "check_grammar_report", return_value={"status": "none"}), \
                 mock.patch.object(drain, "snapshot", side_effect=lambda: sample()), \
                 mock.patch.object(drain, "snapshot_without_sidecar", side_effect=post), \
                 mock.patch.object(drain, "command", side_effect=command), \
                 mock.patch.object(drain.time, "sleep"):
                result = drain.main()
            report = json.loads((evidence / "drain-report.json").read_text())
            self.assertEqual((evidence / "drain-report.json").stat().st_mode & 0o077, 0)
            return result, report, calls

    def test_ring_delta_aborts_and_restarts_sidecar(self):
        changed = {"bootId": "same-boot", "latestSeq": 56, "events": [{"seq": 56}]}
        result, report, calls = self.run_main(lambda: changed)
        self.assertEqual(result, 1)
        self.assertEqual(report["undeliveredRingDelta"], changed)
        self.assertIn(("docker", "start", drain.SIDECAR), calls)
        self.assertNotIn(("docker", "stop", "--time", "120", drain.GAME), calls)

    def test_post_stop_read_error_restarts_sidecar(self):
        def fail():
            raise RuntimeError("ring unavailable")
        result, report, calls = self.run_main(fail)
        self.assertEqual(result, 1)
        self.assertIn("ring unavailable", report["error"])
        self.assertIn(("docker", "start", drain.SIDECAR), calls)

    def test_game_stop_error_restarts_sidecar(self):
        result, report, calls = self.run_main(
            lambda: sample()["ring"],
            fail_on=("docker", "stop", "--time", "120", drain.GAME),
        )
        self.assertEqual(result, 1)
        self.assertIn("injected Docker failure", report["error"])
        self.assertIn(("docker", "start", drain.SIDECAR), calls)

    def test_clean_drain_removes_old_container(self):
        result, report, calls = self.run_main(lambda: sample()["ring"])
        self.assertEqual(result, 0)
        self.assertEqual(report["outcome"], "quiescent-stopped")
        self.assertFalse(report["exactBarrier"])
        self.assertIn(("docker", "rm", drain.SIDECAR), calls)
        self.assertNotIn(("docker", "start", drain.SIDECAR), calls)

    def test_grammar_report_binds_live_regex_and_fixture_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures = root / "fixtures.json"
            fixtures.write_text("[]\n")
            report = root / "report.json"
            expression = {"VEIN_LOG_CHAT_RE": "(?<msg>hello)"}
            canonical = json.dumps(expression, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
            value = {"version": 1, "status": "pass", "configuredKeys": ["VEIN_LOG_CHAT_RE"],
                     "keys": {"VEIN_LOG_CHAT_RE": {"status": "pass"}},
                     "configSha256": hashlib.sha256(canonical).hexdigest(),
                     "fixtureSha256": hashlib.sha256(fixtures.read_bytes()).hexdigest(),
                     "generatedAtUtc": dt.datetime.now(dt.timezone.utc).isoformat()}
            report.write_text(json.dumps(value))
            with mock.patch.object(drain, "command", return_value=json.dumps(["VEIN_LOG_CHAT_RE=(?<msg>hello)",
                                                                                "TAKARO_REGISTRATION_TOKEN=hidden"])):
                self.assertEqual(drain.check_grammar_report(report, fixtures)["status"], "pass")
                value["configSha256"] = "0" * 64
                report.write_text(json.dumps(value))
                with self.assertRaisesRegex(RuntimeError, "does not match"):
                    drain.check_grammar_report(report, fixtures)


if __name__ == "__main__":
    unittest.main()
