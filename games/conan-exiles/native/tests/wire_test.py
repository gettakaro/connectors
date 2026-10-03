#!/usr/bin/env python3
"""Wire tests: the production Takaro half (tests/build/harness, same sources as the library)
against tests/fake_takaro.py over real TLS WebSockets.

Covers the connector checklist's WIRE items (ping/pong, requestId correlation, [] / {} / JSON-string
/ null args, flat vs nested player, reconnect backoff, unknown action) plus identify, events,
durable outbox replay across a forced 20 s outage and across a process kill, identify rejection,
and the unknown-build refusal mode.

Usage: wire_test.py <harness binary>   (OUTAGE_SECONDS=20 by default)
"""
import json
import os
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fake_takaro import FakeTakaro, events_of, make_cert  # noqa: E402

IDENTITY = 'identity-SECRET-7f3a91c2'
REGISTRATION = 'registration-SECRET-0b44d1e9'
OUTAGE = float(os.environ.get('OUTAGE_SECONDS', '20'))
RESULTS = []


def check(cond, label, detail=''):
    RESULTS.append((bool(cond), label))
    print(('PASS ' if cond else 'FAIL ') + label + ('' if cond or not detail else f'  -- {detail}'), flush=True)
    if not cond:
        raise AssertionError(label + (': ' + str(detail) if detail else ''))


class Harness:
    def __init__(self, binary, fake, cert, state, mode='ready'):
        env = dict(os.environ, TAKARO_IDENTITY_TOKEN=IDENTITY, TAKARO_REGISTRATION_TOKEN=REGISTRATION,
                   TAKARO_SERVER_NAME='Wire Test Conan', TAKARO_RECONNECT_BASE_MS='500',
                   TAKARO_RECONNECT_MAX_MS='4000', TAKARO_ACTION_TIMEOUT_MS='5000')
        self.proc = subprocess.Popen([binary, f'wss://localhost:{fake.port}/', str(cert), str(state), mode],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, bufsize=1, env=env)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        line = self.lines.get(timeout=10)
        assert line == 'READY', line

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip('\n'))

    def send(self, line):
        self.proc.stdin.write(line + '\n')
        self.proc.stdin.flush()

    def emit(self, etype, data):
        self.send(f'emit {etype} {json.dumps(data)}')

    def expect(self, prefix, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=0.1)
            except queue.Empty:
                continue
            if line.startswith(prefix):
                return line[len(prefix):].strip()
        raise AssertionError(f'harness printed no {prefix!r}')

    def health(self):
        self.send('health')
        return json.loads(self.expect('HEALTH'))

    def quit(self):
        self.send('quit')
        self.expect('BYE', timeout=15)
        self.proc.wait(timeout=10)

    def kill(self):
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait(timeout=10)


def eventually(fn, timeout=10, what='condition'):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = fn()
        if last:
            return last
        time.sleep(0.1)
    raise AssertionError(f'{what} did not happen within {timeout}s (last={last!r})')


def chat(n):
    return {'msg': f'event-{n}', 'channel': 'global',
            'player': {'gameId': '76561198000000001', 'name': 'Tester', 'steamId': '76561198000000001',
                       'platformId': 'steam:76561198000000001'}}


def msgs(log, conn=None):
    return [e['data'].get('msg') for e in events_of(log, conn)]


def wait_identified(fake, h, conn_count):
    eventually(lambda: len([c for c in fake.conns]) >= conn_count, what=f'connection #{conn_count}')
    eventually(lambda: h.health()['identified'] and h.health()['connection']['identifies'] >= 1, what='identify')


def basics(binary, cert, key, root):
    fake = FakeTakaro(key, cert)
    h = Harness(binary, fake, cert, root / 'basics')
    try:
        _, ident = fake.next_frame(lambda v: isinstance(v, dict) and v.get('type') == 'identify', what='identify')
        p = ident['payload']
        check(p.get('identityToken') == IDENTITY and p.get('registrationToken') == REGISTRATION
              and p.get('name') == 'Wire Test Conan', 'identify carries both tokens and the server name')
        wait_identified(fake, h, 1)
        check(fake.identifies == 1, 'exactly one identify on the first connection')
        check(not fake.errors, 'no WebSocket subprotocol, masked client frames', fake.errors)

        conn = fake.current()
        conn.send_json({'type': 'ping'})
        fake.next_frame(lambda v: v == {'type': 'pong'}, what='pong')
        check(True, 'WIRE-1 Takaro application ping is answered with {"type":"pong"}')
        eventually(lambda: fake.ws_pings >= 1, timeout=8, what='WebSocket ping')
        check(fake.ws_pings >= 1, 'transport heartbeat: WebSocket pings reach Takaro')

        r = fake.response(fake.request('testReachability', {}))
        check(r.get('payload') == {'connectable': True, 'reason': None}, 'testReachability connectable', r)
        r = fake.response(fake.request('getMapInfo', '{}'))
        check(r.get('payload', {}).get('enabled') is False, 'getMapInfo schema fallback', r)
        r = fake.response(fake.request('getMapTile', {'x': 0, 'y': 0, 'z': 0}))
        check('error' in r and 'not supported' in r['error'], 'getMapTile structured error', r)
        r = fake.response(fake.request('doSomethingElse', {}))
        check('unknown action' in r.get('error', ''), "WIRE-6 unknown action answers an error", r)
        for action in ('getPlayers',):
            r = fake.response(fake.request(action, {'gameId': '76561198000000001', 'command': 'help'}))
            check('not implemented by the native Conan connector yet' in r.get('error', ''),
                  f'pending action {action} answers a structured not-ported error (no RCON)', r)
        for action in ('kickPlayer', 'executeConsoleCommand', 'shutdown'):
            # the harness has no game behind the adapter: native actions answer a structured error
            r = fake.response(fake.request(action, {'gameId': '76561198000000001', 'command': 'help'}))
            check(action in r.get('error', ''), f'{action} without a game answers a structured error (no RCON)', r)

        # WIRE-3 / WIRE-4: every args shape Takaro and modules send
        shapes = [
            ('object args, global', {'message': 'hello all'}, ('hello all', '', 'Takaro')),
            ('JSON-string args, nested opts.recipient.gameId',
             json.dumps({'message': 'to you', 'opts': {'recipient': {'gameId': '76561198000000001'}}}),
             ('to you', '76561198000000001', 'Takaro')),
            ('flat player gameId + senderNameOverride',
             {'message': 'flat', 'gameId': '76561198000000002', 'opts': {'senderNameOverride': 'Discord Bob'}},
             ('flat', '76561198000000002', 'Discord Bob')),
            ('nested player + platformId steam: prefix',
             {'message': 'nested', 'player': {'platformId': 'steam:76561198000000003'}},
             ('nested', '76561198000000003', 'Takaro')),
            ('explicit JSON null opts', {'message': 'nulls', 'opts': None, 'recipient': None},
             ('nulls', '', 'Takaro')),
        ]
        for label, args, want in shapes:
            r = fake.response(fake.request('sendMessage', args))
            check(r.get('payload') == {} and 'error' not in r, f'sendMessage ok: {label}', r)
            got = json.loads(h.expect('CHAT'))
            check((got['message'], got['recipient'], got['sender']) == want, f'sendMessage routed: {label}', got)
        for label, args in (('[] args', []), ('null args', None), ('"" args', ''), ('{} args', {})):
            r = fake.response(fake.request('sendMessage', args))
            check('non-empty message' in r.get('error', ''), f'WIRE-3 sendMessage with {label} is refused', r)
        r = fake.response(fake.request('sendMessage', {'message': 'please FAIL'}))
        check('No online Conan players' in r.get('error', ''), 'a failed send is answered with its error', r)

        # WIRE-2: correlation under concurrency
        ids = [fake.request('testReachability', {}, request_id=f'c-{i}') for i in range(20)]
        got = {fake.response(i)['requestId'] for i in ids}
        check(got == set(ids), 'WIRE-2 20 concurrent requests answered by requestId')
        rid = fake.request('testReachability', {}, request_id='dup-1')
        fake.response(rid)
        fake.request('testReachability', {}, request_id='dup-1')
        r = fake.response('dup-1')
        check(r.get('error') == 'duplicate requestId', 'a reused requestId is refused', r)
        conn.send_text('not json at all')
        r = fake.response(fake.request('testReachability', {}))
        check(r.get('payload', {}).get('connectable') is True, 'a malformed frame is ignored, the link stays up')

        # events
        for n in range(1, 4):
            h.emit('chat-message', chat(n))
        h.emit('not-a-takaro-event', {'msg': 'x'})
        eventually(lambda: msgs(fake.log) == ['event-1', 'event-2', 'event-3'], what='3 events')
        ev = events_of(fake.log)[0]
        check(ev['type'] == 'chat-message' and ev['data']['player']['gameId'] == '76561198000000001',
              'events arrive as gameEvent frames with the payload intact', ev)
        eventually(lambda: h.health()['outbox']['confirmedTotal'] == 3, what='confirmation')
        hl = h.health()
        check(hl['outbox']['pending'] == 0 and hl['outbox']['rejected'] == 1,
              'events confirmed by a later pong; an unknown event type is rejected', hl['outbox'])
        text = json.dumps(hl) + (root / 'basics' / 'harness.log').read_text()
        h.quit()
        text += (root / 'basics' / 'harness.log').read_text() + (root / 'basics' / 'event-outbox.json').read_text()
        check(IDENTITY not in text and REGISTRATION not in text, 'no token value in health, log or state files')
    finally:
        if h.proc.poll() is None:
            h.kill()
        fake.close()


def outage(binary, cert, key, root):
    fake = FakeTakaro(key, cert)
    h = Harness(binary, fake, cert, root / 'outage')
    try:
        wait_identified(fake, h, 1)
        for n in range(1, 4):
            h.emit('chat-message', chat(n))
        eventually(lambda: h.health()['outbox']['confirmedTotal'] == 3 and msgs(fake.log) == ['event-1', 'event-2',
                                                                                             'event-3'],
                   what='events 1-3 confirmed')
        fake.pongs = False  # Takaro stops confirming: events 4-5 are written but unconfirmed
        h.emit('chat-message', chat(4))
        h.emit('chat-message', chat(5))
        eventually(lambda: msgs(fake.log)[-2:] == ['event-4', 'event-5'], what='events 4-5 on the wire')
        first_conns = len(fake.conns)
        t0 = time.monotonic()
        fake.outage(OUTAGE)
        print(f'... forced Takaro outage for {OUTAGE:.0f}s', flush=True)
        time.sleep(1)
        h.emit('chat-message', chat(6))
        time.sleep(2)
        hl = h.health()
        check(hl['state'] == 'connecting' and hl['outbox']['pending'] == 3,
              'during the outage the bridge reconnects and keeps 3 unconfirmed events',
              {'state': hl['state'], 'pending': hl['outbox']['pending']})
        fake.pongs = True
        eventually(lambda: len(fake.conns) > first_conns and fake.conns[-1].alive, timeout=OUTAGE + 15,
                   what='reconnect after the outage')
        back = time.monotonic() - t0
        check(back < OUTAGE + 6, f'reconnected {back - OUTAGE:.1f}s after Takaro came back (backoff capped at 4 s)')
        new = fake.conns[-1].index
        eventually(lambda: msgs(fake.log, new) == ['event-4', 'event-5', 'event-6'], timeout=10,
                   what='replay on the new connection')
        identifies_new = [v for _, i, v in fake.log if i == new and isinstance(v, dict) and v.get('type') == 'identify']
        check(len(identifies_new) == 1, 'exactly one identify on the new connection')
        first_event = next(t for t, i, v in fake.log if i == new and isinstance(v, dict) and v.get('type') == 'gameEvent')
        ident_t = next(t for t, i, v in fake.log if i == new and isinstance(v, dict) and v.get('type') == 'identify')
        check(ident_t < first_event, 'replay starts only after identify')
        check(msgs(fake.log, new) == ['event-4', 'event-5', 'event-6'],
              'outbox replay: unconfirmed 4, 5 and the outage-time 6 re-sent in order; confirmed 1-3 are not')
        eventually(lambda: h.health()['outbox']['confirmedTotal'] == 6, what='replayed events confirmed')
        check(h.health()['outbox']['pending'] == 0, 'replayed events confirmed; the outbox is empty')
        r = fake.response(fake.request('testReachability', {}))
        check(r.get('payload', {}).get('connectable') is True, 'requests work again after the outage')
        check(h.health()['outbox']['losses'] == 0, 'no event lost')
        h.quit()
    finally:
        if h.proc.poll() is None:
            h.kill()
        fake.close()


def restart(binary, cert, key, root):
    fake = FakeTakaro(key, cert)
    state = root / 'restart'
    h = Harness(binary, fake, cert, state)
    try:
        wait_identified(fake, h, 1)
        fake.pongs = False
        h.emit('chat-message', chat(7))
        h.emit('player-death', {'player': {'gameId': '76561198000000001', 'name': 'Tester'}})
        eventually(lambda: [e['type'] for e in events_of(fake.log)] == ['chat-message', 'player-death'],
                   what='events on the wire')
        time.sleep(0.5)  # the outbox write is batched (100 ms when small)
        h.kill()
        check(True, 'connector killed with 2 unconfirmed events (SIGKILL, no clean shutdown)')
        conns = len(fake.conns)
        fake.pongs = True
        h = Harness(binary, fake, cert, state)
        eventually(lambda: len(fake.conns) > conns, what='new process connects')
        new = fake.conns[-1].index
        eventually(lambda: [e['type'] for e in events_of(fake.log, new)] == ['chat-message', 'player-death'],
                   what='replay from disk')
        check(msgs(fake.log, new)[0] == 'event-7', 'durable outbox: a new process replays the unconfirmed events')
        eventually(lambda: h.health()['outbox']['confirmedTotal'] == 2, what='confirmed after restart')
        check(True, 'replayed events confirmed and removed from the outbox')
        h.quit()
    finally:
        if h.proc.poll() is None:
            h.kill()
        fake.close()


def rejected(binary, cert, key, root):
    fake = FakeTakaro(key, cert)
    fake.reject_identify = True
    h = Harness(binary, fake, cert, root / 'rejected')
    try:
        eventually(lambda: fake.identifies >= 3, timeout=15, what='identify retries')
        times = [t for t, _, v in fake.log if isinstance(v, dict) and v.get('type') == 'identify']
        gaps = [b - a for a, b in zip(times, times[1:])]
        check(all(g >= 0.4 for g in gaps) and gaps[-1] >= gaps[0], 'a rejected identify is retried with backoff',
              [round(g, 2) for g in gaps])
        hl = h.health()
        check('bad token' in hl['lastIdentifyError'] and not hl['identified'], 'identify error surfaced in health')
        fake.reject_identify = False
        eventually(lambda: h.health()['identified'], timeout=15, what='identify after the fix')
        check(True, 'identifies once Takaro accepts it')
        h.quit()
    finally:
        if h.proc.poll() is None:
            h.kill()
        fake.close()


def refused(binary, cert, key, root):
    fake = FakeTakaro(key, cert)
    h = Harness(binary, fake, cert, root / 'refused', mode='refused')
    try:
        wait_identified(fake, h, 1)
        eventually(lambda: events_of(fake.log), what='critical notice')
        notices = events_of(fake.log)
        check(len(notices) == 1 and notices[0]['type'] == 'log' and 'CRITICAL' in notices[0]['data']['msg'],
              'unknown build: one critical notice (log event) after identify', notices)
        r = fake.response(fake.request('testReachability', {}))
        check(r.get('payload', {}).get('connectable') is False and 'refused' in r['payload'].get('reason', ''),
              'unknown build: testReachability connectable=false with the reason', r)
        for action in ('sendMessage', 'getPlayers', 'getMapInfo', 'shutdown'):
            r = fake.response(fake.request(action, {'message': 'x'}))
            check('refused' in r.get('error', ''), f'unknown build: {action} answers a structured error', r)
        time.sleep(1.5)
        check(len(events_of(fake.log)) == 1, 'the notice is sent once')
        h.quit()
    finally:
        if h.proc.poll() is None:
            h.kill()
        fake.close()


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='conan-wire-') as tmp:
        root = Path(tmp)
        key, cert = make_cert(root)
        only = os.environ.get('WIRE_ONLY')
        for name, fn in (('basics', basics), ('outage', outage), ('restart', restart), ('rejected', rejected),
                         ('refused', refused)):
            if only and name not in only.split(','):
                continue
            print(f'== {name}', flush=True)
            try:
                fn(binary, cert, key, root)
            except Exception:
                log = root / name / 'harness.log'
                if log.exists():
                    print(f'--- {log} (tail)\n' + '\n'.join(log.read_text().splitlines()[-40:]), flush=True)
                raise
    failed = [label for ok, label in RESULTS if not ok]
    print(f'wire tests: {len(RESULTS) - len(failed)}/{len(RESULTS)} passed', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
