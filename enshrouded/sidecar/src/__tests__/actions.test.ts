import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { Bridge, EVENT_LOCATION_FALLBACK_MS } from '../bridge.js';
import { MemoryCursorStore } from '../enshrouded/cursorStore.js';
import { EnshroudedPluginClient } from '../enshrouded/pluginClient.js';
import { GAME_SERVER_ACTIONS, type WsMessage } from '../takaro/protocol.js';
import { MockPlugin } from '../testing/mockPlugin.js';

const LIMON = '76561198000005875';
let mock: MockPlugin;
let bridge: Bridge;
let sent: WsMessage[];
let n = 0;

async function call(action: string, args: unknown = {}): Promise<WsMessage> {
  const reply = await bridge.handleRequest({ type: 'request', requestId: `req-${++n}`, payload: { action, args } });
  expect(reply?.requestId).toBe(`req-${n}`);
  expect(sent[sent.length - 1]).toEqual(reply);
  return reply!;
}

async function ok(action: string, args: unknown = {}): Promise<any> {
  const reply = await call(action, args);
  expect(reply.error, `unexpected error for ${action}: ${reply.error}`).toBeUndefined();
  expect(reply.type).toBe('response');
  return reply.payload;
}

beforeEach(async () => {
  mock = new MockPlugin();
  await mock.start();
  sent = [];
  bridge = new Bridge({
    plugin: new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token, timeoutMs: 2000 }),
    takaro: { send: (m) => (sent.push(m), true), sendGameEvent: () => true },
    cursorStore: new MemoryCursorStore(),
    logFile: '/nonexistent',
    logTailMode: 'never',
  });
});
afterEach(async () => {
  bridge.stopEvents();
  await mock.stop();
});

describe('all 17 Takaro actions', () => {
  it('covers the full action list', () => {
    expect(GAME_SERVER_ACTIONS).toHaveLength(17);
  });

  it('sends Authorization: Bearer token to the plugin', async () => {
    await ok('getPlayers');
    expect(mock.requests.every((r) => r.auth === `Bearer ${mock.token}`)).toBe(true);
  });

  it('testReachability reflects plugin /health', async () => {
    expect(await ok('testReachability')).toEqual({ connectable: true, reason: null });
    mock.health = { status: 'ok', capabilities: { players: 'ok', kick: 'degraded', chatEvents: 'unimplemented' } };
    const degraded = await ok('testReachability');
    expect(degraded.connectable).toBe(true);
    expect(degraded.reason).toMatch(/kick=degraded/);
    expect(degraded.reason).not.toMatch(/chatEvents/);
    mock.health = { status: 'starting' };
    expect((await ok('testReachability')).connectable).toBe(false);
    mock.token = 'rotated';
    const unauth = await ok('testReachability');
    expect(unauth).toMatchObject({ connectable: false });
    expect(unauth.reason).toMatch(/401/);
    await mock.stop();
    const down = await ok('testReachability');
    expect(down.connectable).toBe(false);
    expect(down.reason).toMatch(/unreachable/);
  });

  it('getPlayers maps to IGamePlayer with gameId=steamId', async () => {
    mock.players.push({ gameId: '9(9)', name: 'Offline', steamId: '1', online: false });
    expect(await ok('getPlayers', [])).toEqual([
      { gameId: LIMON, name: 'Limon', steamId: LIMON, platformId: `steam:${LIMON}` },
      { gameId: '76561198000001111', name: 'Guest', steamId: '76561198000001111', platformId: 'steam:76561198000001111' },
    ]);
  });

  it('getPlayer accepts flat, nested, JSON-string args and returns null when unknown', async () => {
    const expected = { gameId: LIMON, name: 'Limon', steamId: LIMON, platformId: `steam:${LIMON}` };
    expect(await ok('getPlayer', { gameId: LIMON })).toEqual(expected);
    expect(await ok('getPlayer', { player: { gameId: LIMON } })).toEqual(expected);
    expect(await ok('getPlayer', JSON.stringify({ gameId: LIMON }))).toEqual(expected);
    expect(await ok('getPlayer', { gameId: `steam:${LIMON}` })).toEqual(expected);
    expect(await ok('getPlayer', { gameId: '123' })).toEqual({});
    expect((await call('getPlayer', [])).error).toMatch(/player identifier/);
  });

  it('getPlayerLocation returns IPosition and resolves steamId to plugin gameId', async () => {
    expect(await ok('getPlayerLocation', { player: { gameId: LIMON } })).toEqual({ x: 10.5, y: 20, z: -3 });
    expect(mock.lastRequest('GET', '/players/76561198000005875/location')).toBeTruthy();
    expect((await call('getPlayerLocation', { gameId: 'nobody' })).error).toMatch(/404/);
  });

  describe('getPlayerLocation during a connect/disconnect event window', () => {
    const player = { gameId: LIMON, name: 'Limon', steamId: LIMON, platformId: `steam:${LIMON}` };

    it('without a forwarded event a failing lookup is still an error', async () => {
      mock.unimplemented.add('GET /players/:id/location');
      expect((await call('getPlayerLocation', { gameId: LIMON })).error).toMatch(/501/);
    });

    it('answers the origin when the plugin cannot locate (501) a player whose event was just forwarded', async () => {
      mock.unimplemented.add('GET /players/:id/location');
      bridge.noteConnectionEvent('player-connected', { player });
      expect(await ok('getPlayerLocation', { gameId: LIMON })).toEqual({ x: 0, y: 0, z: 0 });
      expect(await ok('getPlayerLocation', { player: { gameId: LIMON } })).toEqual({ x: 0, y: 0, z: 0 });
    });

    it('answers the last real position after the player left', async () => {
      expect(await ok('getPlayerLocation', { gameId: LIMON })).toEqual({ x: 10.5, y: 20, z: -3 });
      mock.players = mock.players.filter((p) => p.gameId !== LIMON);
      bridge.noteConnectionEvent('player-disconnected', { player });
      expect(await ok('getPlayerLocation', { gameId: LIMON })).toEqual({ x: 10.5, y: 20, z: -3 });
    });

    it('window expires and ignores non-connection events and other players', async () => {
      mock.unimplemented.add('GET /players/:id/location');
      bridge.noteConnectionEvent('chat-message', { player });
      expect(bridge.eventLocationFallback({ gameId: LIMON })).toBeNull();
      bridge.noteConnectionEvent('player-disconnected', { player }, 1_000);
      expect(bridge.eventLocationFallback({ gameId: LIMON }, 1_000 + EVENT_LOCATION_FALLBACK_MS)).toEqual({ x: 0, y: 0, z: 0 });
      expect(bridge.eventLocationFallback({ gameId: LIMON }, 2_000 + EVENT_LOCATION_FALLBACK_MS)).toBeNull();
      expect(bridge.eventLocationFallback({ gameId: 'someone-else' })).toBeNull();
    });
  });

  it('getPlayerInventory returns IItemDTO[] with string quality', async () => {
    expect(await ok('getPlayerInventory', { gameId: LIMON })).toEqual([
      { code: 'Wood', name: 'Wood Log', amount: 25 },
      { code: 'Sword_Iron', name: 'Iron Sword', amount: 1, quality: '3' },
    ]);
  });

  it('giveItem uses key `item` and forwards amount/quality', async () => {
    expect(await ok('giveItem', { player: { gameId: LIMON }, item: 'Torch', amount: 5, quality: '2' })).toEqual({});
    expect(mock.lastRequest('POST', '/give')?.body).toEqual({ gameId: '76561198000005875', code: 'Torch', amount: 5, quality: '2' });
    await ok('giveItem', JSON.stringify({ gameId: LIMON, item: 'Wood', amount: 1 }));
    expect(mock.lastRequest('POST', '/give')?.body).toEqual({ gameId: '76561198000005875', code: 'Wood', amount: 1 });
    expect((await call('giveItem', { gameId: LIMON })).error).toMatch(/'item'/);
  });

  it('listItems / listEntities / listLocations', async () => {
    expect(await ok('listItems')).toEqual([
      { code: 'Wood', name: 'Wood Log', description: 'Basic material' },
      { code: 'Torch', name: 'Torch' },
    ]);
    expect(await ok('listEntities')).toEqual([
      { code: 'Scavenger', name: 'Scavenger', type: 'hostile' },
      { code: 'Blacksmith', name: 'Blacksmith', type: 'friendly' },
      { code: 'Wolf', name: 'Wolf', type: 'neutral' },
    ]);
    expect(await ok('listLocations', '{}')).toEqual([{ code: 'cradle', name: 'Cradle', position: { x: 0, y: 100, z: 0 }, radius: 50 }]);
  });

  it('executeConsoleCommand returns CommandOutput', async () => {
    expect(await ok('executeConsoleCommand', { command: 'save' })).toEqual({ success: true, rawResult: 'ran save', errorMessage: null });
    mock.commandHandler = () => ({ success: false, output: 'unknown command' });
    expect(await ok('executeConsoleCommand', { command: 'nope' })).toEqual({ success: false, rawResult: 'unknown command', errorMessage: 'unknown command' });
  });

  it('sendMessage: global, opts.recipient, senderNameOverride', async () => {
    await ok('sendMessage', { message: 'hello all' });
    expect(mock.lastRequest('POST', '/message')?.body).toEqual({ text: 'hello all' });
    await ok('sendMessage', { message: 'Welcome', opts: { recipient: { gameId: LIMON }, senderNameOverride: 'Takaro' } });
    expect(mock.lastRequest('POST', '/message')?.body).toEqual({ text: 'Takaro: Welcome', recipientGameId: '76561198000005875' });
    expect((await call('sendMessage', {})).error).toMatch(/'message'/);
  });

  it('teleportPlayer', async () => {
    await ok('teleportPlayer', { player: { gameId: LIMON }, x: 1, y: '2', z: 3.5 });
    expect(mock.lastRequest('POST', '/teleport')?.body).toEqual({ gameId: '76561198000005875', x: 1, y: 2, z: 3.5 });
    expect((await call('teleportPlayer', { gameId: LIMON, x: 1 })).error).toMatch(/numeric/);
  });

  it('kickPlayer', async () => {
    await ok('kickPlayer', { player: { gameId: LIMON }, reason: 'afk' });
    expect(mock.lastRequest('POST', '/kick')?.body).toEqual({ gameId: '76561198000005875', reason: 'afk' });
  });

  it('banPlayer / listBans / unbanPlayer round-trip (incl. offline steamId)', async () => {
    await ok('banPlayer', { player: { gameId: LIMON }, reason: 'grief', expiresAt: '2030-01-01T00:00:00.000Z' });
    expect(mock.lastRequest('POST', '/ban')?.body).toEqual({ gameId: '76561198000005875', reason: 'grief', expiresAt: '2030-01-01T00:00:00.000Z' });
    await ok('banPlayer', { gameId: '76561198000009999' });
    expect(mock.lastRequest('POST', '/ban')?.body).toEqual({ gameId: '76561198000009999' });
    expect(await ok('listBans')).toEqual([
      { player: { gameId: LIMON, name: 'Limon', steamId: LIMON, platformId: `steam:${LIMON}` }, reason: 'grief', expiresAt: '2030-01-01T00:00:00.000Z' },
      { player: { gameId: '76561198000009999', name: '76561198000009999', steamId: '76561198000009999', platformId: 'steam:76561198000009999' }, reason: '', expiresAt: null },
    ]);
    await ok('unbanPlayer', { gameId: '76561198000009999' });
    expect(await ok('listBans')).toHaveLength(1);
  });

  it('listBans maps the real plugin /bans shape (bannedAccounts from enshrouded_server.json)', async () => {
    mock.bans = [
      { gameId: LIMON, steamId: LIMON, name: 'Limon', characterName: 'Hero', bannedAt: '2026-09-13T20:00:00Z', reason: '', expiresAt: null } as never,
    ];
    expect(await ok('listBans')).toEqual([
      { player: { gameId: LIMON, name: 'Limon', steamId: LIMON, platformId: `steam:${LIMON}` }, reason: '', expiresAt: null },
    ]);
    mock.bans = [{ gameId: LIMON, name: 'Limon' } as never];
    expect((await ok('listBans'))[0].player.platformId).toBe(`steam:${LIMON}`);
  });

  it('shutdown', async () => {
    expect(await ok('shutdown')).toEqual({});
    expect(mock.lastRequest('POST', '/shutdown')).toBeTruthy();
  });

  it('plugin 501 becomes a clear error response, not a crash', async () => {
    mock.unimplemented.add('POST /teleport');
    mock.unimplemented.add('GET /players/:id/inventory');
    mock.unimplemented.add('GET /entities');
    const t = await call('teleportPlayer', { gameId: LIMON, x: 1, y: 2, z: 3 });
    expect(t.type).toBe('response');
    expect(t.error).toMatch(/cannot perform 'teleportPlayer'.*not implemented \/teleport \(HTTP 501\)/);
    expect((await call('getPlayerInventory', { gameId: LIMON })).error).toMatch(/HTTP 501/);
    expect((await call('listEntities')).error).toMatch(/HTTP 501/);
    expect(await ok('getPlayers')).toHaveLength(2); // still serving
  });

  it('unknown action and malformed request get error responses', async () => {
    expect((await call('flyToMoon')).error).toMatch(/Unknown Takaro action/);
    const reply = await bridge.handleRequest({ type: 'request', requestId: 'bad', payload: {} });
    expect(reply).toEqual({ type: 'response', requestId: 'bad', error: 'Takaro request missing action' });
    expect(await bridge.handleRequest({ type: 'request', payload: { action: 'getPlayers' } })).toBeNull();
  });
});
