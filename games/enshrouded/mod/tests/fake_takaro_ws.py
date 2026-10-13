#!/usr/bin/env python3
"""Fake Takaro generic-connector WSS endpoint for the Enshrouded native connector's Wine integration test.

Grown from mod/probe/fake_takaro_ws.py (G0). Speaks Takaro's shapes (sidecar/src/takaro/protocol.ts and
Takaro's app-connector websocket.ts as observed on connect.takaro.io 2026-09-29):
  -> {type:'connected', payload:{clientId}}                     first frame, like the real server
  <- {type:'identify', payload:{identityToken, registrationToken, name}}
  -> {type:'identifyResponse', payload:{gameServerId}}          or payload.error for a bad identity
  <- {type:'ping'}  -> {type:'pong', payload:null, requestId}     the connector's application heartbeat
  -> {type:'ping'}  <- {type:'pong'}
  -> {type:'request', requestId, payload:{action, args}}        <- {type:'response', requestId, payload|error}
  <- {type:'gameEvent', payload:{type, data}}
It also sends WebSocket protocol PINGs with an empty payload every FAKE_WS_PING_S (Takaro does every 30 s) and
aborts a connection that leaves one unanswered.

FAKE_SCENARIO:
  native  drives the connector (the plugin loaded by tests/wine/host.exe) through the checks below, using the
          shared server directory mounted at FAKE_SRV (log file to append to, connector-state to read):
            identify, ping/pong both ways, testReachability/getPlayers/unknown-action requests, a log-tail
            player-connected event that stays unconfirmed while pongs are withheld and is confirmed by a later
            pong, a TCP abort with the unconfirmed player-disconnected resent on the next connection, a
            clean server close followed by a reconnect, and a silent (dead) link the connector must detect. Prints {"ev":"result",...} and exits 0 on success.
  count   accepts TLS and counts identifies (for the rejected-certificate runs).
  accept  answers every identify: accepted when its registrationToken is FAKE_EXPECT_REGISTRATION, else refused;
          logs the identity and name it carried (the plugin.json hot-reload runs).
Every observation is one JSON line on stdout.
"""
import asyncio
import json
import os
import ssl
import sys
import time
import uuid

import websockets

SCENARIO = os.environ.get("FAKE_SCENARIO", "native")
PORT = int(os.environ.get("FAKE_PORT", "8443"))
WS_PING_S = float(os.environ.get("FAKE_WS_PING_S", "5"))
SRV = os.environ.get("FAKE_SRV", "/srv")
IDENTITY = os.environ.get("FAKE_EXPECT_IDENTITY", "wine-identity")
REGISTRATION = os.environ.get("FAKE_EXPECT_REGISTRATION", "wine-registration")
NAME = os.environ.get("FAKE_EXPECT_NAME", "Wine Test")
T0 = time.monotonic()
SID = "76561198000005875"


def log(ev, **kw):
    print(json.dumps({"t": round(time.monotonic() - T0, 3), "ev": ev, **kw}), flush=True)


class Conn:
    def __init__(self, ws, n):
        self.ws, self.n = ws, n
        self.frames = asyncio.Queue()
        self.hold_pongs = False
        self.held = []
        self.client_pings = 0
        self.identify = None
        self.identified = asyncio.Event()
        self.closed = asyncio.Event()
        self.events = []

    async def send(self, obj):
        await self.ws.send(json.dumps(obj))

    async def reader(self):
        try:
            async for raw in self.ws:
                msg = json.loads(raw)
                t = msg.get("type")
                if t == "identify":
                    self.identify = msg.get("payload") or {}
                    p = self.identify
                    if SCENARIO == "accept":
                        ok = p.get("registrationToken") == REGISTRATION
                        log("identify", conn=self.n, ok=ok, identity=p.get("identityToken"), name=p.get("name"))
                    else:
                        ok = p.get("identityToken") == IDENTITY and p.get("registrationToken") == REGISTRATION and p.get("name") == NAME
                        log("identify", conn=self.n, ok=ok, keys=sorted(p.keys()))
                    if ok:
                        await self.send({"type": "identifyResponse", "payload": {"gameServerId": "fake-gs"}, "requestId": str(uuid.uuid4())})
                        self.identified.set()
                    else:
                        await self.send({"type": "identifyResponse", "payload": {"error": {"name": "BadRequestError", "message": "bad identify", "http": 400}}})
                elif t == "ping":
                    self.client_pings += 1
                    pong = {"type": "pong", "payload": None, "requestId": str(uuid.uuid4())}
                    if self.hold_pongs:
                        self.held.append(pong)
                    else:
                        await self.send(pong)
                elif t == "gameEvent":
                    self.events.append(msg.get("payload"))
                    log("gameEvent", conn=self.n, payload=msg.get("payload"))
                    await self.frames.put(msg)
                else:
                    await self.frames.put(msg)
        except websockets.ConnectionClosed:
            pass
        finally:
            self.closed.set()

    async def release_pongs(self):
        self.hold_pongs = False
        held, self.held = self.held, []
        for p in held:
            await self.send(p)
        return len(held)

    async def expect(self, pred, timeout, what):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                msg = await asyncio.wait_for(self.frames.get(), deadline - time.monotonic())
            except asyncio.TimeoutError:
                break
            if pred(msg):
                return msg
        raise AssertionError(f"timed out waiting for {what}")

    async def request(self, action, args, timeout=10):
        rid = str(uuid.uuid4())
        t = time.monotonic()
        await self.send({"type": "request", "requestId": rid, "payload": {"action": action, "args": args}})
        msg = await self.expect(lambda m: m.get("type") == "response" and m.get("requestId") == rid, timeout, f"response to {action}")
        log("response", conn=self.n, action=action, ms=round((time.monotonic() - t) * 1000, 1), payload=msg.get("payload"), error=msg.get("error"))
        return msg


conns = []
new_conn = asyncio.Event()
results = {}


def record(name, ok, **detail):
    results[name] = bool(ok)
    log("check", name=name, ok=bool(ok), **detail)


def outbox_pending():
    try:
        with open(os.path.join(SRV, "takaro", "connector-state", "event-outbox.json")) as fh:
            return len(json.load(fh)["pending"])
    except (OSError, ValueError, KeyError):
        return -1


def append_log(lines):
    with open(os.path.join(SRV, "logs", "enshrouded_server.log"), "a") as fh:
        fh.write("".join(line + "\n" for line in lines))


async def wait_until(pred, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        await asyncio.sleep(0.1)
    return pred()


async def heartbeat(c):
    """Takaro-style protocol ping with an empty payload; abort when the previous one was not answered."""
    waiter = None
    try:
        while True:
            await asyncio.sleep(WS_PING_S)
            if waiter is not None and not (waiter.done() and not waiter.cancelled() and waiter.exception() is None):
                log("ws_heartbeat_terminate", conn=c.n)
                c.ws.transport.abort()
                return
            waiter = await c.ws.ping(b"")
    except (asyncio.CancelledError, websockets.ConnectionClosed):
        pass


async def handler(ws):
    c = Conn(ws, len(conns) + 1)
    conns.append(c)
    log("accept", conn=c.n)
    await c.send({"type": "connected", "payload": {"clientId": f"client-{uuid.uuid4()}"}, "requestId": str(uuid.uuid4())})
    rt = asyncio.create_task(c.reader())
    hb = asyncio.create_task(heartbeat(c)) if WS_PING_S else None
    new_conn.set()
    try:
        await c.closed.wait()
    finally:
        rt.cancel()
        if hb:
            hb.cancel()
        log("conn_closed", conn=c.n, clientPings=c.client_pings, events=len(c.events))


async def next_conn(after, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if len(conns) > after:
            return conns[after]
        new_conn.clear()
        try:
            await asyncio.wait_for(new_conn.wait(), deadline - time.monotonic())
        except asyncio.TimeoutError:
            break
    raise AssertionError(f"no connection #{after + 1}")


JOIN = [
    f"[I 00:03:52,650] [online] Added peer 0(1) (steamid:{SID})",
    "[I 00:04:24,062] [server] Machine '1': Player '0(0)' logged in",
    "[I 00:04:24,063] [server] Player 'Limon' logged in with Permissions:",
]
LEAVE = ["[I 00:07:46,566] [server] Remove Player 'Limon'", "[I 00:07:46,704] [online] Removed peer 0(1)"]


async def scenario_native():
    c1 = await next_conn(0, 90)
    await asyncio.wait_for(c1.identified.wait(), 30)
    record("identify", c1.identify.get("identityToken") == IDENTITY and c1.identify.get("registrationToken") == REGISTRATION and c1.identify.get("name") == NAME)

    await c1.send({"type": "ping"})
    await c1.expect(lambda m: m.get("type") == "pong", 5, "pong")
    record("ping-pong-server", True)
    ok = await wait_until(lambda: c1.client_pings >= 1, 12)
    record("ping-pong-client", ok, clientPings=c1.client_pings)

    r = await c1.request("testReachability", "{}")
    p = r.get("payload") or {}
    record("request-testReachability", p.get("connectable") is True and "capabilities not ok" in (p.get("reason") or ""), payload=p)
    r = await c1.request("getPlayers", [])
    record("request-getPlayers", r.get("payload") == [], payload=r.get("payload"))
    r = await c1.request("flyToMoon", None)
    record("request-unknown-action", r.get("error") == "Unknown Takaro action 'flyToMoon'", error=r.get("error"))

    # event delivery, confirmed only by the pong of a later ping
    c1.hold_pongs = True
    append_log(JOIN)
    ev = await c1.expect(lambda m: m.get("type") == "gameEvent" and m["payload"]["type"] == "player-connected", 15, "player-connected")
    player = ev["payload"]["data"]["player"]
    record("event-delivered", player == {"gameId": SID, "name": "Limon", "steamId": SID, "platformId": f"steam:{SID}"}, player=player)
    await asyncio.sleep(6)  # pings keep coming and stay unanswered
    pending_held = outbox_pending()
    record("event-unconfirmed-while-pongs-withheld", pending_held == 1 and c1.held, pending=pending_held, heldPongs=len(c1.held))
    released = await c1.release_pongs()
    ok = await wait_until(lambda: outbox_pending() == 0, 8)
    record("event-confirmed-by-later-pong", ok, released=released, pending=outbox_pending())

    # TCP abort with an unconfirmed event: it must go out again on the next connection
    c1.hold_pongs = True
    append_log(LEAVE)
    await c1.expect(lambda m: m.get("type") == "gameEvent" and m["payload"]["type"] == "player-disconnected", 15, "player-disconnected")
    log("server_close", conn=c1.n, kind="tcp-abort")
    c1.ws.transport.abort()
    c2 = await next_conn(1, 30)
    await asyncio.wait_for(c2.identified.wait(), 30)
    record("reconnect-after-abort", True)
    ev2 = await c2.expect(lambda m: m.get("type") == "gameEvent" and m["payload"]["type"] == "player-disconnected", 15, "resent player-disconnected")
    record("unconfirmed-event-resent", ev2["payload"]["data"]["player"]["gameId"] == SID)
    ok = await wait_until(lambda: outbox_pending() == 0, 10)
    record("resent-event-confirmed", ok, pending=outbox_pending())
    r = await c2.request("testReachability", {})
    record("request-after-reconnect", (r.get("payload") or {}).get("connectable") is True)

    # clean server close (Takaro restart): reconnect again
    log("server_close", conn=c2.n, kind="clean-1012")
    await c2.ws.close(1012, "fake takaro restart")
    c3 = await next_conn(2, 30)
    await asyncio.wait_for(c3.identified.wait(), 30)
    record("reconnect-after-clean-close", True)
    await c3.send({"type": "ping"})
    await c3.expect(lambda m: m.get("type") == "pong", 5, "pong on conn 3")
    record("ping-pong-after-reconnect", True)

    # dead link: Takaro goes silent (pongs withheld, no frames; Wine still auto-answers protocol pings, which
    # WinHTTP hides from the connector). The connector must give up after its 20 s idle window and reconnect.
    c3.hold_pongs = True
    silent_since = time.monotonic()
    c4 = await next_conn(3, 45)
    waited = round(time.monotonic() - silent_since, 1)
    record("dead-link-detected", 19 <= waited <= 35, seconds=waited)
    await asyncio.wait_for(c4.identified.wait(), 30)
    r = await c4.request("testReachability", {})
    record("request-after-dead-link", (r.get("payload") or {}).get("connectable") is True)


async def main():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.environ.get("FAKE_CERT", "/certs/fake-takaro.pem"), os.environ.get("FAKE_KEY", "/certs/fake-takaro.key"))
    log("listen", port=PORT, scenario=SCENARIO, wsPingS=WS_PING_S, websockets=websockets.__version__, python=sys.version.split()[0])
    async with websockets.serve(handler, "0.0.0.0", PORT, ssl=ctx, ping_interval=None, max_size=2**20):
        if SCENARIO in ("count", "accept"):
            await asyncio.Future()
        try:
            await asyncio.wait_for(scenario_native(), float(os.environ.get("FAKE_TIMEOUT_S", "240")))
        except (AssertionError, asyncio.TimeoutError) as err:
            record("scenario", False, error=str(err) or type(err).__name__)
        ok = bool(results) and all(results.values())
        log("result", ok=ok, passed=sum(results.values()), total=len(results), failed=[k for k, v in results.items() if not v])
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
