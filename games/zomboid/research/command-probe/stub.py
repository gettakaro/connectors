#!/usr/bin/env python3
"""Stub backend for the #201 command probe. Not a Takaro protocol.

GET  /catalog  -> {"revision": n, "commands": [{"name", "installationId"}]}
POST /invoke   -> records a receipt line and answers by mode:
                  ok     {"status":"ok","replies":[one line per matching installation]}
                  deny   {"status":"denied","replies":["You may not run <name>."]}
                  slow   answers after 6 s (the probe gives up at 3 s)
                  drop   closes the socket without answering
                  offline: /catalog and /invoke answer 503
POST /control  -> {"mode": "..."} and/or {"catalog": {...}} (the revision is bumped)

Usage: stub.py [--port 8790] [--receipts receipts.jsonl]
"""
import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {
    "mode": "ok",
    "catalog": {
        "revision": 1,
        "commands": [
            {"name": "tprobe", "installationId": "install-a"},
            {"name": "tdeny", "installationId": "install-a"},
        ],
    },
}
LOCK = threading.Lock()
RECEIPTS = None


def receipt(entry):
    entry["stubAt"] = time.time()
    with LOCK, open(RECEIPTS, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(entry) + "\n")


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("content-length", "0"))
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        if self.path != "/catalog":
            return self._send(404, {})
        with LOCK:
            mode, catalog = STATE["mode"], STATE["catalog"]
        if mode == "offline":
            return self._send(503, {"error": "offline"})
        self._send(200, catalog)

    def do_POST(self):
        body = self._body()
        if self.path == "/control":
            with LOCK:
                if "mode" in body:
                    STATE["mode"] = body["mode"]
                if "catalog" in body:
                    cat = body["catalog"]
                    cat.setdefault("revision", STATE["catalog"]["revision"] + 1)
                    STATE["catalog"] = cat
                out = dict(STATE)
            return self._send(200, out)
        if self.path != "/invoke":
            return self._send(404, {})
        with LOCK:
            mode = STATE["mode"]
        receipt({"invocation": body, "mode": mode})
        name = body.get("command", "")
        if mode == "offline":
            return self._send(503, {"error": "offline"})
        if mode == "drop":
            self.close_connection = True
            self.connection.close()
            return None
        if mode == "slow":
            time.sleep(6)
        if mode == "deny" or name.lower() == "tdeny":
            return self._send(200, {"status": "denied", "replies": [f"You may not run {name}."]})
        user = body.get("caller", {}).get("username")
        replies = [
            f"[{inst}] {name} ran for {user} args={body.get('args')}"
            for inst in body.get("installations", [])
        ]
        self._send(200, {"status": "ok", "replies": replies})

    def log_message(self, fmt, *args):
        print("stub:", fmt % args, flush=True)


def main():
    global RECEIPTS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--receipts", default="stub-receipts.jsonl")
    a = ap.parse_args()
    RECEIPTS = a.receipts
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
