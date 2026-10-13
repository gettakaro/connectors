import { afterEach, describe, expect, it } from 'vitest';
import { WebSocketServer } from 'ws';
import { Bridge, MAX_PENDING_EVENTS, type TakaroSink } from '../bridge.js';
import { TakaroWsClient } from '../takaro/client.js';
import type { GameEventType } from '../takaro/protocol.js';
import { harness } from './helpers.js';

function waitFor<T>(fn: () => T | undefined | false, timeoutMs = 4000): Promise<T> {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = (): void => {
      const v = fn();
      if (v !== undefined && v !== false) return resolve(v as T);
      if (Date.now() - start > timeoutMs) return reject(new Error('waitFor timeout'));
      setTimeout(tick, 10);
    };
    tick();
  });
}

interface Rig {
  bridge: Bridge;
  wire: [GameEventType, unknown][];
  setUp: (up: boolean) => void;
  confirm: () => void;
}

async function rig(): Promise<Rig> {
  const h = await harness();
  const wire: [GameEventType, unknown][] = [];
  let sendId = 0;
  let up = true;
  const listeners: ((id: number) => void)[] = [];
  const sink: TakaroSink = {
    send: () => up,
    sendGameEvent: (type, data) => (up ? (sendId += 1, wire.push([type, data]), true) : false),
    lastSendId: () => sendId,
    onConfirmed: (cb) => listeners.push(cb),
  };
  const bridge = new Bridge({ adapter: h.adapter, takaro: sink });
  return { bridge, wire, setUp: (value) => (up = value), confirm: () => listeners.forEach((l) => l(sendId)) };
}

/**
 * `ws.send()` succeeding proves only that the LOCAL kernel took the bytes. During an egress outage the sidecar keeps
 * writing gameEvents into a black hole while the socket still reads "open", and a cursor advanced on the send alone
 * loses every event in that window. The fix is an unconfirmed-send window released only by a pong for a ping written
 * after the event.
 */
describe('unconfirmed-send window', () => {
  it('a written-but-unconfirmed event is re-sent after the socket dies, in order, exactly once', async () => {
    const r = await rig();
    r.bridge.emit('chat-message', { msg: 'a' });
    r.bridge.emit('chat-message', { msg: 'b' });
    expect(r.wire.map(([, d]) => (d as { msg: string }).msg)).toEqual(['a', 'b']);
    expect(r.bridge.unconfirmed()).toHaveLength(2);
    expect(r.bridge.pending()).toHaveLength(0);

    // The socket dies before any pong.
    r.bridge.onTakaroDown();
    expect(r.bridge.unconfirmed()).toHaveLength(0);
    expect(r.bridge.pending().map((e) => (e.data as { msg: string }).msg)).toEqual(['a', 'b']);

    r.wire.length = 0;
    expect(r.bridge.flushPending()).toBe(2);
    expect(r.wire.map(([, d]) => (d as { msg: string }).msg)).toEqual(['a', 'b']);

    // Confirmed: released for good, and not sent a third time.
    r.confirm();
    expect(r.bridge.unconfirmed()).toHaveLength(0);
    r.wire.length = 0;
    expect(r.bridge.flushPending()).toBe(0);
    expect(r.wire).toEqual([]);
  });

  it('a confirmed event is NOT re-sent after a reconnect', async () => {
    const r = await rig();
    r.bridge.emit('chat-message', { msg: 'confirmed' });
    r.confirm();
    r.bridge.emit('chat-message', { msg: 'unconfirmed' });
    r.bridge.onTakaroDown();
    r.wire.length = 0;
    r.bridge.flushPending();
    expect(r.wire.map(([, d]) => (d as { msg: string }).msg)).toEqual(['unconfirmed']);
  });
});

describe('outage queue', () => {
  it('events produced while Takaro is down are queued in order and flushed on identify', async () => {
    const r = await rig();
    r.setUp(false);
    // `'queued'`, not `false`: the event is safely held, so a seq-carrying source may advance its SCAN position.
    // `false` would make the plugin poller re-read and re-queue the same seq on every tick (duplicate flood).
    expect(r.bridge.emit('chat-message', { msg: '1' })).toBe('queued');
    r.bridge.emit('chat-message', { msg: '2' });
    expect(r.wire).toEqual([]);
    expect(r.bridge.pending().map((e) => (e.data as { msg: string }).msg)).toEqual(['1', '2']);

    r.setUp(true);
    expect(r.bridge.flushPending()).toBe(2);
    expect(r.wire.map(([, d]) => (d as { msg: string }).msg)).toEqual(['1', '2']);
  });

  it('the queue is bounded: the OLDEST events are dropped and the drop is counted, never silent', async () => {
    const r = await rig();
    r.setUp(false);
    for (let i = 0; i < MAX_PENDING_EVENTS + 5; i += 1) r.bridge.emit('log', { msg: `line ${i}` });
    expect(r.bridge.pending()).toHaveLength(MAX_PENDING_EVENTS);
    expect(r.bridge.dropped()).toBe(5);
    expect((r.bridge.pending()[0].data as { msg: string }).msg).toBe('line 5');
  });

  it('a flush that fails part-way keeps the rest queued, in order', async () => {
    const h = await harness();
    const wire: unknown[] = [];
    let allowed = 1;
    const bridge = new Bridge({
      adapter: h.adapter,
      takaro: {
        send: () => true,
        sendGameEvent: (_t, d) => {
          if (allowed <= 0) return false;
          allowed -= 1;
          wire.push(d);
          return true;
        },
      },
    });
    allowed = 0;
    bridge.emit('log', { msg: 'x' });
    bridge.emit('log', { msg: 'y' });
    bridge.emit('log', { msg: 'z' });
    allowed = 2;
    expect(bridge.flushPending()).toBe(2);
    expect(bridge.pending().map((e) => (e.data as { msg: string }).msg)).toEqual(['z']);
  });
});

describe('connect/disconnect location fallback', () => {
  it('a getPlayerLocation that fails inside the event window answers the last position instead of dropping the event', async () => {
    const r = await rig();
    r.bridge.emit('player-connected', { player: { gameId: 'FLS1', name: 'Tester' } });
    // Takaro asks right after the event; nothing knows this player's position.
    expect(r.bridge.eventLocationFallback({ gameId: 'FLS1' })).toEqual({ x: 0, y: 0, z: 0, dimension: 'survival_1' });
    // Outside any event window there is no fallback: the request fails honestly.
    expect(r.bridge.eventLocationFallback({ gameId: 'someone-else' })).toBeNull();
  });

  it('the bridge keeps the online set in step with what Takaro was told, and reconciles a restart', async () => {
    const r = await rig();
    r.bridge.emit('player-connected', { player: { gameId: 'A', name: 'A' } });
    r.bridge.emit('player-connected', { player: { gameId: 'B', name: 'B' } });
    expect(r.bridge.onlinePlayers().map((p) => p.gameId)).toEqual(['A', 'B']);

    r.wire.length = 0;
    const gone = await r.bridge.reconcileOnline(new Set(['A']));
    expect(gone.map((p) => p.gameId)).toEqual(['B']);
    expect(r.wire.map(([t]) => t)).toEqual(['player-disconnected']);
    expect(r.bridge.onlinePlayers().map((p) => p.gameId)).toEqual(['A']);
  });
});

describe('TakaroWsClient heartbeat (delivery proof)', () => {
  const cleanups: (() => unknown)[] = [];
  afterEach(async () => {
    for (const c of cleanups.splice(0).reverse()) await c();
  });

  it('a pong confirms every gameEvent written before the ping; an inbound frame alone does not', async () => {
    const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise((r) => wss.once('listening', r));
    cleanups.push(() => new Promise((r) => wss.close(r)));
    wss.on('connection', (ws) => {
      ws.on('message', (raw) => {
        if ((JSON.parse(raw.toString()) as { type: string }).type === 'identify') {
          ws.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs-dune' } }));
        }
      });
    });
    const port = (wss.address() as { port: number }).port;
    const takaro = new TakaroWsClient(`ws://127.0.0.1:${port}`, { identityToken: 'dune', registrationToken: 'reg' }, { baseReconnectMs: 20, pingIntervalMs: 60 });
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    await waitFor(() => takaro.identified() || undefined);
    expect(takaro.getGameServerId()).toBe('gs-dune');

    expect(takaro.sendGameEvent('chat-message', { msg: 'wire-1' })).toBe(true);
    expect(takaro.lastSendId()).toBe(1);
    expect(takaro.lastConfirmedId()).toBe(0);
    await waitFor(() => takaro.lastConfirmedId() >= 1 || undefined, 2000);
  });

  it('refuses to write a gameEvent before identifyResponse — such a frame is silently lost', async () => {
    const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise((r) => wss.once('listening', r));
    cleanups.push(() => new Promise((r) => wss.close(r)));
    const port = (wss.address() as { port: number }).port;
    let connected = false;
    wss.on('connection', () => (connected = true));
    const takaro = new TakaroWsClient(`ws://127.0.0.1:${port}`, { identityToken: 'dune', registrationToken: 'reg' }, { pingIntervalMs: 0 });
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    await waitFor(() => connected || undefined);
    expect(takaro.sendGameEvent('chat-message', { msg: 'too early' })).toBe(false);
  });
});
