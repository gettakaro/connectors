#!/usr/bin/env python3
"""Versioned tile packs: a map root holds one folder per render plus a `current` pointer (maptiles.h).

  <root>/current            one line, the folder name being served
  <root>/pack-<UTC time>/   map.json + <z>/<x>/<y>.png

A pack is built in a hidden `.pack-….partial` folder, renamed into place when complete, and only then
does `current` change (write to a temp file, then rename over it). The connector reads `current` once
per request, so it serves either the old pack or the new one, never a mix or a half-copied pack.

Usage: pack.py install <rendered root> <server map root>   copy the current pack over and switch to it
Stdlib only.
"""
import os
import shutil
import sys
import time

POINTER = "current"
PREFIX = "pack-"
KEEP = 2  # the live pack and the one before it (a request may still be reading the old one)


def read_current(root):
    try:
        with open(os.path.join(root, POINTER)) as f:
            name = f.read().strip()
    except FileNotFoundError:
        return None
    return name or None


def _new_name(root):
    # microseconds keep names unique and in creation order, which _prune relies on
    while True:
        now = time.time()
        name = PREFIX + time.strftime("%Y%m%dT%H%M%S", time.gmtime(now)) + f"{int(now * 1e6) % 1000000:06d}Z"
        if not os.path.exists(os.path.join(root, name)):
            return name


def _set_current(root, name):
    tmp = os.path.join(root, f".{POINTER}.tmp")
    with open(tmp, "w") as f:
        f.write(name + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, os.path.join(root, POINTER))


def _prune(root):
    live = read_current(root)
    packs = sorted(d for d in os.listdir(root) if d.startswith(PREFIX) and os.path.isdir(os.path.join(root, d)))
    keep = set(packs[-KEEP:]) | {live}
    for d in packs:
        if d not in keep:
            shutil.rmtree(os.path.join(root, d))
    for d in os.listdir(root):  # leftovers of an interrupted build
        if d.startswith("." + PREFIX) and d.endswith(".partial"):
            shutil.rmtree(os.path.join(root, d))


def publish(root, build, name=None):
    """Run build(dir, name) into a fresh folder under root, then make it the live pack. Returns its name."""
    os.makedirs(root, exist_ok=True)
    if not name or os.path.exists(os.path.join(root, name)):
        name = _new_name(root)
    partial = os.path.join(root, f".{name}.partial")
    shutil.rmtree(partial, ignore_errors=True)
    os.makedirs(partial)
    build(partial, name)
    if not os.path.isfile(os.path.join(partial, "map.json")):
        raise RuntimeError(f"{partial}: build wrote no map.json")
    os.rename(partial, os.path.join(root, name))
    _set_current(root, name)
    _prune(root)
    return name


def install(src_root, dest_root):
    """Copy src_root's live pack into dest_root (a running server's map folder) and switch to it."""
    name = read_current(src_root)
    if not name:
        raise RuntimeError(f"{src_root}: no {POINTER} pointer, render a pack first")
    src = os.path.join(src_root, name)
    return publish(dest_root, lambda d, _n: shutil.copytree(src, d, dirs_exist_ok=True), name)


def main(argv):
    if len(argv) != 4 or argv[1] != "install":
        sys.exit(__doc__)
    print(f"{install(argv[2], argv[3])} is now live in {argv[3]}")


if __name__ == "__main__":
    main(sys.argv)
