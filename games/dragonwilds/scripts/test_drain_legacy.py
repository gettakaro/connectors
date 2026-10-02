"""Failure-path and import checks for the legacy handoff tool; never starts Docker."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SPEC = importlib.util.spec_from_file_location("legacy_drain", Path(__file__).with_name("drain-legacy.py"))
assert SPEC and SPEC.loader
drain = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(drain)

# Fake EOS ProductUserIds: 32 hex characters, never a real player's.
PLAYER = "0123456789abcdef0123456789abcdef"
OTHER = "fedcba9876543210fedcba9876543210"


def sample():
    return {
        "health": {"takaroIdentified": True, "pendingEvents": 0, "eventCursor": 55, "eventScanCursor": 55},
        "ring": {"bootId": "same-boot", "latestSeq": 55, "events": []},
        "pluginPlayers": [],
    }


def legacy_tree(root: Path) -> Path:
    legacy = root / "legacy"
    legacy.mkdir()
    player = {"gameId": PLAYER, "name": "FixturePlayer", "epicOnlineServicesId": PLAYER,
              "platformId": f"epic:{PLAYER}"}
    (legacy / "event-cursor.json").write_text(json.dumps({"seq": 452, "bootId": "c0ffee00c0ffee00"}))
    (legacy / "known-players.json").write_text(json.dumps([player]))
    (legacy / "online-players.json").write_text("[]")
    (legacy / "timed-bans.json").write_text(json.dumps(
        [{"gameId": OTHER, "expiresAt": "2099-01-01T00:00:00.000Z", "reason": "fixture"}]))
    return legacy


class CleanTest(unittest.TestCase):
    def test_clean_requires_same_cursor_and_empty_players(self):
        self.assertEqual(drain.clean(sample()), (True, []))
        changed = sample()
        changed["ring"]["latestSeq"] = 56
        self.assertEqual(drain.clean(changed)[0], False)

    def test_online_player_blocks_drain(self):
        online = sample()
        online["pluginPlayers"] = [{"gameId": PLAYER}]
        self.assertEqual(drain.clean(online)[0], False)

    def test_pending_or_unidentified_sidecar_blocks_drain(self):
        pending = sample()
        pending["health"]["pendingEvents"] = 2
        self.assertEqual(drain.clean(pending)[0], False)
        offline = sample()
        offline["health"]["takaroIdentified"] = False
        self.assertEqual(drain.clean(offline)[0], False)

    def test_missing_cursor_rejects_drain(self):
        missing = sample()
        del missing["health"]["eventScanCursor"]
        self.assertEqual(drain.clean(missing)[0], False)


class HttpTest(unittest.TestCase):
    def test_command_keeps_http_line_endings(self):
        # Text-mode subprocess output turned "\r\n" into "\n" and every live HTTP read failed.
        raw = drain.command("printf", "HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\n[]")
        self.assertEqual(drain.parse_http(raw), [])

    def test_plain_and_chunked_bodies(self):
        plain = 'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{"seq": 12}'
        self.assertEqual(drain.parse_http(plain), {"seq": 12})
        chunked = 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n{"a":\r\n2\r\n1}\r\n0\r\n\r\n'
        self.assertEqual(drain.parse_http(chunked), {"a": 1})

    def test_error_status_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "HTTP 401"):
            drain.parse_http('HTTP/1.1 401 Unauthorized\r\n\r\n{"error":"unauthorized"}')


class ImportTest(unittest.TestCase):
    def test_imports_every_legacy_file_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = legacy_tree(root)
            native = root / "native"
            report = drain.import_state(legacy, native)
            for name in drain.LEGACY_FILES:
                self.assertEqual((native / name).read_bytes(), (legacy / name).read_bytes())
                self.assertEqual(report["files"][name]["status"], "imported")
            self.assertEqual(sorted(p.name for p in native.iterdir()), sorted(drain.LEGACY_FILES))

    def test_absent_files_are_reported_not_invented(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = legacy_tree(root)
            (legacy / "timed-bans.json").unlink()
            report = drain.import_state(legacy, root / "native")
            self.assertEqual(report["files"]["timed-bans.json"], {"status": "absent"})
            self.assertFalse((root / "native" / "timed-bans.json").exists())

    def test_refuses_a_directory_the_native_connector_already_used(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = legacy_tree(root)
            native = root / "native"
            native.mkdir()
            (native / "event-outbox.json").write_text("{}")
            with self.assertRaisesRegex(RuntimeError, "already ran here"):
                drain.import_state(legacy, native)
            self.assertFalse((native / "event-cursor.json").exists())

    def test_refuses_to_overwrite_different_state_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = legacy_tree(root)
            native = root / "native"
            native.mkdir()
            (native / "timed-bans.json").write_text("[]")
            with self.assertRaisesRegex(RuntimeError, "different content"):
                drain.import_state(legacy, native)
            self.assertEqual(sorted(p.name for p in native.iterdir()), ["timed-bans.json"])

    def test_identical_existing_file_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = legacy_tree(root)
            native = root / "native"
            native.mkdir()
            (native / "online-players.json").write_text("[]")
            report = drain.import_state(legacy, native)
            self.assertEqual(report["files"]["online-players.json"]["status"], "identical")

    def test_corrupt_or_misshapen_legacy_state_is_refused(self):
        cases = {
            "timed-bans.json": ["not json", json.dumps([{"gameId": OTHER}]), json.dumps({"bans": []}),
                                json.dumps([{"gameId": OTHER, "expiresAt": "tomorrow"}])],
            "event-cursor.json": [json.dumps({"seq": -1}), json.dumps({"seq": "4"}), json.dumps([])],
            "known-players.json": [json.dumps([{"name": "no id"}]), json.dumps({})],
        }
        for name, bodies in cases.items():
            for body in bodies:
                with self.subTest(name=name, body=body), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    legacy = legacy_tree(root)
                    (legacy / name).write_text(body)
                    with self.assertRaises((RuntimeError, ValueError)):
                        drain.import_state(legacy, root / "native")
                    self.assertEqual(list((root / "native").iterdir()), [])

    def test_same_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            legacy = legacy_tree(Path(directory))
            with self.assertRaisesRegex(RuntimeError, "in place"):
                drain.import_state(legacy, legacy)


class MainTest(unittest.TestCase):
    def run_main(self, post, fail_on=None, extra=()):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evidence = root / "evidence"
            legacy = legacy_tree(root)
            argv = ["drain-legacy.py", "--evidence-dir", str(evidence),
                    "--fence-proof", str(root / "fence.json"), "--fence-check", str(root / "check"), *extra]
            argv = [arg.replace("@LEGACY@", str(legacy)).replace("@NATIVE@", str(root / "native")) for arg in argv]
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
                 mock.patch.object(drain, "snapshot", side_effect=lambda: sample()), \
                 mock.patch.object(drain, "snapshot_without_sidecar", side_effect=post), \
                 mock.patch.object(drain, "command", side_effect=command), \
                 mock.patch.object(drain.time, "sleep"):
                result = drain.main()
            report = json.loads((evidence / "drain-report.json").read_text())
            self.assertEqual((evidence / "drain-report.json").stat().st_mode & 0o077, 0)
            native = sorted(p.name for p in (root / "native").iterdir()) if (root / "native").exists() else []
            return result, report, calls, native

    def test_ring_delta_aborts_and_restarts_sidecar(self):
        changed = {"bootId": "same-boot", "latestSeq": 56, "events": [{"seq": 56}]}
        result, report, calls, native = self.run_main(
            lambda: changed, extra=("--legacy-state-dir", "@LEGACY@", "--native-state-dir", "@NATIVE@"))
        self.assertEqual(result, 1)
        self.assertEqual(report["undeliveredRingDelta"], changed)
        self.assertIn(("docker", "start", drain.SIDECAR), calls)
        self.assertNotIn(("docker", "stop", "--time", "120", drain.GAME), calls)
        self.assertEqual(native, [])

    def test_post_stop_read_error_restarts_sidecar(self):
        def fail():
            raise RuntimeError("ring unavailable")
        result, report, calls, _ = self.run_main(fail)
        self.assertEqual(result, 1)
        self.assertIn("ring unavailable", report["error"])
        self.assertIn(("docker", "start", drain.SIDECAR), calls)

    def test_game_stop_error_restarts_sidecar(self):
        result, report, calls, _ = self.run_main(
            lambda: sample()["ring"], fail_on=("docker", "stop", "--time", "120", drain.GAME))
        self.assertEqual(result, 1)
        self.assertIn("injected Docker failure", report["error"])
        self.assertIn(("docker", "start", drain.SIDECAR), calls)

    def test_clean_drain_removes_old_container_and_imports_state(self):
        result, report, calls, native = self.run_main(
            lambda: sample()["ring"], extra=("--legacy-state-dir", "@LEGACY@", "--native-state-dir", "@NATIVE@"))
        self.assertEqual(result, 0, report.get("error"))
        self.assertEqual(report["outcome"], "quiescent-stopped-imported")
        self.assertFalse(report["exactBarrier"])
        self.assertIn(("docker", "rm", drain.SIDECAR), calls)
        self.assertNotIn(("docker", "start", drain.SIDECAR), calls)
        self.assertEqual(native, sorted(drain.LEGACY_FILES))

    def test_import_only_touches_no_container(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = legacy_tree(root)
            evidence = root / "evidence"
            argv = ["drain-legacy.py", "--evidence-dir", str(evidence), "--import-only",
                    "--legacy-state-dir", str(legacy), "--native-state-dir", str(root / "native")]
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(drain, "command", side_effect=AssertionError("no Docker in import-only")):
                self.assertEqual(drain.main(), 0)
            report = json.loads((evidence / "import-report.json").read_text())
            self.assertEqual(report["outcome"], "imported")
            self.assertTrue((root / "native" / "known-players.json").is_file())
            self.assertEqual(os.stat(evidence / "import-report.json").st_mode & 0o077, 0)


if __name__ == "__main__":
    unittest.main()
