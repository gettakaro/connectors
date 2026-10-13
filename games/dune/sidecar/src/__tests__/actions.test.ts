import { describe, expect, it } from 'vitest';
import { ActionError, MAX_SHUTDOWN_NOTICE_SECONDS, parseArgv, playerId } from '../dune/adapter.js';
import { DunePluginClient } from '../dune/pluginClient.js';
import type { TakaroItem, TakaroPlayer } from '../dune/types.js';
import { MockBattlegroup, mockPlayer } from '../testing/mockBattlegroup.js';
import { harness } from './helpers.js';

const FLS = '6FF6498F4074E3DE';

/** A plugin client backed by a canned fetch, so the "with plugin" paths are testable without a game. */
/**
 * `/players` is served by default, because a plugin that has never seen `PostLogin` can answer nothing
 * at all: the sidecar addresses the plugin by ITS ref (`acct:<account_id>`), which only exists once the
 * join has seen the player. `mockPlayer()` has `accountId: 1`, so `acct:1` is the ref for `FLS`.
 */
const PLUGIN_REF = 'acct:1';

function fakePlugin(routes: Record<string, unknown>): DunePluginClient {
  const withPlayers: Record<string, unknown> = {
    '/players': { players: [{ ref: PLUGIN_REF, accountId: 1, characterName: 'Tester', positionSource: 'pawn' }] },
    ...routes,
  };
  return new DunePluginClient({
    baseUrl: 'http://plugin',
    token: 't',
    fetchImpl: async (input) => {
      // The client percent-encodes the ref (`acct:1` -> `acct%3A1`), so the canned routes are matched
      // on the DECODED path.
      const path = decodeURIComponent(String(input).replace('http://plugin', '').split('?')[0]);
      const body = withPlayers[path];
      if (body === undefined) return new Response('not found', { status: 404 });
      return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
    },
  });
}

describe('reads', () => {
  it('testReachability is driven by the game\'s own farm readiness', async () => {
    const h = await harness();
    expect(await h.adapter.testReachability()).toEqual({ connectable: true, reason: null });

    h.battlegroup.partitions = { expected: 2, readyAlive: 0 };
    const down = await h.adapter.testReachability();
    expect(down.connectable).toBe(false);
    expect(down.reason).toMatch(/no battlegroup partition is alive and ready \(0\/2\)/);

    h.battlegroup.failQueries = 'connection refused';
    expect(await h.adapter.testReachability()).toEqual({ connectable: false, reason: 'connection refused' });
  });

  it('getPlayers returns only online players, with the Dune identity mapping', async () => {
    const h = await harness({
      players: [mockPlayer(), mockPlayer({ flsId: 'BBBB2222CCCC3333', characterName: 'Chani', accountId: 2, onlineStatus: 'Offline' })],
    });
    const players = (await h.adapter.handleAction('getPlayers', {})) as TakaroPlayer[];
    expect(players).toEqual([
      { gameId: FLS, name: 'Tester', steamId: '76561190000000001', platformId: 'steam:76561190000000001' },
    ]);
    // Neither IP nor ping exists out of process; inventing them would be a lie.
    expect(players[0].ip).toBeUndefined();
    expect(players[0].ping).toBeUndefined();
  });

  it('getPlayer NEVER answers {} or null — every branch returns a real IGamePlayer', async () => {
    const h = await harness({ players: [mockPlayer({ onlineStatus: 'Offline' })] });

    // (a) known, offline
    const offline = (await h.adapter.handleAction('getPlayer', { gameId: FLS })) as TakaroPlayer;
    expect(offline).toMatchObject({ gameId: FLS, name: 'Tester', online: false });

    // (b) never seen by this connector: a minimal record built from the identifier
    const unknown = (await h.adapter.handleAction('getPlayer', { gameId: 'nobody-at-all' })) as TakaroPlayer;
    expect(unknown).toEqual({ gameId: 'nobody-at-all', name: 'nobody-at-all', online: false });

    // (c) the database is down: still a record, not an error frame
    h.battlegroup.failQueries = 'postgres is down';
    const degraded = (await h.adapter.handleAction('getPlayer', { gameId: FLS })) as TakaroPlayer;
    expect(degraded.gameId).toBe(FLS);
    expect(typeof degraded.name).toBe('string');

    for (const answer of [offline, unknown, degraded]) {
      expect(typeof answer.gameId).toBe('string');
      expect(answer.gameId.length).toBeGreaterThan(0);
    }
  });

  it('getPlayer / getPlayers accept a steamId or a platformId as the identifier', async () => {
    const h = await harness();
    expect(((await h.adapter.handleAction('getPlayer', { gameId: '76561190000000001' })) as TakaroPlayer).gameId).toBe(FLS);
    expect(((await h.adapter.handleAction('getPlayer', { platformId: 'steam:76561190000000001' })) as TakaroPlayer).gameId).toBe(FLS);
    expect(((await h.adapter.handleAction('getPlayer', { player: { gameId: 'Tester' } })) as TakaroPlayer).gameId).toBe(FLS);
  });

  it('getPlayerLocation prefers the live plugin, then the chat origin, then the last SAVED position', async () => {
    // 3. saved position only
    const saved = await harness();
    expect(await saved.adapter.handleAction('getPlayerLocation', { gameId: FLS })).toEqual({ x: 21723, y: 219189, z: 1200, dimension: 'survival_1' });

    // 2. a chat message gives a live origin, which outranks the saved one
    saved.battlegroup.deliverChat(
      MockBattlegroup.chatBody({ m_Id: 'c1', m_Message: { m_UnlocalizedMessage: 'hi' }, m_OriginLocation: { X: 1, Y: 2, Z: 3 } }),
      { userId: FLS },
    );
    expect(await saved.adapter.handleAction('getPlayerLocation', { gameId: FLS })).toEqual({ x: 1, y: 2, z: 3, dimension: 'survival_1' });

    // 1. the plugin wins outright
    const withPlugin = await harness({ plugin: fakePlugin({ [`/players/${PLUGIN_REF}/location`]: { x: 9, y: 9, z: 9, source: 'pawn' } }) });
    expect(await withPlugin.adapter.handleAction('getPlayerLocation', { gameId: FLS })).toEqual({ x: 9, y: 9, z: 9, dimension: 'survival_1' });

    // nothing at all: fail honestly rather than report the map origin
    const nothing = await harness({ players: [mockPlayer({ position: null })] });
    await expect(nothing.adapter.handleAction('getPlayerLocation', { gameId: FLS })).rejects.toThrow(/No location/);
  });

  it('getPlayerInventory aggregates stacks by (code, quality) and uses catalogue display names', async () => {
    const h = await harness({
      players: [
        mockPlayer({
          inventory: [
            { templateId: 'PowerPack2', stackSize: 2, qualityLevel: 1, inventoryType: 0 },
            { templateId: 'PowerPack2', stackSize: 3, qualityLevel: 1, inventoryType: 14 },
            { templateId: 'PowerPack2', stackSize: 1, qualityLevel: 4, inventoryType: 0 },
            { templateId: 'WaterFlask', stackSize: 1, qualityLevel: 0, inventoryType: 0 },
            { templateId: 'UnknownThing', stackSize: 1, inventoryType: 0 },
          ],
        }),
      ],
    });
    const items = (await h.adapter.handleAction('getPlayerInventory', { gameId: FLS })) as TakaroItem[];
    expect(items).toEqual([
      { code: 'PowerPack2', name: 'Power Pack', amount: 5, quality: '1' },
      { code: 'PowerPack2', name: 'Power Pack', amount: 1, quality: '4' },
      { code: 'WaterFlask', name: 'Water Flask', amount: 1 },
      // No catalogue row: the code is the honest fallback name, never an invented one.
      { code: 'UnknownThing', name: 'UnknownThing', amount: 1 },
    ]);
  });

  it('listItems / listEntities carry human display names, never the dev code', async () => {
    const h = await harness();
    const items = (await h.adapter.handleAction('listItems', {})) as TakaroItem[];
    expect(items.length).toBeGreaterThan(0);
    for (const item of items) {
      expect(item.name).not.toBe(item.code);
      expect(item.name).not.toMatch(/^BP_|_C$/);
    }
    expect(((await h.adapter.handleAction('listItems', { search: 'power' })) as TakaroItem[]).length).toBe(1);

    const entities = (await h.adapter.handleAction('listEntities', {})) as { code: string; name: string; type: string }[];
    expect(entities.find((e) => e.code === 'Sandworm')).toMatchObject({ name: 'Sandworm', type: 'hostile' });
    expect(entities.find((e) => e.code === 'Buggy')).toMatchObject({ name: 'Scout Buggy', type: 'friendly' });
  });

  it('listLocations reports the running partitions', async () => {
    const h = await harness();
    const locations = (await h.adapter.handleAction('listLocations', {})) as { code: string; name: string }[];
    expect(locations[0]).toMatchObject({ code: 'partition:1', name: 'Hagga Basin (survival_1)' });
  });
});

describe('mutations read the effect back', () => {
  it('giveItem publishes AddItemToInventory and verifies the inventory delta', async () => {
    const h = await harness();
    const player = h.battlegroup.players[0];
    // The server writes the item during the verify window.
    setTimeout(() => player.inventory!.push({ templateId: 'WaterFlask', stackSize: 1, inventoryType: 0 }), 10);
    const result = (await h.adapter.handleAction('giveItem', { gameId: FLS, item: 'WaterFlask', amount: 1, quality: null })) as Record<string, unknown>;
    expect(result.verified).toBe(true);
    expect(h.gmSent().at(-1)).toEqual({
      ServerCommand: 'AddItemToInventory',
      PlayerId: FLS,
      ItemName: 'WaterFlask',
      Quantity: 1,
      Durability: 1,
    });
  });

  it('giveItem verifies a grant that landed OUTSIDE the display containers, and says which one', async () => {
    // MEASURED on the live rig: a contract/quest item granted with `AddItemToInventory` lands in
    // `inventories.inventory_type = 29`, which `getPlayerInventory` does not show. Reading the delta back from the
    // display set alone reported `verified:false` for a grant that had plainly succeeded.
    const h = await harness();
    const player = h.battlegroup.players[0];
    setTimeout(() => player.inventory!.push({ templateId: 'wy1ll', stackSize: 4, inventoryType: 29 }), 10);
    const result = (await h.adapter.handleAction('giveItem', { gameId: FLS, item: 'wy1ll', amount: 4 })) as Record<string, unknown>;
    expect(result.verified).toBe(true);
    expect(result.inventoryTypes).toEqual([29]);
    expect(result.count).toBe(4);
    // …and it is still absent from what the player is shown, which is correct and must not be "fixed" by widening
    // getPlayerInventory.
    const shown = (await h.adapter.handleAction('getPlayerInventory', { gameId: FLS })) as Array<{ code: string }>;
    expect(shown.some((i) => i.code === 'wy1ll')).toBe(false);
  });

  it('giveItem answers verified:false with the real reason instead of claiming success', async () => {
    const h = await harness();
    const result = (await h.adapter.handleAction('giveItem', { gameId: FLS, item: 'WaterFlask', amount: 1 })) as Record<string, unknown>;
    expect(result.verified).toBe(false);
    expect(String(result.reason)).toMatch(/persists inventories periodically/);
  });

  it('teleportPlayer tolerates dimension:null and says so when it cannot verify', async () => {
    const h = await harness();
    const result = (await h.adapter.handleAction('teleportPlayer', { gameId: FLS, x: 10, y: 20, z: 30, dimension: null, yaw: null })) as Record<string, unknown>;
    expect(h.gmSent().at(-1)).toEqual({ ServerCommand: 'TeleportToExact', PlayerId: FLS, X: 10, Y: 20, Z: 30 });
    expect(result).toMatchObject({ verified: false });
    expect(String(result.reason)).toMatch(/no live position source/);

    const withPlugin = await harness({ plugin: fakePlugin({ [`/players/${PLUGIN_REF}/location`]: { x: 10, y: 20, z: 30, source: 'pawn' } }) });
    expect(await withPlugin.adapter.handleAction('teleportPlayer', { gameId: FLS, x: 10, y: 20, z: 30 })).toEqual({ verified: true });
  });

  it('kickPlayer verifies by the player leaving the online set', async () => {
    const h = await harness();
    setTimeout(() => h.battlegroup.setOnline(FLS, false), 10);
    expect(await h.adapter.handleAction('kickPlayer', { gameId: FLS, reason: 'testing' })).toEqual({ verified: true });
    expect(h.gmSent().at(-1)).toEqual({ ServerCommand: 'KickPlayer', PlayerId: FLS });

    const stubborn = await harness();
    const result = (await stubborn.adapter.handleAction('kickPlayer', { gameId: FLS })) as Record<string, unknown>;
    expect(result.verified).toBe(false);
    expect(String(result.reason)).toMatch(/still online/);
  });

  it('sendMessage whispers to a recipient and broadcasts without one', async () => {
    const h = await harness();
    const whisper = (await h.adapter.handleAction('sendMessage', {
      message: 'private',
      opts: { recipient: { gameId: FLS }, senderNameOverride: null },
    })) as Record<string, unknown>;
    // The whisper routing key is the FUNCOM id, not the FLS id — that is what the client binds its queue under.
    expect(whisper).toMatchObject({ delivered: true, channel: 'whisper', routingKey: 'PLAYER#12345', sender: 'Takaro' });

    const global = (await h.adapter.handleAction('sendMessage', { message: 'everyone', opts: null })) as Record<string, unknown>;
    expect(global).toMatchObject({ channel: 'global', mode: 'chat', chat: true });
    expect(h.battlegroup.publishes.at(-1)!.exchange).toBe('chat.map');

    const both = await harness({ globalMessageMode: 'both' });
    const result = (await both.adapter.handleAction('sendMessage', { message: 'x' })) as Record<string, unknown>;
    expect(result).toMatchObject({ chat: true, broadcast: true });
    expect(both.gmSent().at(-1)).toMatchObject({ ServerCommand: 'ServiceBroadcast', BroadcastType: 'Generic' });
  });

  it('shutdown refuses clearly when no stop hook is configured', async () => {
    const h = await harness();
    await expect(h.adapter.handleAction('shutdown', {})).rejects.toThrow(/DUNE_SHUTDOWN_CMD/);
    // The in-game notice still went out, so players are not dropped without warning if the operator fixes the config.
    expect(h.gmSent().at(-1)).toMatchObject({ ServerCommand: 'ServiceBroadcast', BroadcastType: 'ServerShutdown' });
  });

  it('shutdown runs the configured hook as argv, with no shell', async () => {
    const calls: { argv: string[] }[] = [];
    const h = await harness({
      players: [],
      shutdownCmd: 'docker stop "takaro dune survival"',
      execRunner: async (argv) => {
        calls.push({ argv });
        return { code: 0, stdout: 'stopped', stderr: '' };
      },
    });
    expect(await h.adapter.handleAction('shutdown', {})).toMatchObject({ scheduled: true, hook: 'docker' });
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(calls[0].argv).toEqual(['docker', 'stop', 'takaro dune survival']);
    expect(parseArgv('["docker","stop","c"]')).toEqual(['docker', 'stop', 'c']);
  });

  /**
   * L6d: the first version announced "server goes down in 60 s" and then ran the stop hook in the same tick, so the
   * countdown on the player's screen was decoration. The notice now has to be true — and since Takaro's request
   * budget is ~10 s, honouring it means SCHEDULING the stop and saying so, not blocking the action.
   */
  it('shutdown with players in world honours the countdown and does NOT claim the server stopped', async () => {
    const waits: number[] = [];
    let stopped = 0;
    const h = await harness({
      players: [mockPlayer()],
      shutdownCmd: 'docker stop c',
      shutdownNoticeSeconds: 45,
      sleep: async (ms) => void waits.push(ms),
      execRunner: async () => {
        stopped += 1;
        return { code: 0, stdout: 'stopped', stderr: '' };
      },
    });
    const result = (await h.adapter.handleAction('shutdown', {})) as Record<string, unknown>;
    expect(result).toMatchObject({ verified: false, scheduled: true, noticeSeconds: 45, onlinePlayers: 1 });
    expect(String(result.reason)).toMatch(/before the server has actually stopped/);
    expect(waits).toEqual([45_000]);
    expect((h.gmSent().at(-1)?.BroadcastPayload as Record<string, unknown>).ShutdownDuration).toBe(45);
    // The scheduled hook still runs, once, after the countdown.
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(stopped).toBe(1);
  });

  it('shutdown on an EMPTY server announces 1 s -- there is nobody to warn -- but still only SCHEDULES', async () => {
    // Stopping the map server takes minutes (UE world save) even with nobody in world, so a synchronous answer
    // would blow Takaro's ~10 s request budget: measured live as `400 "Request timed out after 10000ms : shutdown"`.
    const waits: number[] = [];
    let stopped = 0;
    const h = await harness({
      players: [],
      shutdownCmd: 'docker stop c',
      shutdownNoticeSeconds: 60,
      sleep: async (ms) => void waits.push(ms),
      execRunner: async () => {
        stopped += 1;
        return { code: 0, stdout: 'stopped', stderr: '' };
      },
    });
    expect(await h.adapter.handleAction('shutdown', {})).toMatchObject({
      verified: false,
      scheduled: true,
      noticeSeconds: 1,
      onlinePlayers: 0,
    });
    expect(waits).toEqual([1000]);
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(stopped).toBe(1);
  });

  it('a misconfigured notice cannot run away: the countdown is capped at MAX_SHUTDOWN_NOTICE_SECONDS', async () => {
    const waits: number[] = [];
    const h = await harness({
      players: [mockPlayer()],
      shutdownCmd: 'docker stop c',
      shutdownNoticeSeconds: 600,
      sleep: async (ms) => void waits.push(ms),
      execRunner: async () => ({ code: 0, stdout: 'stopped', stderr: '' }),
    });
    expect(await h.adapter.handleAction('shutdown', {})).toMatchObject({ noticeSeconds: MAX_SHUTDOWN_NOTICE_SECONDS });
    expect(waits).toEqual([MAX_SHUTDOWN_NOTICE_SECONDS * 1000]);
  });
});

describe('argument parsing', () => {
  it('playerId accepts flat and nested shapes and refuses nothing at all', () => {
    expect(playerId({ gameId: 'a' })).toBe('a');
    expect(playerId({ steamId: 'b' })).toBe('b');
    expect(playerId({ player: { platformId: 'steam:c' } })).toBe('steam:c');
    expect(playerId({ playerRef: { gameId: 'd' } })).toBe('d');
    expect(() => playerId({})).toThrow(ActionError);
    expect(() => playerId({ gameId: null })).toThrow(ActionError);
  });

  it('giveItem rejects a missing item and a non-positive amount', async () => {
    const h = await harness();
    await expect(h.adapter.handleAction('giveItem', { gameId: FLS })).rejects.toThrow(/requires 'item'/);
    await expect(h.adapter.handleAction('giveItem', { gameId: FLS, item: 'x', amount: 0 })).rejects.toThrow(/positive/);
  });

  it('an unknown action is an error, not a silent success', async () => {
    const h = await harness();
    await expect(h.adapter.handleAction('nonsense', {})).rejects.toThrow(/Unknown Takaro action/);
  });
});
