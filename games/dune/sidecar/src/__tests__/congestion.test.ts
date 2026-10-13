import { describe, expect, it, vi } from 'vitest';
import { Bridge, MAX_PENDING_EVENTS, type TakaroSink } from '../bridge.js';
import type { EventPoller } from '../dune/eventPoller.js';
import { makeLogForwarder } from '../dune/logTail.js';
import type { GameEventType } from '../takaro/protocol.js';
import { harness } from './helpers.js';

/**
 * Measured on the dev rig 2026-10-09 (Steam build 25789279): the map server's shutdown log burst filled the socket
 * buffer, sends were refused while the socket stayed OPEN, and the reconcile `player-disconnected` sat in the queue
 * with 1153 log lines for minutes because the queue only drained on a reconnect.
 */
async function rig(pluginEvents?: Pick<EventPoller, 'markDelivered' | 'start' | 'stop'>) {
  const h = await harness();
  const wire: [GameEventType, unknown][] = [];
  let sendId = 0;
  let draining = true;
  const listeners: ((id: number) => void)[] = [];
  const sink: TakaroSink = {
    send: () => draining,
    sendGameEvent: (type, data) => (draining ? (sendId += 1, wire.push([type, data]), true) : false),
    lastSendId: () => sendId,
    onConfirmed: (cb) => listeners.push(cb),
  };
  const bridge = new Bridge({
    adapter: h.adapter,
    takaro: sink,
    drainRetryMs: 5,
    ...(pluginEvents ? { pluginEvents: pluginEvents as unknown as EventPoller } : {}),
  });
  return {
    bridge,
    wire,
    setDraining: (v: boolean) => (draining = v),
    confirm: () => listeners.forEach((l) => l(sendId)),
  };
}

const player = { gameId: 'B19BF41E37CCDF24', name: 'TakaroTest' };

describe('a congested but open socket', () => {
  it('drains the queue on the next emit, in order, without waiting for a reconnect', async () => {
    const r = await rig();
    r.setDraining(false);
    expect(r.bridge.emit('player-disconnected', { player })).toBe('queued');
    expect(r.bridge.pending()).toHaveLength(1);

    r.setDraining(true);
    r.bridge.emit('chat-message', { msg: 'after' });
    expect(r.wire.map(([type]) => type)).toEqual(['player-disconnected', 'chat-message']);
    expect(r.bridge.pending()).toHaveLength(0);
  });

  it('never sends a new event past a queue it could not drain', async () => {
    const r = await rig();
    r.setDraining(false);
    r.bridge.emit('player-disconnected', { player });
    r.bridge.emit('chat-message', { msg: 'later' });
    expect(r.wire).toEqual([]);
    expect(r.bridge.pending().map((e) => e.type)).toEqual(['player-disconnected', 'chat-message']);
  });

  it('retries an idle socket on its own, with no new event and no heartbeat confirmation', async () => {
    const r = await rig();
    r.setDraining(false);
    r.bridge.emit('player-disconnected', { player });
    r.setDraining(true);
    // No emit, no confirm(): the real client only confirms when the confirmed sendId advances.
    await vi.waitFor(() => expect(r.wire.map(([type]) => type)).toEqual(['player-disconnected']), { timeout: 1000 });
    expect(r.bridge.pending()).toHaveLength(0);
  });
});

describe('a full queue', () => {
  it('evicts log lines before a player event, however old the player event is', async () => {
    const r = await rig();
    r.setDraining(false);
    r.bridge.emit('player-disconnected', { player });
    for (let i = 0; i < MAX_PENDING_EVENTS + 10; i += 1) r.bridge.emit('log', { msg: `line ${i}` });
    const pending = r.bridge.pending();
    expect(pending).toHaveLength(MAX_PENDING_EVENTS);
    expect(pending[0].type).toBe('player-disconnected');
    expect(r.bridge.dropped()).toBe(11);
  });

  it('never advances the plugin cursor past an older event that is still queued', async () => {
    const delivered: number[] = [];
    const r = await rig({ markDelivered: (seq: number) => void delivered.push(seq), start: () => {}, stop: () => {} });
    r.setDraining(false);
    r.bridge.emit('player-death', { player }, 1);
    for (let i = 0; i <= MAX_PENDING_EVENTS; i += 1) r.bridge.emit('log', { msg: `line ${i}` }, i + 2);
    // seq 2 was dropped from behind seq 1: releasing it would persist cursor 2 and lose seq 1 on a restart.
    expect(r.bridge.pending()[0].seq).toBe(1);
    expect(delivered).toEqual([]);
  });
});

describe('the map-server log budget', () => {
  function forwarder(lines = 3, windowMs = 1000) {
    let t = 0;
    const sent: string[] = [];
    const forward = makeLogForwarder('all', (m) => sent.push(m), { lines, windowMs }, () => t);
    return { forward, sent, at: (ms: number) => (t = ms) };
  }

  it('forwards at most the budget per window and reports what it suppressed', () => {
    const f = forwarder();
    for (let i = 0; i < 10; i += 1) f.forward(`line ${i}`);
    expect(f.sent).toEqual(['line 0', 'line 1', 'line 2']);

    f.at(1000);
    f.forward('next window');
    expect(f.sent.slice(3)).toEqual(['[takaro-dune] 7 map-server log line(s) suppressed over the 3/1s log budget', 'next window']);
  });

  it('reports a burst that simply stops, on the next tick', () => {
    const f = forwarder();
    for (let i = 0; i < 5; i += 1) f.forward(`line ${i}`);
    f.forward.flush();
    expect(f.sent).toHaveLength(3);
    f.at(1000);
    f.forward.flush();
    expect(f.sent.at(-1)).toBe('[takaro-dune] 2 map-server log line(s) suppressed over the 3/1s log budget');
  });

  it('is a sliding window: a burst across a window boundary gets no extra budget', () => {
    const f = forwarder();
    f.at(900);
    for (let i = 0; i < 3; i += 1) f.forward(`late ${i}`);
    f.at(1000);
    f.forward('just after the old boundary');
    expect(f.sent).toEqual(['late 0', 'late 1', 'late 2']);
  });
});
