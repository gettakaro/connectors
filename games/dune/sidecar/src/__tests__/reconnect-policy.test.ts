import net from 'node:net';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { WebSocketServer, type WebSocket } from 'ws';
import { reconnectDelay, TakaroWsClient } from '../takaro/client.js';

/**
 * Reconnect policy (gettakaro/connectors#858): a stable session that drops is retried within ~2 s, only consecutive
 * failures back off (exponential, jittered, capped at 60 s), and the streak resets once a connection held.
 */
const DEFAULTS = { fastMs: 1000, baseMs: 2000, maxMs: 60_000 };
const at = (r: number) => ({ ...DEFAULTS, random: () => r });

describe('reconnectDelay', () => {
  it('retries a dropped stable session within 0.5-1.5 s, never a fixed wait', () => {
    expect(reconnectDelay(0, at(0))).toBe(500);
    expect(reconnectDelay(0, at(0.5))).toBe(1000);
    expect(reconnectDelay(0, at(0.999999))).toBe(1500);
    const seen = new Set<number>();
    for (let i = 0; i < 200; i++) {
      const d = reconnectDelay(0, { ...DEFAULTS, random: Math.random });
      expect(d).toBeGreaterThanOrEqual(500);
      expect(d).toBeLessThanOrEqual(1500);
      seen.add(d);
    }
    expect(seen.size).toBeGreaterThan(20);
  });

  it('the client default retries a dropped stable session in 150-450 ms, so identify lands well inside 2 s', () => {
    const at0 = new TakaroWsClient('ws://x', { identityToken: 'a', registrationToken: 'r' }, { random: () => 0 });
    const at1 = new TakaroWsClient('ws://x', { identityToken: 'a', registrationToken: 'r' }, { random: () => 0.999999 });
    expect(at0.nextReconnectDelay()).toBe(150);
    expect(at1.nextReconnectDelay()).toBe(450);
  });

  it('backs off exponentially on consecutive failures', () => {
    const top = [1, 2, 3, 4, 5, 6, 7].map((n) => reconnectDelay(n, at(0.999999)));
    expect(top).toEqual([2000, 4000, 8000, 16000, 32000, 60000, 60000]);
  });

  it('waits a random 50-100% of each backoff step, also at the cap', () => {
    expect(reconnectDelay(3, at(0))).toBe(4000);
    expect(reconnectDelay(3, at(0.999999))).toBe(8000);
    expect(reconnectDelay(20, at(0))).toBe(30_000);
    const atCap = new Set(Array.from({ length: 100 }, () => reconnectDelay(20, { ...DEFAULTS, random: Math.random })));
    expect(atCap.size).toBeGreaterThan(50);
  });

  it('never exceeds the 60 s cap, also with jitter and huge streaks', () => {
    for (const n of [6, 7, 10, 100, 10_000]) {
      for (const r of [0, 0.5, 0.999999]) expect(reconnectDelay(n, at(r))).toBeLessThanOrEqual(60_000);
    }
    expect(reconnectDelay(100, at(0.999999))).toBe(60_000);
  });

  it('keeps honouring operator overrides of the old base/max keys', () => {
    const p = { fastMs: 1000, baseMs: 10_000, maxMs: 30_000, random: () => 0.5 };
    expect(reconnectDelay(0, p)).toBe(1000);
    expect(reconnectDelay(1, p)).toBe(7500);
    expect(reconnectDelay(5, p)).toBe(22_500);
  });
});

function waitFor<T>(fn: () => T | undefined | false, timeoutMs = 5000): Promise<T> {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = (): void => {
      const v = fn();
      if (v !== undefined && v !== false) return resolve(v as T);
      if (Date.now() - start > timeoutMs) return reject(new Error('waitFor timeout'));
      setTimeout(tick, 5);
    };
    tick();
  });
}

describe('TakaroWsClient reconnect timing', () => {
  const cleanups: (() => unknown)[] = [];
  afterEach(async () => {
    for (const c of cleanups.splice(0).reverse()) await c();
    vi.restoreAllMocks();
  });

  async function server(opts: { autoPong?: boolean; accept?: () => boolean } = {}) {
    const wss = new WebSocketServer({ port: 0, host: '127.0.0.1', autoPong: opts.autoPong ?? true });
    await new Promise((r) => wss.once('listening', r));
    cleanups.push(() => new Promise((r) => wss.close(r)));
    const state = { sockets: [] as WebSocket[], connectedAt: [] as number[] };
    wss.on('connection', (ws) => {
      state.sockets.push(ws);
      state.connectedAt.push(Date.now());
      ws.on('message', (raw) => {
        const msg = JSON.parse(raw.toString()) as { type: string };
        if (msg.type !== 'identify') return;
        if (opts.accept && !opts.accept()) {
          ws.send(JSON.stringify({ type: 'identifyResponse', payload: { error: { message: 'nope' } } }));
          return;
        }
        ws.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs' } }));
      });
    });
    return { url: `ws://127.0.0.1:${(wss.address() as { port: number }).port}`, state };
  }

  function quiet(): string[] {
    const lines: string[] = [];
    for (const k of ['info', 'warn', 'error'] as const) vi.spyOn(console, k).mockImplementation((m: string) => void lines.push(m));
    return lines;
  }

  it('a stable session reset by the peer is identified again within the fast window', async () => {
    quiet();
    const { url, state } = await server();
    const takaro = new TakaroWsClient(
      url,
      { identityToken: 'a', registrationToken: 'r' },
      { pingIntervalMs: 0, fastReconnectMs: 100, baseReconnectMs: 5000, stableAfterMs: 50 },
    );
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    for (let round = 1; round <= 3; round++) {
      await waitFor(() => takaro.identified() || undefined);
      await new Promise((r) => setTimeout(r, 80)); // stable
      const cut = Date.now();
      state.sockets[state.sockets.length - 1].terminate(); // RST-like, no close frame
      await waitFor(() => !takaro.identified() || undefined);
      await waitFor(() => takaro.identified() || undefined);
      // fast window is 50-150 ms; the 5 s base would blow this
      expect(Date.now() - cut).toBeLessThan(1000);
    }
    expect(state.sockets.length).toBe(4);
  });

  it('consecutive failures back off, and the streak resets only after a stable session', async () => {
    const lines = quiet();
    let accept = false;
    const { url, state } = await server({ accept: () => accept });
    const takaro = new TakaroWsClient(
      url,
      { identityToken: 'a', registrationToken: 'r' },
      { pingIntervalMs: 0, fastReconnectMs: 20, baseReconnectMs: 40, maxReconnectMs: 160, stableAfterMs: 100, random: () => 0.5 },
    );
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    await waitFor(() => state.sockets.length >= 6 || undefined, 3000);
    const delays = lines.flatMap((l) => /Reconnecting to Takaro in (\d+)ms/.exec(l)?.[1] ?? []).map(Number);
    // a fresh process gets one fast retry, then 75% of 40, 80, 160 and the 160 cap (jitter pinned to its midpoint)
    expect(delays.slice(0, 5)).toEqual([20, 30, 60, 120, 120]);
    expect(lines.some((l) => l.includes('failure streak 4'))).toBe(true);
    expect(state.connectedAt[5] - state.connectedAt[0]).toBeGreaterThanOrEqual(20 + 30 + 60 + 120 + 120 - 20);

    accept = true;
    await waitFor(() => takaro.identified() || undefined, 2000);
    await waitFor(() => lines.some((l) => l.includes('reconnect backoff reset')) || undefined, 2000);
    expect(takaro.nextReconnectDelay()).toBe(20);
  });

  it('a connection that identifies and drops before it is stable keeps the streak', async () => {
    quiet();
    const { url, state } = await server();
    const takaro = new TakaroWsClient(
      url,
      { identityToken: 'a', registrationToken: 'r' },
      { pingIntervalMs: 0, fastReconnectMs: 20, baseReconnectMs: 40, maxReconnectMs: 1000, stableAfterMs: 10_000, random: () => 0.5 },
    );
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    for (let i = 0; i < 3; i++) {
      await waitFor(() => (takaro.identified() && state.sockets.length === i + 1) || undefined);
      state.sockets[i].terminate();
      await waitFor(() => !takaro.identified() || undefined);
    }
    // drops 1..3: fast (attempt 0), then 40, then 80 → next step is 160, waited at 75%
    expect(takaro.nextReconnectDelay()).toBe(120);
  });

  it('detects a half-open socket (no pongs) within the bounded heartbeat window and reconnects fast', async () => {
    const lines = quiet();
    const { url, state } = await server({ autoPong: false });
    const takaro = new TakaroWsClient(
      url,
      { identityToken: 'a', registrationToken: 'r' },
      { pingIntervalMs: 50, maxMissedPongs: 2, idleTimeoutMs: 10_000, fastReconnectMs: 20, stableAfterMs: 10 },
    );
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    await waitFor(() => takaro.identified() || undefined);
    const start = Date.now();
    await waitFor(() => state.sockets.length >= 2 || undefined, 2000);
    // 2 missed pongs + the tick that notices = 3 ping intervals, plus the fast retry
    // far below the 10 s idle timeout: the pong count, not the idle timer, closed it
    expect(Date.now() - start).toBeLessThan(3 * 50 + 1000);
    expect(lines.some((l) => l.includes('heartbeat ping(s) unanswered'))).toBe(true);
    expect(lines.some((l) => l.includes('fast first retry'))).toBe(true);
  });

  it('gives up on a connect into a blackholed path after the handshake timeout and tries again', async () => {
    const lines = quiet();
    // Accepts TCP and never answers the upgrade, like a path that drops every packet after the SYN.
    const sockets: net.Socket[] = [];
    const tcp = net.createServer((s) => void sockets.push(s));
    await new Promise<void>((r) => tcp.listen(0, '127.0.0.1', r));
    cleanups.push(() => new Promise((r) => { for (const s of sockets) s.destroy(); tcp.close(r); }));
    const url = `ws://127.0.0.1:${(tcp.address() as net.AddressInfo).port}`;
    const takaro = new TakaroWsClient(
      url,
      { identityToken: 'a', registrationToken: 'r' },
      { pingIntervalMs: 0, handshakeTimeoutMs: 100, baseReconnectMs: 20, maxReconnectMs: 40 },
    );
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    await waitFor(() => sockets.length >= 3 || undefined, 2000);
    expect(lines.some((l) => l.includes('Opening handshake has timed out'))).toBe(true);
  });
});

