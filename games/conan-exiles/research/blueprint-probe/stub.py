#!/usr/bin/env python3
"""Stub Takaro end for the Blueprint SDK research probe (native/core/conan/blueprint_probe.h).

Research only. It stands in for a Takaro "invoke command as player" message that the connector
protocol does not have yet. It answers the probe's requests in <dir>:

  run    POST /command/{gameServerId}/trigger {playerId, msg} on the Takaro REST API, so Takaro
         runs its own command pipeline (permissions, cost, cooldown, the module's private reply)
  event  recorded in receipts.jsonl only (Takaro has no custom event type)

The Takaro API key is read from standard input (one line) and kept in memory only, so it never
lands in the probe directory or on the server disk. The connector never holds it.

  echo "$KEY" | python stub.py <dir> <api-base-url> <domainId> <gameServerId>
"""
import json
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

DIR = Path(sys.argv[1])
BASE = sys.argv[2].rstrip("/")
DOMAIN = sys.argv[3]
GS = sys.argv[4]
KEY = sys.stdin.readline().strip()
UA = "takaro-conan-bpprobe-stub/0.1 (+research)"
INBOX, OUTBOX = DIR / "inbox", DIR / "outbox"
LOCK = threading.Lock()
PLAYERS = {}


def log(name, obj):
    obj = dict(obj, stubTs=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    with LOCK, open(DIR / name, "a", encoding="utf-8") as f:
        f.write(json.dumps(obj) + "\n")
    print(name, json.dumps(obj)[:300], flush=True)


def api(method, path, body=None):
    req = urllib.request.Request(BASE + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json", "User-Agent": UA,
                                          "Authorization": "Bearer " + KEY, "X-Takaro-Domain": DOMAIN})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read() or b"{}")


def player_id(steam64):
    if steam64 in PLAYERS:
        return PLAYERS[steam64]
    res = api("POST", "/gameserver/player/search",
              {"filters": {"gameServerId": [GS], "gameId": [steam64]}, "limit": 1})["data"]
    if res:
        PLAYERS[steam64] = res[0]["playerId"]
        return res[0]["playerId"]
    return None


def write_reply(rid, replies):
    tmp = OUTBOX / (rid + ".tmp")
    tmp.write_text(json.dumps({"replies": replies}), encoding="utf-8")
    tmp.replace(OUTBOX / (rid + ".json"))


def answer(path):
    try:
        r = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return
    path.unlink(missing_ok=True)
    rid = r["id"]
    log("received.jsonl", r)
    if r.get("verb") == "event":
        log("receipts.jsonl", dict(r, result="recorded (no Takaro custom event type)"))
        write_reply(rid, [])
        return
    t0 = time.time()
    try:
        pid = player_id(r["steam64"])
        if not pid:
            log("receipts.jsonl", dict(r, result="no Takaro player for this gameId"))
            write_reply(rid, ["Takaro does not know this player yet; nothing was run."])
            return
        res = api("POST", f"/command/{GS}/trigger", {"playerId": pid, "msg": r["text"]})
        ids = res.get("data", {}).get("executionIds", [])
        log("receipts.jsonl", dict(r, takaroPlayerId=pid, triggerHttp=200, executionIds=ids,
                                   triggerMs=int((time.time() - t0) * 1000)))
        # Takaro's own module answers the player (a private sendMessage); only say something when
        # nothing matched, so the player is never left without an answer.
        write_reply(rid, [] if ids else ["No Takaro command matched: " + r["text"]])
    except urllib.error.HTTPError as e:
        log("receipts.jsonl", dict(r, triggerHttp=e.code, error=e.read()[:300].decode("utf-8", "replace")))
        write_reply(rid, ["Takaro refused the request; it was not run."])
    except Exception as e:  # noqa: BLE001 - research stub: record and answer
        log("receipts.jsonl", dict(r, error=repr(e)))
        write_reply(rid, ["Takaro could not be reached; the request may not have run."])


def main():
    INBOX.mkdir(parents=True, exist_ok=True)
    OUTBOX.mkdir(parents=True, exist_ok=True)
    for old in INBOX.glob("*.json"):  # never answer a request from before this stub started
        if time.time() - old.stat().st_mtime > 5:
            old.rename(old.with_suffix(".stale"))
    print("stub: watching", DIR, "gameserver", GS, flush=True)
    while True:
        (DIR / "stub.tmp").write_text(json.dumps({"heartbeatMs": int(time.time() * 1000)}))
        (DIR / "stub.tmp").replace(DIR / "stub.json")
        for p in sorted(INBOX.glob("*.json")):
            work = p.with_suffix(".work")  # claimed once, so no request is answered twice
            p.replace(work)
            threading.Thread(target=answer, args=(work,), daemon=True).start()
        time.sleep(0.2)


if __name__ == "__main__":
    main()
