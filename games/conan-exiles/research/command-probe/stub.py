#!/usr/bin/env python3
"""Local stand-in for Takaro in the #205 command research probe. Not a Takaro protocol.

The connector's research probe (native/core/conan/command_probe.h) talks to this stub through
files in one directory, which both sides can reach (the server's Saved folder, mounted):

  catalog.json      written here: {revision, prefix, commands:[{trigger, aliases}]}
  stub.json         heartbeat written here every 0.5 s; the connector treats the stub as offline
                    3 s after the last one
  inbox/<id>.json   one invocation from the connector
  outbox/<id>.json  the replies, {"replies": [...]}, sent to the caller only

Usage:
  stub.py catalog DIR --revision r1 [--prefix /tk] [--without TRIGGER ...]
  stub.py run DIR [--admins STEAM64,...]

Every invocation is appended to DIR/receipts.jsonl. Invocation files older than 5 s when `run`
starts belong to no live caller (the connector gave up on them): they are moved to inbox-stale/
and never answered (no replay). Each invocation is answered on its own thread, so a slow command
never stops the heartbeat.
"""

import argparse
import json
import os
import shutil
import sys
import threading
import time

# trigger -> aliases, backends (one reply line each: Takaro runs every installed command that
# shares a trigger), and whether only admins may run it. Permission is decided here, on the
# server-supplied gameId, never on anything the player typed.
COMMANDS = {
    "ping": {"aliases": ["p"], "backends": ["pong"], "admin": False},
    "whoami": {"aliases": [], "backends": ["whoami"], "admin": False},
    "echo": {"aliases": ["say"], "backends": ["echo"], "admin": False},
    "fan": {"aliases": [], "backends": ["fan-a", "fan-b"], "admin": False},
    "admin": {"aliases": [], "backends": ["admin"], "admin": True},
    "slow": {"aliases": [], "backends": ["slow"], "admin": False},
    "help": {"aliases": ["commands"], "backends": ["help"], "admin": False},
}


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def cmd_catalog(args):
    cmds = [
        {"trigger": t, "aliases": c["aliases"]}
        for t, c in COMMANDS.items()
        if t not in (args.without or [])
    ]
    cat = {"revision": args.revision, "prefix": args.prefix, "commands": cmds}
    write_atomic(os.path.join(args.dir, "catalog.json"), json.dumps(cat, indent=2) + "\n")
    print(f"catalog {args.revision}: {len(cmds)} commands, prefix {args.prefix}")


def answer(inv, admins, catalog):
    trigger = inv.get("trigger", "")
    player = inv.get("player") or {}
    game_id = player.get("gameId", "")
    revision = catalog.get("revision")
    published = [c.get("trigger") for c in catalog.get("commands", [])]
    spec = COMMANDS.get(trigger)
    if spec is None or trigger not in published:
        return [f"{trigger} is no longer in the catalog."]
    if spec["admin"] and game_id not in admins:
        return [f"You do not have permission to run {trigger}."]
    out = []
    for backend in spec["backends"]:
        if backend == "pong":
            out.append(f"Pong! (stub, catalog {inv.get('catalogRevision')})")
        elif backend == "whoami":
            out.append(
                f"You are {player.get('name')} gameId {game_id} {player.get('platformId')}"
                f" (from the server; {len(inv.get('args', []))} typed args ignored)"
            )
        elif backend == "echo":
            out.append("args: " + " | ".join(f"[{a}]" for a in inv.get("args", [])))
        elif backend in ("fan-a", "fan-b"):
            out.append(f"{backend} ran for {trigger}")
        elif backend == "admin":
            out.append("admin command ran")
        elif backend == "slow":
            time.sleep(7)
            out.append("slow command finished (late)")
        elif backend == "help":
            names = ", ".join(sorted(published))
            out.append(f"Commands (catalog {revision}): {names}")
    return out


def cmd_run(args):
    d = args.dir
    admins = set(a for a in (args.admins or "").split(",") if a)
    inbox, outbox = os.path.join(d, "inbox"), os.path.join(d, "outbox")
    stale = os.path.join(d, "inbox-stale")
    for p in (inbox, outbox, stale):
        os.makedirs(p, exist_ok=True)
    receipts = open(os.path.join(d, "receipts.jsonl"), "a", encoding="utf-8")

    lock = threading.Lock()

    def log(kind, **kw):
        rec = {"at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "kind": kind, **kw}
        with lock:
            receipts.write(json.dumps(rec) + "\n")
            receipts.flush()
            print(json.dumps(rec), flush=True)

    started = time.time()
    for name in sorted(os.listdir(inbox)):
        path = os.path.join(inbox, name)
        if name.endswith(".json") and started - os.path.getmtime(path) > 5:
            shutil.move(path, os.path.join(stale, name))
            log("not-replayed", id=name[:-5])
    log("stub-started", admins=sorted(admins))
    last_hb = 0.0
    while True:
        now = time.time()
        if now - last_hb >= 0.5:
            write_atomic(os.path.join(d, "stub.json"), json.dumps({"heartbeatMs": int(now * 1000)}))
            last_hb = now
        for name in sorted(os.listdir(inbox)):
            if not name.endswith(".json"):
                continue
            path = os.path.join(inbox, name)
            try:
                with open(path, encoding="utf-8") as f:
                    inv = json.load(f)
            except (OSError, ValueError):
                continue
            os.remove(path)
            try:
                with open(os.path.join(d, "catalog.json"), encoding="utf-8") as f:
                    catalog = json.load(f)
            except (OSError, ValueError):
                catalog = {}
            log("invocation", invocation=inv)

            def reply(inv=inv, catalog=catalog):
                replies = answer(inv, admins, catalog)
                write_atomic(os.path.join(outbox, inv["id"] + ".json"), json.dumps({"replies": replies}))
                log("replied", id=inv["id"], replies=replies)

            threading.Thread(target=reply, daemon=True).start()
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("catalog")
    c.add_argument("dir")
    c.add_argument("--revision", required=True)
    c.add_argument("--prefix", default="/tk")
    c.add_argument("--without", action="append")
    r = sub.add_parser("run")
    r.add_argument("dir")
    r.add_argument("--admins", default="")
    args = ap.parse_args()
    {"catalog": cmd_catalog, "run": cmd_run}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
