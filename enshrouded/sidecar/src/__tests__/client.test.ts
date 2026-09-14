import { afterEach, describe, expect, it } from 'vitest';
import { WebSocketServer, type WebSocket } from 'ws';
import { Bridge } from '../bridge.js';
import { MemoryCursorStore } from '../enshrouded/cursorStore.js';
import { EnshroudedPluginClient } from '../enshrouded/pluginClient.js';
import { TakaroWsClient } from '../takaro/client.js';
import { MockPlugin } from '../testing/mockPlugin.js';

function waitFor<T>(fn: () => T | undefined, timeoutMs = 4000): Promise<T> {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = () => {
      const v = fn();
      if (v !== undefined && v !== false) return resolve(v);
      if (Date.now() - start > timeoutMs) return reject(new Error('waitFor timeout'));
      setTimeout(tick, 10);
    };
    tick();
  });
}

describe('TakaroWsClient end-to-end with fake Takaro + mock plugin', () => {
  const cleanups: Array<() => unknown> = [];
  afterEach(async () => {
    for (const c of cleanups.splice(0).reverse()) await c();
  });

  it('identifies, answers ping, serves requests, forwards events, reconnects with backoff', async () => {
    const mock = new MockPlugin();
    await mock.start();
    cleanups.push(() => mock.stop());

    const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise((r) => wss.once('listening', r));
    cleanups.push(() => new Promise((r) => wss.close(r)));
    const port = (wss.address() as { port: number }).port;
    const frames: any[] = [];
    const sockets: WebSocket[] = [];
    wss.on('connection', (ws) => {
      sockets.push(ws);
      ws.on('message', (raw) => {
        const msg = JSON.parse(raw.toString());
        frames.push(msg);
        if (msg.type === 'identify') ws.send(JSON.stringify({ type: 'identifyResponse', payload: { server: { id: 'gs-1' } } }));
      });
      ws.send(JSON.stringify({ type: 'connected' }));
    });

    const takaro = new TakaroWsClient(
      `ws://127.0.0.1:${port}`,
      { identityToken: 'takaro-dev-enshrouded', registrationToken: 'reg-123' },
      { baseReconnectMs: 50, maxReconnectMs: 200 },
    );
    cleanups.push(() => takaro.shutdown());
    const bridge = new Bridge({
      plugin: new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token }),
      takaro,
      cursorStore: new MemoryCursorStore(),
      logFile: '/nonexistent',
      logTailMode: 'never',
      pollIntervalMs: 30,
    });
    cleanups.push(() => bridge.stopEvents());
    takaro.on('request', (m) => void bridge.handleRequest(m));
    takaro.on('identified', () => void bridge.startEvents());
    takaro.on('disconnected', () => bridge.stopEvents());
    takaro.connect();

    await waitFor(() => takaro.identified());
    expect(takaro.getGameServerId()).toBe('gs-1');
    expect(frames[0]).toEqual({ type: 'identify', payload: { identityToken: 'takaro-dev-enshrouded', registrationToken: 'reg-123' } });

    sockets[0].send(JSON.stringify({ type: 'ping' }));
    await waitFor(() => frames.find((f) => f.type === 'pong'));

    sockets[0].send(JSON.stringify({ type: 'request', requestId: 'r-1', payload: { action: 'getPlayers', args: '[]' } }));
    const resp = await waitFor(() => frames.find((f) => f.requestId === 'r-1'));
    expect(resp.type).toBe('response');
    expect(resp.payload[0]).toMatchObject({ gameId: '76561198000005875', platformId: 'steam:76561198000005875' });

    mock.unimplemented.add('POST /shutdown');
    sockets[0].send(JSON.stringify({ type: 'request', requestId: 'r-2', payload: { action: 'shutdown', args: {} } }));
    const err = await waitFor(() => frames.find((f) => f.requestId === 'r-2'));
    expect(err.error).toMatch(/HTTP 501/);

    mock.pushEvent('chat-message', { player: mock.players[0], msg: 'gg' });
    const ev = await waitFor(() => frames.find((f) => f.type === 'gameEvent'));
    expect(ev.payload).toEqual({ type: 'chat-message', data: { player: { gameId: '76561198000005875', name: 'Limon', steamId: '76561198000005875', platformId: 'steam:76561198000005875' }, msg: 'gg', channel: 'global' } });

    // Drop the connection: client must reconnect and re-identify.
    expect(takaro.nextReconnectDelay()).toBe(50);
    sockets[0].terminate();
    await waitFor(() => !takaro.identified() || undefined);
    await waitFor(() => frames.filter((f) => f.type === 'identify').length === 2 || undefined);
    await waitFor(() => takaro.identified());
    expect(sockets).toHaveLength(2);
  });

  it('backoff grows exponentially and caps; identify error forces reconnect', async () => {
    const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise((r) => wss.once('listening', r));
    cleanups.push(() => new Promise((r) => wss.close(r)));
    let identifies = 0;
    wss.on('connection', (ws) => {
      ws.on('message', () => {
        identifies += 1;
        ws.send(JSON.stringify({ type: 'identifyResponse', payload: { error: { message: 'bad token' } } }));
      });
    });
    const port = (wss.address() as { port: number }).port;
    const takaro = new TakaroWsClient(`ws://127.0.0.1:${port}`, { identityToken: 'x', registrationToken: '' }, { baseReconnectMs: 20, maxReconnectMs: 80 });
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    await waitFor(() => identifies >= 4 || undefined, 5000);
    expect(takaro.identified()).toBe(false);
    expect(takaro.nextReconnectDelay()).toBe(80);
  });
});
