import assert from 'node:assert/strict';
import { test } from 'node:test';
import { WebSocketServer, type WebSocket } from 'ws';
import { TakaroWsClient } from '../takaro/client.js';
import type { WsMessage } from '../takaro/protocol.js';
import { DEFAULT_RECONNECT_TIMING, reconnectDelay } from '../takaro/reconnectPolicy.js';

const fixed = (value: number) => ({ ...DEFAULT_RECONNECT_TIMING, random: () => value });

test('a drop after a stable session is retried within 0.5 to 1.25 s, never after a fixed wait', () => {
  assert.equal(reconnectDelay(0, fixed(0)), 500);
  assert.equal(reconnectDelay(0, fixed(0.5)), 875);
  assert.equal(reconnectDelay(0, fixed(0.999999)), 1250);
  const seen = new Set<number>();
  for (let i = 0; i < 500; i += 1) {
    const delay = reconnectDelay(0, DEFAULT_RECONNECT_TIMING);
    assert.ok(delay >= 500 && delay <= 1250, `${delay}`);
    seen.add(delay);
  }
  // Jitter: a fleet reconnecting after one Takaro restart does not arrive in the same millisecond.
  assert.ok(seen.size > 100, `only ${seen.size} distinct delays`);
});

test('consecutive failures back off exponentially from 2 s with equal jitter, capped at 60 s', () => {
  const top = [1, 2, 3, 4, 5, 6, 7, 8].map((failures) => reconnectDelay(failures, fixed(0.999999)));
  assert.deepEqual(top, [2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000]);
  const bottom = [1, 2, 3, 4, 5, 6, 7].map((failures) => reconnectDelay(failures, fixed(0)));
  assert.deepEqual(bottom, [1000, 2000, 4000, 8000, 16000, 30000, 30000]);
  for (const failures of [1, 3, 6, 50, 1_000, Number.MAX_SAFE_INTEGER]) {
    for (let i = 0; i < 200; i += 1) {
      const delay = reconnectDelay(failures, DEFAULT_RECONNECT_TIMING);
      assert.ok(Number.isFinite(delay) && delay > 0 && delay <= 60_000, `${failures}: ${delay}`);
    }
  }
});

test('an operator-set base and cap are still honoured', () => {
  const timing = { ...fixed(0.999999), baseMs: 5_000, maxMs: 20_000 };
  assert.deepEqual([1, 2, 3, 4].map((failures) => reconnectDelay(failures, timing)), [5000, 10000, 20000, 20000]);
  // The fast retry after a stable session never exceeds a cap set below it.
  assert.equal(reconnectDelay(0, { ...timing, maxMs: 300 }), 300);
});

interface FakeOptions {
  /** Answer identify; false drops the socket as soon as identify arrives. */
  acceptIdentify?: boolean;
}

/** A Takaro stand-in that answers identify and pings, and can go silent like a blackholed link. */
async function fakeTakaro(options: FakeOptions = {}) {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const state = { identifies: [] as number[], pings: 0, silent: false, sockets: [] as WebSocket[] };
  server.on('connection', (socket) => {
    state.sockets.push(socket);
    socket.on('message', (raw) => {
      if (state.silent) return;
      const message = JSON.parse(raw.toString()) as WsMessage;
      if (message.type === 'identify') {
        state.identifies.push(Date.now());
        if (options.acceptIdentify === false) {
          socket.terminate();
          return;
        }
        socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs-1', protocolVersion: 1 } }));
      }
      if (message.type === 'ping') {
        state.pings += 1;
        socket.send(JSON.stringify({ type: 'pong' }));
      }
    });
  });
  return {
    url: `ws://127.0.0.1:${address.port}`,
    state,
    /** The edge resets the live connection. */
    reset() {
      for (const socket of server.clients) socket.terminate();
    },
    async close() {
      for (const socket of server.clients) socket.terminate();
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
  };
}

async function waitFor(check: () => boolean, timeoutMs = 5000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('timed out');
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const identity = { registrationToken: 'reg', identityToken: 'id', name: 'Terraria Test' };
const v1 = { offerV1: true, manifest: () => [] };

test('a reset after a stable session reconnects at once, and the streak only grows on failures', async () => {
  const takaro = await fakeTakaro();
  // A 10 s failure backoff: only the fast path can reconnect inside the waits below.
  const client = new TakaroWsClient(takaro.url, identity, 10_000, 60_000, v1, {
    stableMs: 150, fastRetryMinMs: 20, fastRetryMaxMs: 60,
  });
  const scheduled: Array<{ delayMs: number; failures: number }> = [];
  client.on('reconnectScheduled', (info) => scheduled.push(info));
  try {
    client.connect();
    for (let drop = 1; drop <= 3; drop += 1) {
      await waitFor(() => client.identified());
      await sleep(250);
      const cutAt = Date.now();
      takaro.reset();
      await waitFor(() => takaro.state.identifies.length === drop + 1, 1000);
      assert.ok(takaro.state.identifies[drop] - cutAt < 500, `drop ${drop}: ${takaro.state.identifies[drop] - cutAt} ms`);
    }
    assert.deepEqual(scheduled.map((s) => s.failures), [0, 0, 0]);
    for (const s of scheduled) assert.ok(s.delayMs >= 20 && s.delayMs <= 60, `${s.delayMs}`);
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test('a drop before the session is stable counts as a failure and backs off', async () => {
  const takaro = await fakeTakaro();
  const client = new TakaroWsClient(takaro.url, identity, 400, 60_000, v1, {
    stableMs: 10_000, fastRetryMinMs: 1, fastRetryMaxMs: 2,
  });
  const scheduled: Array<{ delayMs: number; failures: number }> = [];
  client.on('reconnectScheduled', (info) => scheduled.push(info));
  try {
    client.connect();
    await waitFor(() => client.identified());
    takaro.reset();
    await waitFor(() => scheduled.length === 1);
    assert.equal(scheduled[0].failures, 1);
    assert.ok(scheduled[0].delayMs >= 200 && scheduled[0].delayMs <= 400, `${scheduled[0].delayMs}`);
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test('a peer that accepts every socket and drops it keeps backing off: an open socket is not a success', async () => {
  const takaro = await fakeTakaro({ acceptIdentify: false });
  const client = new TakaroWsClient(takaro.url, identity, 10, 80, v1);
  const scheduled: Array<{ delayMs: number; failures: number }> = [];
  client.on('reconnectScheduled', (info) => scheduled.push(info));
  try {
    client.connect();
    await waitFor(() => scheduled.length >= 6);
    assert.deepEqual(scheduled.slice(0, 6).map((s) => s.failures), [1, 2, 3, 4, 5, 6]);
    for (const s of scheduled) assert.ok(s.delayMs <= 80, `${s.delayMs}`);
    assert.ok(scheduled[4].delayMs >= 40, 'the cap is reached, not a fixed short wait');
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test('a silent link is closed after the inbound timeout and reconnected; a link that answers pings is kept', async () => {
  const takaro = await fakeTakaro();
  const client = new TakaroWsClient(takaro.url, identity, 10_000, 60_000, v1, {
    stableMs: 50, pingIntervalMs: 40, inboundTimeoutMs: 200, fastRetryMinMs: 10, fastRetryMaxMs: 20,
  });
  const dead: number[] = [];
  client.on('linkDead', (silentMs) => dead.push(silentMs));
  try {
    client.connect();
    await waitFor(() => client.identified());
    // Pings are answered: three inbound timeouts pass without a reconnect.
    await sleep(600);
    assert.ok(takaro.state.pings >= 5, `${takaro.state.pings} pings`);
    assert.equal(dead.length, 0);
    assert.equal(takaro.state.identifies.length, 1);

    // Blackholed: the socket stays open on both ends, nothing comes back.
    takaro.state.silent = true;
    const silentAt = Date.now();
    await waitFor(() => dead.length === 1, 2000);
    const detectedIn = Date.now() - silentAt;
    assert.ok(detectedIn >= 150 && detectedIn < 600, `${detectedIn} ms`);
    takaro.state.silent = false;
    // The session before the blackhole was stable, so this is the fast retry.
    await waitFor(() => takaro.state.identifies.length === 2 && client.identified(), 1000);
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test('version 1 events written during the gap reach Takaro after the fast reconnect', async () => {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const received: Array<{ seq: number; data: unknown }> = [];
  server.on('connection', (socket) => {
    socket.on('message', (raw) => {
      const message = JSON.parse(raw.toString()) as WsMessage;
      if (message.type === 'identify') {
        socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs-1', protocolVersion: 1 } }));
      }
      if (message.type === 'gameEvent') {
        const payload = message.payload as { seq: number; data: unknown; streamId: string };
        received.push({ seq: payload.seq, data: payload.data });
        socket.send(JSON.stringify({ type: 'eventAck', payload: { streamId: payload.streamId, upTo: payload.seq } }));
      }
    });
  });
  const client = new TakaroWsClient(`ws://127.0.0.1:${address.port}`, identity, 10_000, 60_000, v1, {
    stableMs: 50, fastRetryMinMs: 100, fastRetryMaxMs: 100,
  });
  try {
    client.connect();
    await waitFor(() => client.identified());
    client.sendGameEvent('chat-message', { msg: 'before' });
    await waitFor(() => received.length === 1);
    await sleep(100);
    for (const socket of server.clients) socket.terminate();
    await waitFor(() => !client.identified());
    // The bridge is between sockets: this event is buffered, not dropped.
    assert.equal(client.sendGameEvent('chat-message', { msg: 'during the gap' }), true);
    await waitFor(() => received.length === 2, 2000);
    assert.deepEqual(received.map((r) => r.data), [{ msg: 'before' }, { msg: 'during the gap' }]);
    assert.deepEqual(received.map((r) => r.seq), [1, 2]);
    await waitFor(() => client.unacknowledged() === 0);
  } finally {
    client.shutdown();
    for (const socket of server.clients) socket.terminate();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

test('events buffered for one game server are not replayed to another after the settings change', async () => {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const received: Array<{ server: string; data: unknown }> = [];
  server.on('connection', (socket) => {
    let gs = '';
    socket.on('message', (raw) => {
      const message = JSON.parse(raw.toString()) as WsMessage;
      if (message.type === 'identify') {
        gs = `gs-${(message.payload as { identityToken: string }).identityToken}`;
        socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: gs, protocolVersion: 1 } }));
      }
      // Never acknowledged, so everything stays buffered.
      if (message.type === 'gameEvent') received.push({ server: gs, data: (message.payload as { data: unknown }).data });
    });
  });
  const url = `ws://127.0.0.1:${address.port}`;
  const client = new TakaroWsClient(url, identity, 10_000, 60_000, v1);
  try {
    client.connect();
    await waitFor(() => client.getGameServerId() === 'gs-id');
    client.sendGameEvent('chat-message', { msg: 'for the first server' });
    await waitFor(() => received.length === 1);
    client.reconfigure(url, { ...identity, identityToken: 'other' });
    await waitFor(() => client.getGameServerId() === 'gs-other');
    client.sendGameEvent('chat-message', { msg: 'for the second server' });
    await waitFor(() => received.length === 2);
    await sleep(100);
    assert.deepEqual(received, [
      { server: 'gs-id', data: { msg: 'for the first server' } },
      { server: 'gs-other', data: { msg: 'for the second server' } },
    ]);
    assert.equal(client.unacknowledged(), 1);
  } finally {
    client.shutdown();
    for (const socket of server.clients) socket.terminate();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});
