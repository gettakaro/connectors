#!/usr/bin/env python3
"""Runs tests/build/poller_test against a fake of the bridge's loopback /mod API.

The fake follows games/conan-exiles/bridge/src/mod/commandBridge.ts in strict mode
(requireModSourceAttribution=true): one queued command per GET /mod/poll, POST /mod/result
by requestId (404 when unknown), and 400 when the source is missing or not TakaroConan*.

Usage: fake_bridge_test.py <poller_test binary>
"""
import json
import re
import socket
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

SOURCE_RE = re.compile(r"^TakaroConan($|[ /:@+-])", re.I)


class Bridge:
    def __init__(self):
        self.lock = threading.Lock()
        self.queue = []
        self.results = {}
        self.poll_sources = set()
        self.result_sources = set()
        self.polls = 0
        self.mode = "normal"  # normal | malformed | chunked
        self.fail_results = 0  # answer this many POST /mod/result with 500 first
        self.server = None

    def enqueue(self, action, args):
        rid = str(uuid.uuid4())
        with self.lock:
            self.queue.append({"requestId": rid, "action": action, "args": args})
        return rid

    def wait_result(self, rid, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if rid in self.results:
                    return self.results[rid]
            time.sleep(0.02)
        raise AssertionError(f"no result for {rid} within {timeout}s")

    def start(self, port):
        bridge = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def source(self):
                q = parse_qs(urlparse(self.path).query).get("source", [None])[0]
                return q or self.headers.get("X-Takaro-Mod-Source")

            def reply(self, status, obj, chunked=False):
                data = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                if chunked:
                    self.send_header("Transfer-Encoding", "chunked")
                    self.end_headers()
                    half = len(data) // 2
                    for part in (data[:half], data[half:]):
                        self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)

            def check_source(self):
                src = self.source()
                if not src:
                    self.reply(400, {"error": "Missing mod source attribution"})
                    return None
                if not SOURCE_RE.match(src):
                    self.reply(400, {"error": "Invalid mod source attribution"})
                    return None
                return src

            def do_GET(self):
                if urlparse(self.path).path != "/mod/poll":
                    return self.reply(404, {"error": "Not found"})
                src = self.check_source()
                if not src:
                    return
                with bridge.lock:
                    bridge.polls += 1
                    bridge.poll_sources.add(src)
                    mode = bridge.mode
                    cmd = bridge.queue.pop(0) if bridge.queue else None
                if mode == "malformed":
                    data = b"{not json"
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                body = {"hasCommand": True, "command": cmd} if cmd else {"hasCommand": False}
                self.reply(200, body, chunked=(mode == "chunked"))

            def do_POST(self):
                if urlparse(self.path).path != "/mod/result":
                    return self.reply(404, {"error": "Not found"})
                src = self.check_source()
                if not src:
                    return
                n = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(n) or b"{}")
                rid = payload.get("requestId")
                if not rid:
                    return self.reply(400, {"error": "Missing requestId"})
                with bridge.lock:
                    if bridge.fail_results > 0:
                        bridge.fail_results -= 1
                        return self.reply(500, {"error": "injected failure"})
                with bridge.lock:
                    bridge.result_sources.add(src)
                    bridge.results[rid] = payload.get("result")
                self.reply(200, {"success": True})

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    binary = sys.argv[1]
    port = free_port()
    bridge = Bridge()
    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    # Bridge down at start: the poller must back off and connect once it appears.
    proc = subprocess.Popen([binary, f"http://127.0.0.1:{port}", "25"], stderr=subprocess.PIPE, text=True)
    time.sleep(1.0)
    bridge.start(port)

    rid = bridge.enqueue("sendMessage", {"message": "hello world"})
    r = bridge.wait_result(rid)
    check(r.get("success") is True and r.get("delivered") == 1, f"global send after bridge came up: {r}")
    check(r.get("transport") == "TakaroConan-native", "result names the native transport")
    check(bridge.poll_sources == {"TakaroConan-native"}, f"poll source attribution: {bridge.poll_sources}")
    check(bridge.result_sources == {"TakaroConan-native"}, f"result source attribution: {bridge.result_sources}")

    rid = bridge.enqueue("sendMessage", {"message": "hi", "recipient": "76561198000000001"})
    r = bridge.wait_result(rid)
    check(r.get("success") is True, f"targeted send: {r}")

    rid = bridge.enqueue("sendMessage", {"message": "hi", "recipient": "offline"})
    r = bridge.wait_result(rid)
    check(r.get("success") is False and "not online" in r.get("error", ""), f"offline recipient: {r}")

    rid = bridge.enqueue("sendMessage", {"message": ""})
    # The stub executor accepts an empty message; the real one refuses it (unit-tested via chat
    # in the live proof). Here only the round trip matters.
    bridge.wait_result(rid)

    rid = bridge.enqueue("kickPlayer", {"gameId": "1"})
    r = bridge.wait_result(rid)
    check(r.get("success") is False and "Unsupported action kickPlayer" in r.get("error", ""), f"unsupported: {r}")

    # Several queued at once: the poller drains them back to back.
    rids = [bridge.enqueue("sendMessage", {"message": f"burst {i}"}) for i in range(5)]
    t0 = time.time()
    rs = [bridge.wait_result(x) for x in rids]
    check(all(x.get("success") for x in rs) and time.time() - t0 < 2.0, "burst of 5 drained quickly")

    rid = bridge.enqueue("sendMessage", {"message": "stall"})
    r = bridge.wait_result(rid)
    check(r.get("success") is False and "did not respond" in r.get("error", ""), f"stalled game thread: {r}")
    time.sleep(1.5)  # the stall ends; the queue must work again
    rid = bridge.enqueue("sendMessage", {"message": "after stall"})
    check(bridge.wait_result(rid).get("success") is True, "recovers after a stall")

    bridge.fail_results = 2
    rid = bridge.enqueue("sendMessage", {"message": "result retried"})
    check(bridge.wait_result(rid).get("success") is True and bridge.fail_results == 0,
          "result POST retried after two 500s")

    bridge.mode = "chunked"
    rid = bridge.enqueue("sendMessage", {"message": "chunked"})
    check(bridge.wait_result(rid).get("success") is True, "chunked poll response")

    bridge.mode = "malformed"
    time.sleep(0.5)
    bridge.mode = "normal"
    rid = bridge.enqueue("sendMessage", {"message": "after malformed"})
    check(bridge.wait_result(rid).get("success") is True, "recovers after a malformed poll body")

    # Bridge restart.
    bridge.stop()
    time.sleep(1.0)
    bridge2 = Bridge()
    bridge2.start(port)
    rid = bridge2.enqueue("sendMessage", {"message": "after restart"})
    check(bridge2.wait_result(rid).get("success") is True, "reconnects after a bridge restart")

    proc.terminate()
    _, err = proc.communicate(timeout=10)
    check(proc.returncode in (0, -15), f"poller_test exit code {proc.returncode}")
    check("bridge connected" in err, "logs the connection")
    bridge2.stop()

    print(f"{len(failures)} failure(s)")
    if failures:
        print(err[-4000:])
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
