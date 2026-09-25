#!/usr/bin/env python3
"""Production native bridge: real workers, TLS, protocol, ring and durable stores.
Only calls into the unavailable VEIN engine are stubbed in the companion binary.
"""
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import native_bridge_adversarial as wire

BINARY = Path(__file__).resolve().parent / 'build/native_full_bridge'
PONGS = threading.Event()
PONGS.set()
original_send = wire.send_frame


def controlled_send(sock, opcode, data):
    if opcode != 10 or PONGS.is_set():
        original_send(sock, opcode, data)


wire.send_frame = controlled_send


class Run(wire.Run):
    def __init__(self, cert, key, state):
        self.peer = wire.Peer(cert, key)
        self.proc = subprocess.Popen(
            [str(BINARY), f'wss://localhost:{self.peer.port}/', str(cert), str(state)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self.read_lines, daemon=True).start()
        try:
            line = self.lines.get(timeout=10)
            assert line == 'READY', line
            self.index = self.identify()
            self.pending = []
            eventually(lambda: self.health()['connection']['identified'])
        except Exception as error:
            self.proc.kill()
            self.proc.wait(timeout=5)
            self.peer.close()
            raise AssertionError(f'full bridge startup failed: {error}\n{self.proc.stderr.read()}') from error

    def event(self, nonce, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for pos, (index, opcode, data) in enumerate(self.pending):
                if opcode == 1:
                    value = json.loads(data)
                    if value.get('type') == 'gameEvent' and value['payload']['data'].get('msg') == nonce:
                        self.pending.pop(pos)
                        return value
            try:
                self.pending.append(self.peer.frames.get(timeout=0.2))
            except queue.Empty:
                pass
        raise AssertionError(f'missing event {nonce}')

    def crash(self):
        self.proc.kill()
        self.proc.wait(timeout=5)
        self.peer.close()


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
    player = '76561198000000001'
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
                   any(row['gameId'] == '76561198000000001'
                       for row in read(state / 'known-players.json')))
        assert read(state / 'known-players.json')[0]['gameId'] == '76561198000000001'
        run.no_response('old-read', seconds=0.5)
        run.request('new-read', 'getPlayer', {'gameId': '76561198000000001'})
        assert run.response('new-read')['payload']['gameId'] == '76561198000000001'
        print('production bridge stale response discarded while completion state is retained: pass', flush=True)
    finally:
        run.close()


def engine_crash_setup(cert, key, state, existing=False):
    player = '76561198000000001'
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
    player = '76561198000000001'
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
    player = '76561198000000001'
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
        run.request('newer-cleanup', 'unbanPlayer', {'gameId': '76561198000000001'})
        assert run.response('newer-cleanup')['payload'] == {}
        print('production bridge recovery preserves newer permanent ban over stale timed intent: pass', flush=True)
    finally:
        run.close()


def permanent_restart(cert, key, state):
    player = '76561198000000001'
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
    player = '76561198000000001'
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
    player = '76561198000000001'
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
    player = '76561198000000001'
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
    player = '76561198000000001'
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
    player = '76561198000000001'
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


def main():
    with tempfile.TemporaryDirectory(prefix='vein-full-bridge-') as tmp:
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
        for test in (replay, crash_write, directory_sync, mutations, stale_effect,
                     engine_before_plugin, ambiguous_engine_ban, newer_ban_after_crash, permanent_restart, timed_replacement,
                     failed_ban_write,
                     late_ban, late_permanent, partial_unban, shutdown):
            state = directory / test.__name__
            state.mkdir()
            print(f'production bridge case: {test.__name__}', flush=True)
            test(cert, key, state)


if __name__ == '__main__':
    main()
