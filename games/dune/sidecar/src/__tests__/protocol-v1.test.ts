import { afterEach, describe, expect, it, vi } from 'vitest';
import { WebSocketServer, type WebSocket } from 'ws';
import { Bridge } from '../bridge.js';
import { dimensionForMap, HAGGA_BASIN, MAP_DIMENSIONS } from '../dune/mapDimensions.js';
import { mockPlayer } from '../testing/mockBattlegroup.js';
import { ACTION_CAPABILITY, buildManifest } from '../takaro/capabilities.js';
import { TakaroWsClient } from '../takaro/client.js';
import { GAME_EVENT_TYPES, GAME_SERVER_ACTIONS, type WsMessage } from '../takaro/protocol.js';
import { harness } from './helpers.js';

function waitFor<T>(fn: () => T | undefined | false, timeoutMs = 4000): Promise<T> {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = (): void => {
      const v = fn();
      if (v) return resolve(v);
      if (Date.now() - start > timeoutMs) return reject(new Error('waitFor timed out'));
      setTimeout(tick, 10);
    };
    tick();
  });
}

describe('map Dimensions', () => {
  it('declares Hagga Basin with the server-derived calibration and no image (the operator uploads one)', () => {
    expect(MAP_DIMENSIONS).toEqual([HAGGA_BASIN]);
    expect(HAGGA_BASIN).toEqual({
      id: 'survival_1',
      name: 'Hagga Basin',
      axes: { horizontal: { field: 'x', direction: 'right' }, vertical: { field: 'y', direction: 'down' } },
      // 8 x 8 landscape tiles of 101600 cm, origins -457200 .. 254000.
      bounds: { horizontal: { min: -457200, max: 355600 }, vertical: { min: -457200, max: 355600 } },
      source: { type: 'none' },
    });
    expect(HAGGA_BASIN.bounds.horizontal.max - HAGGA_BASIN.bounds.horizontal.min).toBe(8 * 101600);
  });

  it('names the Dimension after the partition map, so other maps never land on the Hagga Basin image', () => {
    expect(dimensionForMap('Survival_1', 'HaggaBasin')).toBe('survival_1');
    expect(dimensionForMap('DeepDesert_1', null)).toBe('deepdesert_1');
    expect(dimensionForMap('SH_Arrakeen')).toBe('sh_arrakeen');
    expect(dimensionForMap('', 'HaggaBasin')).toBe('survival_1');
    expect(dimensionForMap(null, null)).toBe('survival_1');
  });
});

describe('Capability Manifest', () => {
  it('lists every action but testReachability, every event, and the map', () => {
    const manifest = buildManifest(MAP_DIMENSIONS);
    const names = manifest.map((c) => c.name);
    expect(Object.keys(ACTION_CAPABILITY).sort()).toEqual(GAME_SERVER_ACTIONS.filter((a) => a !== 'testReachability').sort());
    for (const type of GAME_EVENT_TYPES) expect(names).toContain(`events.${type}`);
    expect(names).not.toContain('map.tiles');
    expect(manifest.find((c) => c.name === 'map')).toEqual({ name: 'map', parameters: { dimensions: [HAGGA_BASIN] } });
    expect(new Set(names).size).toBe(names.length);
  });

  it('leaves the map out when there is no Dimension', () => {
    expect(buildManifest([]).some((c) => c.name === 'map')).toBe(false);
  });
});

describe('getPlayerLocation carries the Dimension', () => {
  it('Hagga Basin by default, the partition map otherwise', async () => {
    const hagga = await harness();
    expect(await hagga.adapter.handleAction('getPlayerLocation', { gameId: '6FF6498F4074E3DE' })).toMatchObject({ dimension: 'survival_1' });
    const deep = await harness({ players: [mockPlayer({ worldMap: 'DeepDesert_1', map: 'DeepDesert_0' })] });
    expect(await deep.adapter.handleAction('getPlayerLocation', { gameId: '6FF6498F4074E3DE' })).toEqual({
      x: 21723,
      y: 219189,
      z: 1200,
      dimension: 'deepdesert_1',
    });
  });

  it('a plugin death position without a map takes the Dimension of the last answered location', async () => {
    const h = await harness({ players: [mockPlayer({ worldMap: 'DeepDesert_1' })] });
    const wire: [string, unknown][] = [];
    const bridge = new Bridge({ adapter: h.adapter, takaro: { send: () => true, sendGameEvent: (t, d) => (wire.push([t, d]), true) } });
    await bridge.handleRequest({ type: 'request', requestId: 'r1', payload: { action: 'getPlayerLocation', args: { gameId: '6FF6498F4074E3DE' } } });
    bridge.emit('player-death', { player: { gameId: '6FF6498F4074E3DE', name: 'Tester' }, position: { x: 1, y: 2, z: 3 } });
    bridge.emit('player-death', { player: { gameId: 'unknown', name: 'Other' }, position: { x: 1, y: 2, z: 3 } });
    expect(wire.map(([, d]) => (d as { position: unknown }).position)).toEqual([
      { x: 1, y: 2, z: 3, dimension: 'deepdesert_1' },
      { x: 1, y: 2, z: 3, dimension: 'survival_1' },
    ]);
  });

  it('an unknown action fails with the v1 code unsupported', async () => {
    const h = await harness();
    const sent: WsMessage[] = [];
    const bridge = new Bridge({ adapter: h.adapter, takaro: { send: (m) => (sent.push(m), true), sendGameEvent: () => true } });
    await bridge.handleRequest({ type: 'request', requestId: 'r2', payload: { action: 'getMapInfo', args: {} } });
    expect(sent[0].error).toEqual({ code: 'unsupported', message: "Unknown Takaro action 'getMapInfo'" });
  });
});

describe('TakaroWsClient protocol negotiation', () => {
  const cleanups: (() => unknown)[] = [];
  afterEach(async () => {
    for (const c of cleanups.splice(0).reverse()) await c();
    vi.restoreAllMocks();
  });

  /** A fake Takaro that answers identify with `version`, and records every frame. */
  async function takaro(version: 0 | 1 | null) {
    const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise((r) => wss.once('listening', r));
    cleanups.push(() => new Promise((r) => wss.close(r)));
    const state = { frames: [] as WsMessage[], socket: null as WebSocket | null };
    wss.on('connection', (ws) => {
      state.socket = ws;
      ws.on('message', (raw) => {
        const msg = JSON.parse(raw.toString()) as WsMessage;
        state.frames.push(msg);
        if (msg.type !== 'identify') return;
        const payload: Record<string, unknown> = { gameServerId: 'gs-1' };
        if (version !== null) payload.protocolVersion = version;
        ws.send(JSON.stringify({ type: 'identifyResponse', payload }));
      });
    });
    return { url: `ws://127.0.0.1:${(wss.address() as { port: number }).port}`, state };
  }

  function quiet(): void {
    vi.spyOn(console, 'info').mockImplementation(() => undefined);
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    vi.spyOn(console, 'error').mockImplementation(() => undefined);
  }

  it('offers [0, 1] with the manifest; on version 1 events are numbered and only eventAck confirms them', async () => {
    quiet();
    const { url, state } = await takaro(1);
    const client = new TakaroWsClient(url, { identityToken: 'id', registrationToken: 'reg' }, { pingIntervalMs: 0, manifest: () => buildManifest(MAP_DIMENSIONS) });
    cleanups.push(() => client.shutdown());
    const confirmed: number[] = [];
    client.onConfirmed((id) => confirmed.push(id));
    client.connect();
    await waitFor(() => client.identified());
    expect(client.getProtocolVersion()).toBe(1);
    const identify = state.frames[0].payload as Record<string, unknown>;
    expect(identify.protocolVersions).toEqual([0, 1]);
    expect((identify.capabilities as { name: string }[]).some((c) => c.name === 'map')).toBe(true);

    expect(client.sendGameEvent('chat-message', { msg: 'a' })).toBe(true);
    expect(client.sendGameEvent('chat-message', { msg: 'b' })).toBe(true);
    await waitFor(() => state.frames.filter((f) => f.type === 'gameEvent').length === 2);
    const events = state.frames.filter((f) => f.type === 'gameEvent').map((f) => f.payload as Record<string, unknown>);
    expect(events.map((e) => e.seq)).toEqual([1, 2]);
    expect(events[0].streamId).toBe(events[1].streamId);
    expect(events[0]).toMatchObject({ type: 'chat-message', data: { msg: 'a' } });

    // An ack for another stream is ignored; ours confirms up to seq.
    state.socket!.send(JSON.stringify({ type: 'eventAck', payload: { streamId: 'other', upTo: 2 } }));
    state.socket!.send(JSON.stringify({ type: 'eventAck', payload: { streamId: events[0].streamId, upTo: 1 } }));
    await waitFor(() => confirmed.length === 1);
    expect(client.lastConfirmedId()).toBe(1);

    // Errors take the { code, message } shape.
    client.sendError('req-1', 'boom');
    await waitFor(() => state.frames.some((f) => f.type === 'response'));
    expect(state.frames.find((f) => f.type === 'response')?.error).toEqual({ code: 'game_error', message: 'boom' });

    // A new manifest can replace the old one.
    expect(client.sendCapabilities()).toBe(true);
    await waitFor(() => state.frames.some((f) => f.type === 'capabilities'));
  });

  it('an older Takaro that answers without a version keeps everything on version 0', async () => {
    quiet();
    const { url, state } = await takaro(null);
    const client = new TakaroWsClient(url, { identityToken: 'id', registrationToken: 'reg' }, { pingIntervalMs: 0, manifest: () => buildManifest(MAP_DIMENSIONS) });
    cleanups.push(() => client.shutdown());
    client.connect();
    await waitFor(() => client.identified());
    expect(client.getProtocolVersion()).toBe(0);
    client.sendGameEvent('chat-message', { msg: 'a' });
    client.sendError('req-1', 'boom');
    expect(client.sendCapabilities()).toBe(false);
    await waitFor(() => state.frames.some((f) => f.type === 'response'));
    expect(state.frames.find((f) => f.type === 'gameEvent')?.payload).toEqual({ type: 'chat-message', data: { msg: 'a' } });
    expect(state.frames.find((f) => f.type === 'response')?.error).toBe('boom');
  });

  it('TAKARO_PROTOCOL=0 (no manifest) sends the version 0 identify unchanged', async () => {
    quiet();
    const { url, state } = await takaro(0);
    const client = new TakaroWsClient(url, { identityToken: 'id', registrationToken: 'reg', serverName: 'Dune' }, { pingIntervalMs: 0 });
    cleanups.push(() => client.shutdown());
    client.connect();
    await waitFor(() => client.identified());
    expect(state.frames[0].payload).toEqual({ identityToken: 'id', registrationToken: 'reg', name: 'Dune' });
  });
});
