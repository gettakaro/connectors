"""Game Profile checks that need the whole games/vein tree (the C++ tests only see mod/)."""

import hashlib
import re
import struct
import subprocess
import sys
import unittest
from pathlib import Path

GAME = Path(__file__).resolve().parent.parent
PNG = GAME / "assets" / "icon-256.png"
SEMVER = re.compile(
    r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?"
    r"(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$")


class GameProfileTest(unittest.TestCase):
    def test_connector_version_is_the_release_version(self):
        header = (GAME / "mod" / "src" / "common.h").read_text()
        m = re.search(r'#define TAKARO_PLUGIN_VERSION "([^"]+)" // x-release-please-version', header)
        self.assertIsNotNone(m, "release-please marker on TAKARO_PLUGIN_VERSION")
        version = (GAME / "version.txt").read_text().strip()
        self.assertEqual(m.group(1), version)
        self.assertRegex(version, SEMVER)
        self.assertLessEqual(len(version), 64)

    def test_icon_is_a_256_png_within_128_kib(self):
        png = PNG.read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(png[12:16], b"IHDR")
        self.assertEqual(struct.unpack(">II", png[16:24]), (256, 256))
        self.assertLessEqual(len(png), 128 * 1024)
        self.assertRegex(hashlib.sha256(png).hexdigest()[:12], r"^[0-9a-f]{12}$")

    def test_compiled_icon_matches_the_asset(self):
        tool = GAME / "mod" / "tools" / "gen_game_icon.py"
        r = subprocess.run([sys.executable, str(tool), "--check"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
