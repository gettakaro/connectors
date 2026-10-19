#!/usr/bin/env python3
"""Production native bridge: real workers, TLS, protocol, ring and durable stores.
Only calls into the unavailable Dragonwilds engine are stubbed in the companion binary.
Every id is a fake fixture value.
"""
import base64
import hashlib
import json
import os
import re
import struct
from datetime import datetime, timedelta, timezone
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import native_bridge_adversarial as wire

BINARY = Path(__file__).resolve().parent / 'build/native_full_bridge'
COMMON_H = Path(__file__).resolve().parent.parent / 'src/common.h'
PONGS = threading.Event()
PONGS.set()
original_send = wire.send_frame


def controlled_send(sock, opcode, data):
    if opcode != 10 or PONGS.is_set():
        original_send(sock, opcode, data)


wire.send_frame = controlled_send


class Run(wire.Run):
    def __init__(self, cert, key, state, env=None, identify=True):
        self.peer = wire.Peer(cert, key)
        child_env = os.environ.copy()
        child_env.update(env or {})
        self.proc = subprocess.Popen(
            [str(BINARY), f'wss://localhost:{self.peer.port}/', str(cert), str(state)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1, env=child_env)
        self.lines = queue.Queue()
        threading.Thread(target=self.read_lines, daemon=True).start()
        try:
            line = self.lines.get(timeout=10)
            assert line == 'READY', line
            self.pending = []
            if identify:
                self.index = self.identify()
                eventually(lambda: self.health()['connection']['identified'])
        except Exception as error:
            self.proc.kill()
            self.proc.wait(timeout=5)
            self.peer.close()
            raise AssertionError(f'full bridge startup failed: {error}\n{self.proc.stderr.read()}') from error

    def event(self, nonce, timeout=8):
        return self.game_event(lambda value: value['payload']['data'].get('msg') == nonce, timeout, f'event {nonce}')

    def game_event(self, predicate, timeout=8, what='event'):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for pos, (index, opcode, data) in enumerate(self.pending):
                if opcode == 1:
                    value = json.loads(data)
                    if value.get('type') == 'gameEvent' and predicate(value):
                        self.pending.pop(pos)
                        return value
            try:
                self.pending.append(self.peer.frames.get(timeout=0.2))
            except queue.Empty:
                pass
        raise AssertionError(f'missing {what}')

    def crash(self):
        self.proc.kill()
        self.proc.wait(timeout=5)
        self.peer.close()


class V1Run(Run):
    identify_reply = {'gameServerId': 'test-server', 'protocolVersion': 1}


def protocol_v1(cert, key, state):
    run = V1Run(cert, key, state)
    try:
        offered = run.identify_payload
        assert offered['protocolVersions'] == [0, 1], offered
        names = [c['name'] for c in offered['capabilities']]
        assert len(names) == len(set(names)) and 'testReachability' not in names, names
        assert {'players.location', 'moderation.ban', 'events.player-connected', 'events.chat-message'} <= set(names), names
        # Game Profile: the game and connector identity, game.profile with the running version and icon.
        release = re.search(r'#define TAKARO_PLUGIN_VERSION "([^"]+)" // x-release-please-version',
                            COMMON_H.read_text()).group(1)
        assert offered['game'] == 'dragonwilds', offered.get('game')
        assert offered['connector'] == {'name': 'takaro-dragonwilds', 'version': release}, offered.get('connector')
        profile = offered['capabilities'][names.index('game.profile')]['parameters']
        assert profile['name'] == 'RuneScape: Dragonwilds' and profile['version'] == '1.0.1.1', profile
        icon = profile['icon']
        assert re.fullmatch(r'[0-9a-f]{12}', icon['revision']) and 0 < icon['size'] <= 128 * 1024, icon
        eventually(lambda: run.health()['protocolVersion'] == 1)
        run.request('v1-icon', 'getGameIcon', '{}')
        answer = run.response('v1-icon')
        assert 'error' not in answer or answer['error'] is None, answer
        payload = answer['payload']
        png = base64.b64decode(payload['data'], validate=True)
        assert payload['revision'] == icon['revision'], payload['revision']
        assert len(png) == icon['size'], (len(png), icon['size'])
        assert hashlib.sha256(png).hexdigest()[:12] == icon['revision']
        assert png[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', png[16:24]) == (256, 256)

        run.command('emit:v1-event', 'EMITTED')
        event = run.event('v1-event')
        stream, seq = event['payload']['streamId'], event['payload']['seq']
        assert stream.startswith('dragonwilds-') and isinstance(seq, int) and seq >= 1, event
        # A pong is not an acknowledgement on version 1: the event stays until eventAck.
        time.sleep(1.5)
        assert read(state / 'event-outbox.json')['pending'], 'version 1 event confirmed without eventAck'

        # Reconnect before the ack: the same event is resent with the same stream and seq.
        run.peer.close_session(run.index)
        run.index = run.identify()
        run.pending = []
        resent = run.event('v1-event')
        assert (resent['payload']['streamId'], resent['payload']['seq']) == (stream, seq), resent
        run.peer.send(run.index, {'type': 'eventAck', 'payload': {'streamId': 'someone-else', 'upTo': seq}})
        time.sleep(0.5)
        assert read(state / 'event-outbox.json')['pending'], 'ack for another stream confirmed our event'
        # An ack beyond what was sent confirms only what was sent, never a later event.
        run.peer.send(run.index, {'type': 'eventAck', 'payload': {'streamId': stream, 'upTo': seq + 1000}})
        eventually(lambda: not read(state / 'event-outbox.json')['pending'])
        run.command('emit:v1-later', 'EMITTED')
        later = run.event('v1-later')
        assert later['payload']['streamId'] == stream and later['payload']['seq'] > seq, later
        time.sleep(1)
        assert read(state / 'event-outbox.json')['pending'], 'oversized ack confirmed a later event'
        run.peer.send(run.index, {'type': 'eventAck', 'payload': {'streamId': stream, 'upTo': later['payload']['seq']}})
        eventually(lambda: not read(state / 'event-outbox.json')['pending'])

        run.request('v1-unknown', 'getMapTile', '{"dimension":"overworld","zoom":0,"x":0,"y":0}')
        error = run.response('v1-unknown')['error']
        assert error['code'] == 'unsupported' and error['message'], error
        run.request('v1-bad', 'giveItem', {'gameId': '0123456789abcdef0123456789abcdef'})
        error = run.response('v1-bad')['error']
        assert error == {'code': 'invalid_args', 'message': "giveItem requires 'item'"}, error
        print('production bridge protocol version 1 manifest, streamId/seq, eventAck and error codes: pass', flush=True)
    finally:
        run.close()


def protocol_v0_forced(cert, key, state):
    # TAKARO_PROTOCOL=0 keeps the plain version 0 identify, plain string errors and no seq.
    run = Run(cert, key, state, env={'TAKARO_PROTOCOL': '0'})
    try:
        assert 'protocolVersions' not in run.identify_payload and 'capabilities' not in run.identify_payload, \
            run.identify_payload
        assert run.identify_payload['game'] == 'dragonwilds', run.identify_payload
        assert run.identify_payload['connector']['name'] == 'takaro-dragonwilds', run.identify_payload
        assert run.health()['protocolVersion'] == 0
        run.command('emit:v0-event', 'EMITTED')
        event = run.event('v0-event')
        assert 'streamId' not in event['payload'] and 'seq' not in event['payload'], event
        run.request('v0-unknown', 'getMapTile', '{}')
        error = run.response('v0-unknown')['error']
        assert isinstance(error, str) and 'Unknown Takaro action' in error, error
        print('production bridge protocol version 0 fallback: pass', flush=True)
    finally:
        run.close()


def eventually(check, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError('condition did not become true')


def read(path):
    return json.loads(path.read_text())


def replay(cert, key, state):
    PONGS.clear()
    run = Run(cert, key, state)
    try:
        run.command('emit:unconfirmed-crash', 'EMITTED')
        event = run.event('unconfirmed-crash')
        snapshot = read(state / 'event-outbox.json')
        assert any(json.loads(row['frame']) == event for row in snapshot['pending'])
        old_boot = snapshot['scan']['bootId']
    finally:
        run.crash()
    run = Run(cert, key, state)
    try:
        assert run.event('unconfirmed-crash') == event
        eventually(lambda: read(state / 'event-outbox.json')['scan']['bootId'] != old_boot)
        assert read(state / 'event-outbox.json')['pending'], 'unconfirmed replay removed before pong'
        PONGS.set()
        eventually(lambda: not read(state / 'event-outbox.json')['pending'], timeout=8)
        print('production bridge durable admission, crash replay, boot change and pong confirmation: pass', flush=True)
    finally:
        PONGS.set()
        run.close()


def backlog_request_latency(cert, key, state):
    # A large unconfirmed backlog replayed after a restart must not delay Takaro requests past
    # its 10 s timeout (seen live with ~350 events: requests timed out for ~30 s after boot).
    PONGS.clear()
    run = Run(cert, key, state)
    try:
        run.command('emit-many:400', 'EMITTED')
        eventually(lambda: len(read(state / 'event-outbox.json')['pending']) >= 400, timeout=60)
    finally:
        run.crash()
    PONGS.set()
    run = Run(cert, key, state)
    try:
        run.command('rawlog-many:600', 'RAWLOGGED')
        started = time.monotonic()
        for i in range(20):
            run.request(f'backlog-reach-{i}', 'testReachability')
        for i in range(20):
            run.response(f'backlog-reach-{i}', timeout=max(0.1, 2 - (time.monotonic() - started)))
        elapsed = time.monotonic() - started
        eventually(lambda: not read(state / 'event-outbox.json')['pending'], timeout=60)
        print(f'production bridge 20 requests during 400-event replay plus 600 raw log lines answered in {elapsed:.2f}s: pass', flush=True)
    finally:
        run.close()


def directory_sync(cert, key, state):
    run = Run(cert, key, state)
    try:
        run.command('fail-dir-sync', 'SYNCFAILED')
        run.command('emit:visible-not-durable', 'EMITTED')
        eventually(lambda: any('visible-not-durable' in row['frame']
                              for row in read(state / 'event-outbox.json')['pending']))
        snapshot = read(state / 'event-outbox.json')
        eventually(lambda: run.health()['persistenceErrors'] > 0)
        try:
            run.event('visible-not-durable', timeout=1.5)
        except AssertionError as error:
            assert str(error) == 'missing event visible-not-durable'
        else:
            raise AssertionError('event sent before directory sync confirmed durability')
        assert read(state / 'event-outbox.json')['nextOutboxId'] == snapshot['nextOutboxId']
        run.command('restore-dir-sync', 'SYNCRESTORED')
        event = run.event('visible-not-durable')
        assert any(json.loads(row['frame']) == event for row in snapshot['pending'])
        eventually(lambda: not read(state / 'event-outbox.json')['pending'])
        print('production bridge visible rename waits for directory durability before send: pass', flush=True)
    finally:
        run.command('restore-dir-sync', 'SYNCRESTORED')
        run.close()


def mutations(cert, key, state):
    run = Run(cert, key, state)
    player = '0123456789abcdef0123456789abcdef'
    try:
        run.command('holdban', 'HOLDBAN')
        run.request('timed', 'banPlayer', {'gameId': 'Character alias', 'expiresAt': '2099-01-01T00:00:00.123Z'})
        eventually(lambda: (state / 'ban-intent.json').exists())
        assert read(state / 'ban-intent.json')['intents'][0]['gameId'] == player, 'journal was written before canonical resolution'
        run.command('release', 'RELEASED')
        assert run.response('timed')['payload'] == {}
        assert read(state / 'timed-bans.json')[0]['expiresAt'] == '2099-01-01T00:00:00.123Z'
        assert not (state / 'ban-intent.json').exists()
        run.request('permanent', 'banPlayer', {'gameId': player, 'expiresAt': None})
        assert run.response('permanent')['payload'] == {}
        assert read(state / 'timed-bans.json') == []
        assert read(state / 'bans.json')['bans'][0]['gameId'] == player
        run.request('unban', 'unbanPlayer', {'gameId': player})
        assert run.response('unban')['payload'] == {}
        assert read(state / 'bans.json')['bans'] == []
        print('production bridge serialized timed/permanent/unban effects and current state: pass', flush=True)
    finally:
        run.close()


def crash_write(cert, key, state):
    PONGS.clear()
    run = Run(cert, key, state)
    try:
        run.command('emit:already-durable', 'EMITTED')
        expected = run.event('already-durable')
        before = (state / 'event-outbox.json').read_bytes()
        run.command('arm-write', 'ARMED')
        run.command('emit:not-admitted-before-crash', 'EMITTED')
        eventually(lambda: (state / 'before-rename').exists())
        assert list(state.glob('event-outbox.json.tmp.*')), 'test did not reach a durable temporary write'
    finally:
        run.crash()
    assert (state / 'event-outbox.json').read_bytes() == before, 'partial write replaced last committed state'
    run = Run(cert, key, state)
    try:
        assert run.event('already-durable') == expected
        PONGS.set()
        eventually(lambda: not read(state / 'event-outbox.json')['pending'])
        print('production bridge crash after temp fsync keeps committed outbox and tolerates stale temporary: pass', flush=True)
    finally:
        PONGS.set()
        run.close()


def stale_effect(cert, key, state):
    run = Run(cert, key, state)
    try:
        # Block the real action worker, drop its connection, then let its result finish.
        run.command('hold', 'HELD')
        run.request('old-read', 'getPlayers', {})
        eventually(lambda: run.health()['queues']['actionBytes'] > 0)
        time.sleep(0.1)
        run.peer.close_session(run.index)
        run.index = run.identify()
        run.command('release', 'RELEASED')
        eventually(lambda: (state / 'known-players.json').exists() and
                   any(row['gameId'] == '0123456789abcdef0123456789abcdef'
                       for row in read(state / 'known-players.json')))
        assert read(state / 'known-players.json')[0]['gameId'] == '0123456789abcdef0123456789abcdef'
        run.no_response('old-read', seconds=0.5)
        run.request('new-read', 'getPlayer', {'gameId': '0123456789abcdef0123456789abcdef'})
        assert run.response('new-read')['payload']['gameId'] == '0123456789abcdef0123456789abcdef'
        print('production bridge stale response discarded while completion state is retained: pass', flush=True)
    finally:
        run.close()


def engine_crash_setup(cert, key, state, existing=False):
    player = '0123456789abcdef0123456789abcdef'
    run = Run(cert, key, state)
    try:
        if existing:
            run.request('old-permanent', 'banPlayer', {'gameId': player, 'reason': 'old permanent'})
            assert run.response('old-permanent')['payload'] == {}
        before = read(state / 'bans.json')['bans'] if (state / 'bans.json').exists() else []
        run.command('engine-only-ban', 'ENGINEONLY')
        run.request('engine-crash', 'banPlayer', {'gameId': player,
                    'expiresAt': '2099-01-01T00:00:00.123Z', 'reason': 'requested timed ban'})
        eventually(lambda: (state / 'engine-ban-written').exists())
        journal = read(state / 'ban-intent.json')
        assert journal['intents'], 'engine mutation began without durable intent'
        assert '2099-01-01T00:00:00.123Z' in json.dumps(journal), 'intent lost requested expiry'
        assert 'requested timed ban' in json.dumps(journal), 'intent lost requested reason'
        assert read(state / 'game-bans.fixture.json')[0]['gameId'] == player
        current = read(state / 'bans.json')['bans'] if (state / 'bans.json').exists() else []
        assert current == before, 'fixture did not isolate engine-before-plugin crash window'
        return before
    finally:
        run.crash()


def engine_before_plugin(cert, key, state):
    engine_crash_setup(cert, key, state)
    run = Run(cert, key, state)
    player = '0123456789abcdef0123456789abcdef'
    try:
        eventually(lambda: (state / 'timed-bans.json').exists() and
                   any(row['gameId'] == player and row['expiresAt'] == '2099-01-01T00:00:00.123Z'
                       for row in read(state / 'timed-bans.json')))
        eventually(lambda: not (state / 'ban-intent.json').exists())
        ban = read(state / 'bans.json')['bans'][0]
        assert ban['expiresAt'] == '2099-01-01T00:00:00.123Z'
        assert ban['reason'] == 'requested timed ban'
        run.request('engine-cleanup', 'unbanPlayer', {'gameId': player})
        assert run.response('engine-cleanup')['payload'] == {}
        print('production bridge engine-before-plugin crash recovers durable requested expiry and reason: pass', flush=True)
    finally:
        run.close()


def ambiguous_engine_ban(cert, key, state):
    before = engine_crash_setup(cert, key, state, existing=True)
    run = Run(cert, key, state)
    player = '0123456789abcdef0123456789abcdef'
    try:
        time.sleep(2.5)  # Recovery verification has a chance to run.
        assert read(state / 'bans.json')['bans'] == before, 'ambiguous recovery overwrote existing ban'
        assert (state / 'ban-intent.json').exists(), 'ambiguous recovery discarded requested expiry'
        health = run.health()
        assert health['banRecoveryPending'], health
        assert health.get('lastError'), 'ambiguous recovery was not reported explicitly'
        run.request('resolve-ambiguity', 'unbanPlayer', {'gameId': player})
        assert run.response('resolve-ambiguity')['payload'] == {}
        eventually(lambda: not (state / 'ban-intent.json').exists())
        assert read(state / 'bans.json')['bans'] == []
        assert read(state / 'timed-bans.json') == []
        print('production bridge ambiguous older ban retains intent and enforcement until explicit resolution: pass', flush=True)
    finally:
        run.close()


def newer_ban_after_crash(cert, key, state):
    engine_crash_setup(cert, key, state, existing=True)
    path = state / 'bans.json'
    current = read(path)
    current['bans'][0]['reason'] = 'newer permanent ban'
    current['bans'][0]['createdAt'] = '2098-01-01T00:00:00Z'
    current['bans'][0]['expiresAt'] = None
    path.write_text(json.dumps(current))
    run = Run(cert, key, state)
    try:
        eventually(lambda: not (state / 'ban-intent.json').exists())
        assert read(path) == current, 'recovery replaced a newer permanent ban with stale intent'
        assert read(state / 'timed-bans.json') == []
        run.request('newer-cleanup', 'unbanPlayer', {'gameId': '0123456789abcdef0123456789abcdef'})
        assert run.response('newer-cleanup')['payload'] == {}
        print('production bridge recovery preserves newer permanent ban over stale timed intent: pass', flush=True)
    finally:
        run.close()


def permanent_restart(cert, key, state):
    player = '0123456789abcdef0123456789abcdef'
    run = Run(cert, key, state)
    try:
        run.request('prior-timed', 'banPlayer', {'gameId': player,
                    'expiresAt': '2099-01-01T00:00:00Z', 'reason': 'prior timed'})
        assert run.response('prior-timed')['payload'] == {}
        old_timed = read(state / 'timed-bans.json')
        assert run.command('external-permanent', 'EXTERNAL ') == '200'
        assert read(state / 'bans.json')['bans'][0]['expiresAt'] is None
    finally:
        run.crash()
    # Model a crash before the bridge's derived compatibility mirror catches up.
    (state / 'timed-bans.json').write_text(json.dumps(old_timed))
    run = Run(cert, key, state)
    try:
        eventually(lambda: read(state / 'timed-bans.json') == [])
        current = read(state / 'bans.json')['bans'][0]
        assert current['expiresAt'] is None, 'native restart rehydrated a stale legacy expiry'
        assert current['reason'] == 'external newer permanent'
        run.request('restart-cleanup', 'unbanPlayer', {'gameId': player})
        assert run.response('restart-cleanup')['payload'] == {}
        print('production bridge native restart cannot reimport stale expiry over newer permanent ban: pass', flush=True)
    finally:
        run.close()


def timed_replacement(cert, key, state):
    player = '0123456789abcdef0123456789abcdef'
    run = Run(cert, key, state)
    try:
        run.request('first-timed', 'banPlayer', {'gameId': player, 'expiresAt': '2099-01-01T00:00:00Z'})
        assert run.response('first-timed')['payload'] == {}
        expiry = (datetime.now(timezone.utc) + timedelta(seconds=6)).isoformat()
        assert run.command('external-timed:' + expiry, 'EXTERNAL ') == '200'
        eventually(lambda: any(row['gameId'] == player and row.get('reason') == 'external newer timed'
                               for row in read(state / 'timed-bans.json')))
        eventually(lambda: read(state / 'bans.json')['bans'] == [], timeout=10)
        assert read(state / 'game-bans.fixture.json') == []
        eventually(lambda: read(state / 'timed-bans.json') == [])
        print('production bridge external timed replacement updates scheduler and expires without restart: pass', flush=True)
    finally:
        run.close()


def failed_ban_write(cert, key, state):
    run = Run(cert, key, state)
    timed = state / 'timed-bans.json'
    player = '0123456789abcdef0123456789abcdef'
    try:
        # Fail the derived timed-ban write after the game ban has persisted.
        timed.unlink(missing_ok=True)
        timed.mkdir()
        run.request('fault-ban', 'banPlayer', {'gameId': player, 'expiresAt': '2099-01-01T00:00:00Z'})
        assert run.response('fault-ban').get('error')
        assert read(state / 'bans.json')['bans'][0]['gameId'] == player
        assert (state / 'ban-intent.json').exists(), 'failed state transition lost recovery journal'
        eventually(lambda: run.health()['persistenceErrors'] > 0)
    finally:
        run.crash()
        timed.rmdir()
    run = Run(cert, key, state)
    try:
        eventually(lambda: timed.is_file() and
                   any(row['gameId'] == player for row in read(timed)))
        eventually(lambda: not (state / 'ban-intent.json').exists())
        run.request('cleanup-ban', 'unbanPlayer', {'gameId': player})
        assert run.response('cleanup-ban')['payload'] == {}
        print('production bridge failed timed-state write retains current ban and recovers journal: pass', flush=True)
    finally:
        run.close()


def late_ban(cert, key, state):
    run = Run(cert, key, state)
    player = '0123456789abcdef0123456789abcdef'
    try:
        run.command('lateban', 'LATEBAN')
        run.request('late', 'banPlayer', {'gameId': player, 'expiresAt': '2099-01-01T00:00:00Z'})
        assert 'timed out' in run.response('late')['error']
        assert (state / 'ban-intent.json').exists(), 'timed-out started job lost its intent'
        eventually(lambda: (state / 'timed-bans.json').is_file() and
                   any(row['gameId'] == player for row in read(state / 'timed-bans.json')))
        eventually(lambda: not (state / 'ban-intent.json').exists())
        run.request('late-cleanup', 'unbanPlayer', {'gameId': player})
        assert run.response('late-cleanup')['payload'] == {}
        print('production bridge retains timeout intent until late ban completes and expiry persists: pass', flush=True)
    finally:
        run.close()


def partial_unban(cert, key, state):
    player = '0123456789abcdef0123456789abcdef'
    run = Run(cert, key, state)
    try:
        run.request('partial-seed', 'banPlayer', {'gameId': player, 'expiresAt': '2099-01-01T00:00:00Z'})
        assert run.response('partial-seed')['payload'] == {}
        run.command('fail-unban', 'FAILUNBAN')
        run.request('partial-remove', 'unbanPlayer', {'gameId': player})
        assert run.response('partial-remove').get('error')
        assert read(state / 'bans.json')['bans'] == []
        assert read(state / 'game-bans.fixture.json')[0]['gameId'] == player
        assert read(state / 'timed-bans.json')[0]['gameId'] == player, 'partial unban silently lost expiry'
        time.sleep(2.5)
        assert (state / 'ban-intent.json').exists(), 'plugin-only absence cleared unresolved engine ban'
    finally:
        run.crash()
    run = Run(cert, key, state)
    try:
        run.request('partial-list', 'listBans', {})
        ban = run.response('partial-list')['payload'][0]
        assert ban['expiresAt'] == '2099-01-01T00:00:00.000Z'
        run.request('partial-retry', 'unbanPlayer', {'gameId': player})
        assert run.response('partial-retry')['payload'] == {}
        eventually(lambda: not (state / 'ban-intent.json').exists())
        assert read(state / 'timed-bans.json') == []
        assert read(state / 'game-bans.fixture.json') == []
        print('production bridge partial engine-unban failure preserves expiry across restart until verified removal: pass', flush=True)
    finally:
        run.close()


def shutdown(cert, key, state):
    PONGS.clear()
    run = Run(cert, key, state)
    try:
        run.request('shutdown', 'shutdown', {})
        assert run.response('shutdown')['payload'] == {}
        assert run.command('shutdowns', 'SHUTDOWNS ') == '0', 'game shutdown ran before acknowledgment attempt'
        eventually(lambda: run.command('shutdowns', 'SHUTDOWNS ') == '1', timeout=4)
        print('production bridge shutdown response precedes bounded pong wait and game shutdown: pass', flush=True)
    finally:
        PONGS.set()
        run.close()


def late_permanent(cert, key, state):
    run = Run(cert, key, state)
    player = '0123456789abcdef0123456789abcdef'
    try:
        expiry = (datetime.now(timezone.utc) + timedelta(seconds=1)).isoformat()
        run.request('short-timed', 'banPlayer', {'gameId': player, 'expiresAt': expiry})
        assert run.response('short-timed')['payload'] == {}
        run.command('lateban', 'LATEBAN')
        run.request('late-permanent', 'banPlayer', {'gameId': player, 'expiresAt': None})
        assert 'timed out' in run.response('late-permanent')['error']
        eventually(lambda: not (state / 'ban-intent.json').exists())
        time.sleep(1.2)  # Let another expiry sweep run after the late job completes.
        assert read(state / 'timed-bans.json') == []
        bans = read(state / 'bans.json')['bans']
        assert len(bans) == 1 and bans[0]['gameId'] == player, 'old expiry lifted a new permanent ban'
        run.request('permanent-cleanup', 'unbanPlayer', {'gameId': player})
        assert run.response('permanent-cleanup')['payload'] == {}
        print('production bridge old expiry cannot lift a late replacement permanent ban: pass', flush=True)
    finally:
        run.close()


PLAYER = '0123456789abcdef0123456789abcdef'


def outage(cert, key, state):
    # F10 + F20: an event written into a link that dies before Takaro's pong confirms it is not
    # lost, events produced while disconnected are kept, and nothing is sent on the new socket
    # before identifyResponse. Delivery resumes in order after it.
    PONGS.clear()
    run = Run(cert, key, state)
    try:
        run.command('emit:outage-start', 'EMITTED')
        first = run.event('outage-start')
        assert read(state / 'event-outbox.json')['pending'], 'outage-start confirmed without a pong'
        old = run.index
        run.peer.close_session(old)
        eventually(lambda: not run.health()['connection']['identified'])
        run.command('emit:during-outage', 'EMITTED')
        index = run.peer.connections.get(timeout=15)
        i, opcode, data = run.peer.frames.get(timeout=5)
        assert i == index and json.loads(data)['type'] == 'identify', data
        time.sleep(0.7)
        early = []
        while True:
            try:
                early.append(run.peer.frames.get_nowait())
            except queue.Empty:
                break
        assert not [e for e in early if e[0] == index and e[1] == 1 and json.loads(e[2]).get('type') == 'gameEvent'], \
            'gameEvent sent before identifyResponse'
        run.pending = [e for e in early]
        PONGS.set()
        run.peer.send(index, {'type': 'identifyResponse', 'payload': {}})
        run.index = index
        again = run.event('outage-start')
        assert again == first
        run.event('during-outage')
        eventually(lambda: not read(state / 'event-outbox.json')['pending'])
        print('production bridge outage start replayed, outage events kept, nothing before identify (F10/F20): pass', flush=True)
    finally:
        PONGS.set()
        run.close()


def reconcile_after_crash(cert, key, state):
    run = Run(cert, key, state)
    try:
        run.command('emit-join:' + PLAYER, 'EMITTED')
        joined = run.game_event(lambda v: v['payload']['type'] == 'player-connected', what='player-connected')
        assert joined['payload']['data']['player'] == {
            'gameId': PLAYER, 'name': 'Limon', 'epicOnlineServicesId': PLAYER, 'platformId': 'epic:' + PLAYER}, joined
        eventually(lambda: any(row['gameId'] == PLAYER for row in read(state / 'online-players.json')))
        eventually(lambda: not read(state / 'event-outbox.json')['pending'])
    finally:
        run.crash()
    # The server process died with the player connected: the new process lists nobody.
    run = Run(cert, key, state, env={'FIXTURE_PLAYERS': 'none'})
    try:
        left = run.game_event(lambda v: v['payload']['type'] == 'player-disconnected', timeout=10, what='player-disconnected')
        assert left['payload']['data']['player']['gameId'] == PLAYER, left
        eventually(lambda: read(state / 'online-players.json') == [])
        print('production bridge crash with a player online reconciles to player-disconnected: pass', flush=True)
    finally:
        run.close()


def log_events(cert, key, state):
    run = Run(cert, key, state)
    try:
        run.command('rawlog:[2026.09.16-15.38.52:100][  0]LogRedpointEOS: Verbose: EOS_Platform_Tick', 'RAWLOGGED')
        run.command('rawlog:LogNet: Login request: ?p=c3dvcmRmaXNo?pf=PC?cpx=1?Name=Limon userId: RedpointEOS:' + PLAYER, 'RAWLOGGED')
        run.command('rawlog:LogDominion: WorldPassword=swordfish AdminPassword=hunter2', 'RAWLOGGED')
        login = run.game_event(lambda v: v['payload']['type'] == 'log' and 'Login request' in v['payload']['data']['msg'])
        assert login['payload']['data']['msg'] == ('LogNet: Login request: ?p=[redacted]?pf=PC?cpx=1?Name=Limon userId: '
                                                    'RedpointEOS:' + PLAYER), login
        assert set(login['payload']['data']) == {'msg', 'timestamp'}, login
        password = run.game_event(lambda v: v['payload']['type'] == 'log' and 'WorldPassword' in v['payload']['data']['msg'])
        assert password['payload']['data']['msg'] == 'LogDominion: WorldPassword=[redacted] AdminPassword=[redacted]'
        eventually(lambda: not read(state / 'event-outbox.json')['pending'])
        outbox = (state / 'event-outbox.json').read_text()
        assert 'swordfish' not in outbox and 'c3dvcmRmaXNo' not in outbox and 'EOS_Platform_Tick' not in outbox
        print('production bridge log events redacted (WorldPassword, ?p=) and UE/EOS noise dropped: pass', flush=True)
    finally:
        run.close()


def whitelist(cert, key, state):
    run = Run(cert, key, state)
    try:
        run.command('emit-kill:Goblin', 'EMITTED')
        kill = run.game_event(lambda v: v['payload']['type'] == 'entity-killed', what='entity-killed')
        data = kill['payload']['data']
        assert set(data) == {'player', 'entity', 'weapon', 'timestamp'}, data
        assert data['entity'] == 'Goblin' and data['weapon'] == 'Rune Sword', data
        assert set(data['player']) == {'gameId', 'name', 'epicOnlineServicesId', 'platformId'}, data
        print('production bridge entity-killed leaves with exactly player/entity/weapon: pass', flush=True)
    finally:
        run.close()


def args_shapes(cert, key, state):
    run = Run(cert, key, state)
    try:
        run.request('as-string', 'getPlayer', json.dumps({'gameId': PLAYER}))
        assert run.response('as-string')['payload']['gameId'] == PLAYER
        run.request('nested', 'getPlayer', {'player': {'gameId': PLAYER, 'id': 'pog', 'dimension': None}})
        assert run.response('nested')['payload']['gameId'] == PLAYER
        run.request('array', 'getPlayers', [])
        assert run.response('array')['payload'][0]['gameId'] == PLAYER
        run.request('null-args', 'getPlayers', None)
        assert isinstance(run.response('null-args')['payload'], list)
        run.request('no-id', 'getPlayer', [])
        assert 'player identifier' in run.response('no-id')['error']
        run.request('offline', 'getPlayer', {'gameId': 'deadbeefdeadbeefdeadbeefdeadbeef'})
        assert run.response('offline')['payload'] == {
            'gameId': 'deadbeefdeadbeefdeadbeefdeadbeef', 'name': 'deadbeefdeadbeefdeadbeefdeadbeef',
            'epicOnlineServicesId': 'deadbeefdeadbeefdeadbeefdeadbeef',
            'platformId': 'epic:deadbeefdeadbeefdeadbeefdeadbeef', 'online': False}
        run.request('bans', 'listBans', None)
        assert run.response('bans')['payload'] == []
        print('production bridge args as string/[]/null/nested pog row, offline getPlayer, bare listBans: pass', flush=True)
    finally:
        run.close()


def timed_expiry_restart(cert, key, state):
    run = Run(cert, key, state)
    try:
        expiry = (datetime.now(timezone.utc) + timedelta(seconds=4)).isoformat()
        run.request('short', 'banPlayer', {'player': {'gameId': PLAYER}, 'reason': 'cooldown', 'expiresAt': expiry})
        assert run.response('short')['payload'] == {}
        run.request('list', 'listBans', {})
        listed = run.response('list')['payload']
        assert listed[0]['expiresAt'] and listed[0]['reason'] == 'cooldown', listed
    finally:
        run.crash()
    time.sleep(4.5)
    run = Run(cert, key, state)
    try:
        eventually(lambda: read(state / 'bans.json')['bans'] == [], timeout=10)
        assert read(state / 'game-bans.fixture.json') == []
        eventually(lambda: read(state / 'timed-bans.json') == [])
        print('production bridge timed ban lifted by the connector after a restart: pass', flush=True)
    finally:
        run.close()


def legacy_import(cert, key, state):
    other = 'aa11bb22cc33dd44ee55ff6600112233'
    (state / 'event-cursor.json').write_text(json.dumps({'seq': 452, 'bootId': '0f0f0f0f0f0f0f0f'}))
    (state / 'known-players.json').write_text(json.dumps([
        {'gameId': other, 'name': 'takarotester', 'epicOnlineServicesId': other, 'platformId': 'epic:' + other}]))
    (state / 'online-players.json').write_text('[]')
    (state / 'timed-bans.json').write_text(json.dumps([{'gameId': other, 'expiresAt': '2099-01-01T00:00:00.000Z', 'reason': 'cooldown'}]))
    (state / 'bans.json').write_text(json.dumps({'version': 1, 'bans': [
        {'gameId': other, 'name': 'takarotester', 'reason': '', 'createdAt': '2026-09-16T12:00:00.000Z', 'expiresAt': None}]}))
    run = Run(cert, key, state)
    try:
        assert read(state / 'bans.json')['bans'][0]['expiresAt'] == '2099-01-01T00:00:00.000Z'
        assert read(state / 'event-outbox.json')['legacyBanMigrationDone'] is True
        run.request('known', 'getPlayer', {'gameId': other})
        assert run.response('known')['payload'] == {
            'gameId': other, 'name': 'takarotester', 'epicOnlineServicesId': other, 'platformId': 'epic:' + other,
            'online': False}
        run.request('list', 'listBans', {})
        listed = run.response('list')['payload']
        # (the fixture's game ban list carries no names; the real one reports the KnownPlayerList name)
        assert len(listed) == 1 and listed[0]['player']['gameId'] == other and listed[0]['reason'] == 'cooldown' and \
            listed[0]['expiresAt'] == '2099-01-01T00:00:00.000Z' and set(listed[0]) == {'player', 'reason', 'expiresAt'}, listed
        print('production bridge one-shot import of 0.2.x sidecar state: pass', flush=True)
    finally:
        run.close()


def answer_deadline(cert, key, state):
    # Takaro times out at 10 s; the connector answers every request before its deadline even when
    # the game thread is stuck. (1.5 s here instead of the default 8.5 s to keep the test short.)
    run = Run(cert, key, state, env={'TAKARO_REQUEST_DEADLINE_MS': '1500'})
    try:
        run.command('hold', 'HELD')
        started = time.monotonic()
        run.request('stuck', 'getPlayers', {})
        run.request('queued', 'getPlayer', {'gameId': PLAYER})
        stuck = run.response('stuck', timeout=5)
        queued = run.response('queued', timeout=5)
        elapsed = time.monotonic() - started
        assert 'timed out' in stuck['error'], stuck
        assert 'not started' in queued['error'], queued
        assert elapsed < 4, elapsed
        run.command('release', 'RELEASED')
        run.no_response('stuck', seconds=1.0)
        run.no_response('queued', seconds=0.3)
        assert run.health()['deadlineAnswers'] == 2
        run.request('after', 'getPlayer', {'gameId': PLAYER})
        assert run.response('after')['payload']['gameId'] == PLAYER
        print(f'production bridge answers a stuck and a queued request within the deadline ({elapsed:.2f}s): pass', flush=True)
    finally:
        run.close()


def main():
    with tempfile.TemporaryDirectory(prefix='dragonwilds-full-bridge-') as tmp:
        directory = Path(tmp)
        key, csr, cert = (directory / name for name in ('peer.key', 'peer.csr', 'peer.pem'))
        ext = directory / 'extensions.cnf'
        ext.write_text('subjectAltName=DNS:localhost\n')
        subprocess.run(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
                        '-subj', '/CN=localhost', '-keyout', str(key), '-out', str(csr)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(['openssl', 'x509', '-req', '-in', str(csr), '-signkey', str(key),
                        '-days', '1', '-extfile', str(ext), '-out', str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for test in (protocol_v1, protocol_v0_forced, answer_deadline, outage, reconcile_after_crash, log_events, whitelist, args_shapes, timed_expiry_restart, legacy_import,
                     replay, backlog_request_latency, crash_write, directory_sync, mutations, stale_effect,
                     engine_before_plugin, ambiguous_engine_ban, newer_ban_after_crash, permanent_restart, timed_replacement,
                     failed_ban_write,
                     late_ban, late_permanent, partial_unban, shutdown):
            state = directory / test.__name__
            state.mkdir()
            print(f'production bridge case: {test.__name__}', flush=True)
            test(cert, key, state)


if __name__ == '__main__':
    main()
