#!/usr/bin/env python3
"""Tests for pack.py: python3 -I -m unittest discover -s games/enshrouded/mod/tools/map-tiles"""
import os
import tempfile
import unittest

import pack


def tiles(root, name):
    """{relative tile path: body} of one pack folder."""
    out = {}
    base = os.path.join(root, name)
    for d, _, files in os.walk(base):
        for f in files:
            if f.endswith(".png"):
                with open(os.path.join(d, f)) as fh:
                    out[os.path.relpath(os.path.join(d, f), base)] = fh.read()
    return out


def builder(body, paths=("0/0/0.png", "4/39/45.png", "4/40/45.png")):
    def build(d, rev):
        for p in paths:
            os.makedirs(os.path.dirname(os.path.join(d, p)), exist_ok=True)
            with open(os.path.join(d, p), "w") as f:
                f.write(body)
        with open(os.path.join(d, "map.json"), "w") as f:
            f.write('{"revision": "%s"}' % rev)
    return build


class PackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "map")

    def tearDown(self):
        self.tmp.cleanup()

    def test_rerender_switches_every_tile_at_once(self):
        old = pack.publish(self.root, builder("old"))
        seen = {}

        def slow_build(d, rev):
            # mid-build the live pack is still entirely the old render
            builder("new", ("0/0/0.png", "4/39/45.png"))(d, rev)
            os.remove(os.path.join(d, "map.json"))
            seen["mid"] = set(tiles(self.root, pack.read_current(self.root)).values())
            builder("new")(d, rev)

        new = pack.publish(self.root, slow_build)
        self.assertNotEqual(old, new)
        self.assertEqual(seen["mid"], {"old"})
        self.assertEqual(pack.read_current(self.root), new)
        self.assertEqual(set(tiles(self.root, new).values()), {"new"})
        self.assertEqual(set(tiles(self.root, old).values()), {"old"})  # untouched, still readable

    def test_failed_build_leaves_the_live_pack(self):
        old = pack.publish(self.root, builder("old"))

        def broken(d, rev):
            builder("new")(d, rev)
            raise RuntimeError("render crashed")

        with self.assertRaises(RuntimeError):
            pack.publish(self.root, broken)
        self.assertEqual(pack.read_current(self.root), old)
        with self.assertRaises(RuntimeError):  # no map.json: never switched live
            pack.publish(self.root, lambda d, rev: None)
        self.assertEqual(pack.read_current(self.root), old)
        pack.publish(self.root, builder("new"))  # the next good build also clears the leftovers
        self.assertFalse([d for d in os.listdir(self.root) if d.endswith(".partial")])

    def test_prune_keeps_live_and_previous(self):
        names = [pack.publish(self.root, builder(str(i))) for i in range(4)]
        left = sorted(d for d in os.listdir(self.root) if d.startswith(pack.PREFIX))
        self.assertEqual(left, names[-2:])
        self.assertTrue(os.path.isfile(os.path.join(self.root, pack.POINTER)))
        self.assertFalse(os.path.exists(os.path.join(self.root, ".current.tmp")))

    def test_install_copies_then_switches(self):
        src = os.path.join(self.tmp.name, "rendered")
        name = pack.publish(src, builder("new"))
        old = pack.publish(self.root, builder("old"))
        self.assertEqual(pack.install(src, self.root), name)
        self.assertEqual(pack.read_current(self.root), name)
        self.assertEqual(set(tiles(self.root, name).values()), {"new"})
        self.assertIn(old, os.listdir(self.root))
        with self.assertRaises(RuntimeError):
            pack.install(os.path.join(self.tmp.name, "empty"), self.root)


if __name__ == "__main__":
    unittest.main()
