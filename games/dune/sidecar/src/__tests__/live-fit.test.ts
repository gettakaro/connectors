import { describe, expect, it } from 'vitest';
import { DunePg, isLeaving, isOnline } from '../dune/pg.js';
import { PresencePoller, graceFor } from '../dune/presence.js';
import { DuneRmq } from '../dune/rmq.js';
import type { DunePlayerRow } from '../dune/types.js';
import { DunePluginClient } from '../dune/pluginClient.js';
import { MockBattlegroup, mockPlayer } from '../testing/mockBattlegroup.js';
import { harness } from './helpers.js';

/**
 * Every assertion in this file pins a fact that was VERIFIED on the live rig or read out of the shipped schema on
 * 2026-09-21 — not a guess. Each block names its source.
 *
 * Sources: `_data/dune-dev/probe/Database/01_Dune.sql` (types and tables),
 * `59_proc_player_online_state.sql` (presence semantics), and the rig's own broker topology.
 */

// ---------------------------------------------------------------------------
// `PlayerConnectionStatus` is three-state (01_Dune.sql: Offline | LoggingOut | Online)
// ---------------------------------------------------------------------------

describe('online_status is a three-state enum', () => {
  it('LoggingOut is neither online nor an unknown state', () => {
    expect(isOnline({ onlineStatus: 'Online' })).toBe(true);
    expect(isOnline({ onlineStatus: 'LoggingOut' })).toBe(false);
    expect(isOnline({ onlineStatus: 'Offline' })).toBe(false);
    expect(isLeaving({ onlineStatus: 'LoggingOut' })).toBe(true);
    expect(isLeaving({ onlineStatus: ' loggingout ' })).toBe(true);
    expect(isLeaving({ onlineStatus: 'Online' })).toBe(false);
    expect(isLeaving({ onlineStatus: 'Offline' })).toBe(false);
  });

  it('a player who goes Online → LoggingOut → Offline disconnects exactly ONCE', async () => {
    const events: string[] = [];
    let status = 'Online';
    const roster = async (): Promise<DunePlayerRow[]> => (status === 'Offline' ? [] : [mockPlayer({ onlineStatus: status })]);
    const presence = new PresencePoller({ roster, emit: (type) => events.push(type), transferGraceMs: 0 });

    await presence.pollOnce();
    expect(events).toEqual(['player-connected']);

    // LoggingOut: the row is still in `player_state`, but the player is on their way out.
    status = 'LoggingOut';
    await presence.pollOnce();
    expect(events).toEqual(['player-connected', 'player-disconnected']);
    expect(presence.onlinePlayers()).toHaveLength(0);

    // …and the Offline row that follows must NOT produce a second leave.
    status = 'Offline';
    await presence.pollOnce();
    await presence.pollOnce();
    expect(events).toEqual(['player-connected', 'player-disconnected']);
  });

  it('a LoggingOut row that flips back to Online inside the grace is not churn', async () => {
    const events: string[] = [];
    let status = 'Online';
    const presence = new PresencePoller({
      roster: async () => [mockPlayer({ onlineStatus: status })],
      emit: (type) => events.push(type),
      transferGraceMs: 45_000,
      now: () => 1_000,
    });
    await presence.pollOnce();
    status = 'LoggingOut';
    await presence.pollOnce();
    status = 'Online';
    await presence.pollOnce();
    expect(events).toEqual(['player-connected']);
    expect(presence.onlinePlayers()).toHaveLength(1);
  });

  it('the roster SQL asks for Online only, so LoggingOut never reaches the differ from Postgres', async () => {
    const battlegroup = new MockBattlegroup();
    battlegroup.players = [mockPlayer({ flsId: 'A' }), mockPlayer({ flsId: 'B', accountId: 2, onlineStatus: 'LoggingOut' })];
    const pg = new DunePg(battlegroup.db);
    await pg.probe();
    const online = await pg.onlinePlayers();
    expect(online.map((p) => p.flsId)).toEqual(['A']);
    const sql = battlegroup.queries.at(-1)?.sql ?? '';
    expect(sql).toContain("ps.online_status::text = 'Online'");
    expect(sql).not.toContain('LoggingOut');
  });
});

// ---------------------------------------------------------------------------
// `reconnect_grace_period_end` beats our hardcoded guess (59_proc_player_online_state.sql)
// ---------------------------------------------------------------------------

describe('transfer grace uses the server’s own reconnect window when it has one', () => {
  const now = Date.parse('2026-09-21T12:00:00Z');

  it('falls back to the configured value when the column is absent or stale', () => {
    expect(graceFor({ flsId: 'A' }, 45_000, now)).toBe(45_000);
    expect(graceFor({ flsId: 'A', reconnectGraceEnd: '' }, 45_000, now)).toBe(45_000);
    expect(graceFor({ flsId: 'A', reconnectGraceEnd: 'not a date' }, 45_000, now)).toBe(45_000);
    // Already expired: never shorter than the configured floor.
    expect(graceFor({ flsId: 'A', reconnectGraceEnd: '2026-09-21T11:00:00Z' }, 45_000, now)).toBe(45_000);
  });

  it('waits until the server’s grace end when that is later', () => {
    expect(graceFor({ flsId: 'A', reconnectGraceEnd: '2026-09-21T12:02:00Z' }, 45_000, now)).toBe(120_000);
    // The column is a bare TIMESTAMP holding UTC; a missing `Z` must not be read as local time.
    expect(graceFor({ flsId: 'A', reconnectGraceEnd: '2026-09-21T12:02:00' }, 45_000, now)).toBe(120_000);
  });

  it('the poller holds a departure for the server’s window, not ours', async () => {
    let clock = now;
    let present = true;
    const row = mockPlayer({ reconnectGraceEnd: '2026-09-21T12:02:00Z' });
    const events: string[] = [];
    const presence = new PresencePoller({
      roster: async () => (present ? [row] : []),
      emit: (type) => events.push(type),
      transferGraceMs: 45_000,
      now: () => clock,
    });
    await presence.pollOnce();
    present = false;
    await presence.pollOnce();
    // Past our 45 s guess, still inside the server's 120 s window: no leave yet.
    clock = now + 60_000;
    await presence.pollOnce();
    expect(events).toEqual(['player-connected']);
    clock = now + 121_000;
    await presence.pollOnce();
    expect(events).toEqual(['player-connected', 'player-disconnected']);
  });
});

// ---------------------------------------------------------------------------
// Schema types (01_Dune.sql)
// ---------------------------------------------------------------------------

describe('schema types', () => {
  it('server_id is read as text, never cast to int8', async () => {
    const battlegroup = new MockBattlegroup();
    battlegroup.players = [mockPlayer({ serverId: 'Survival_1' })];
    const pg = new DunePg(battlegroup.db);
    await pg.probe();
    const [row] = await pg.roster();
    expect(row.serverId).toBe('Survival_1');
    const sql = battlegroup.queries.at(-1)?.sql ?? '';
    // `encrypted_player_state.server_id` is TEXT and references `farm_state.server_id TEXT PRIMARY KEY`.
    expect(sql).toContain('ps.server_id::text');
    expect(sql).not.toContain('ps.server_id::int8');
  });

  it('every reference to the ambiguous "user" column is table-qualified, accounts first', async () => {
    const battlegroup = new MockBattlegroup();
    battlegroup.players = [mockPlayer()];
    const pg = new DunePg(battlegroup.db);
    await pg.probe();
    await pg.findPlayer('6FF6498F4074E3DE');
    const sql = battlegroup.queries.at(-1)?.sql ?? '';
    // `"user"` exists on BOTH `encrypted_accounts` (the base table) and `accounts` (a view over it), so a bare
    // `"user"` would be ambiguous the moment both are joined.
    const references = sql.match(/[A-Za-z_.]*"user"/g) ?? [];
    expect(references.length).toBeGreaterThan(0);
    for (const reference of references) expect(reference).toMatch(/^(acct|enc)\."user"$/);
    // `accounts` first: it is the view the game's own stored procedures read the FLS id through
    // (`is_player_offline`, `get_players_info`, `flag_player_as_cheater` all say `accounts."user"`).
    expect(sql.indexOf('acct."user"')).toBeLessThan(sql.indexOf('enc."user"'));
  });
});

// ---------------------------------------------------------------------------
// `chat.map` may not exist: it is declared by the MAP SERVER, not by text-router (rig probe)
// ---------------------------------------------------------------------------

const WITHOUT_MAP_EXCHANGE = ['heartbeats', 'chat.intercept', 'chat.whispers', 'notifications'];

describe('global sendMessage survives a missing chat exchange', () => {
  it('probes passively on a THROWAWAY channel, never on the one carrying the chat consumer', async () => {
    const { battlegroup, rmq } = await harness({ exchanges: WITHOUT_MAP_EXCHANGE });
    const before = battlegroup.channelsOpened;

    expect(await rmq.exchangeExists('chat.map')).toBe(false);

    expect(battlegroup.channelsOpened).toBe(before + 1); // a second, disposable channel
    expect(battlegroup.channelsKilled).toBe(1); // the 404 killed it — as the broker really does
    expect(battlegroup.mainChannelKilled).toBe(false); // …and not the one we need
    expect(rmq.connected()).toBe(true);
    expect(rmq.chatConsumerBound()).toBe(true);
  });

  it('caches the answer so a chatty module does not open a channel per message', async () => {
    const { battlegroup, rmq } = await harness({ exchanges: WITHOUT_MAP_EXCHANGE });
    const before = battlegroup.channelsOpened;
    await rmq.exchangeExists('chat.map', 30_000, 1_000);
    await rmq.exchangeExists('chat.map', 30_000, 2_000);
    expect(battlegroup.channelsOpened).toBe(before + 1);
    // …and re-probes once the TTL is up, because the map server may have joined in the meantime.
    await rmq.exchangeExists('chat.map', 30_000, 60_000);
    expect(battlegroup.channelsOpened).toBe(before + 2);
  });

  it('a positive answer is cached for good — the game never deletes these', async () => {
    const { battlegroup, rmq } = await harness();
    const before = battlegroup.channelsOpened;
    expect(await rmq.exchangeExists('chat.map', 30_000, 1_000)).toBe(true);
    expect(await rmq.exchangeExists('chat.map', 30_000, 10_000_000)).toBe(true);
    expect(battlegroup.channelsOpened).toBe(before + 1);
  });

  it('refuses the broadcast rather than reporting a publish nobody will route', async () => {
    const { battlegroup, rmq } = await harness({ exchanges: WITHOUT_MAP_EXCHANGE });
    const result = await rmq.broadcastChat('hello');
    expect(result.ok).toBe(false);
    expect(result.reason).toMatch(/chat\.map/);
    expect(battlegroup.publishes.filter((p) => p.exchange === 'chat.map')).toHaveLength(0);
  });

  it('publishes normally once the map server has declared it', async () => {
    const { battlegroup, rmq } = await harness();
    expect((await rmq.broadcastChat('hello')).ok).toBe(true);
    expect(battlegroup.publishes.filter((p) => p.exchange === 'chat.map')).toHaveLength(1);
  });

  it('the connection dropping forgets the cache, because the map server may have joined meanwhile', async () => {
    const battlegroup = new MockBattlegroup();
    battlegroup.exchanges.delete('chat.map');
    const rmq = new DuneRmq({
      url: 'amqps://fls@game-rmq:5672/',
      tlsInsecure: true,
      connect: battlegroup.connect,
      chatQueue: 'takaro_chat_intercept',
      interceptExchange: 'chat.intercept',
      interceptRoutingKey: '#',
      whisperExchange: 'chat.whispers',
      mapExchange: 'chat.map',
      wire: {
        timestampField: 'm_TimeStamp',
        timestampFormat: 'ue',
        channelEnumForm: 'short',
        senderNameField: 'm_UserNameFrom',
        whisperChannelType: 'Map',
        contentKey: 'Content',
        contentType: 'Content',
        amqpType: 'text_chat',
        deliveryMode: 1,
        bodyType: 'TextChat',
      },
      senderName: 'Takaro',
      announcerFuncomId: 'ADMIN#00001',
    });
    await rmq.start();
    expect(await rmq.exchangeExists('chat.map')).toBe(false);
    rmq.resetExchangeCache();
    battlegroup.exchanges.add('chat.map');
    expect(await rmq.exchangeExists('chat.map')).toBe(true);
  });
});

describe('sendMessage global falls back instead of lying', () => {
  it('fans the line out as whispers when chat.map is absent', async () => {
    const { adapter, battlegroup } = await harness({
      exchanges: WITHOUT_MAP_EXCHANGE,
      players: [mockPlayer({ flsId: 'A', funcomId: 'A#1' }), mockPlayer({ flsId: 'B', accountId: 2, characterName: 'Tester', funcomId: 'B#2' })],
    });

    const out = await adapter.sendMessage({ message: 'server restarting' });
    expect(out.chat).toBe(false);
    expect(out.chatRefused).toMatch(/chat\.map/);
    expect(out.fanout).toEqual({ delivered: 2, attempted: 2, failed: 0 });
    expect(out.delivered).toBe(true);

    const whispers = battlegroup.publishes.filter((p) => p.exchange === 'chat.whispers');
    expect(whispers.map((p) => p.routingKey).sort()).toEqual(['A#1', 'B#2']);
    // The connector binds nothing on chat.whispers: the client's own binding is the route, and the `fls` user is
    // refused write access to that queue anyway.
    expect(battlegroup.binds.filter((b) => b.exchange === 'chat.whispers')).toEqual([]);
  });

  it('reports delivered:false honestly when the exchange is gone AND nobody is online', async () => {
    const { adapter } = await harness({ exchanges: WITHOUT_MAP_EXCHANGE, players: [mockPlayer({ onlineStatus: 'Offline' })] });
    const out = await adapter.sendMessage({ message: 'nobody hears this' });
    expect(out.delivered).toBe(false);
    expect(out.fanout).toEqual({ delivered: 0, attempted: 0, failed: 0 });
  });

  it('DUNE_GLOBAL_MESSAGE_MODE=broadcast needs no chat exchange at all', async () => {
    const { adapter, gmSent } = await harness({ exchanges: WITHOUT_MAP_EXCHANGE, globalMessageMode: 'broadcast' });
    const out = await adapter.sendMessage({ message: 'maintenance in 5' });
    expect(out.broadcast).toBe(true);
    expect(out.delivered).toBe(true);
    expect(gmSent().at(-1)?.ServerCommand).toBe('ServiceBroadcast');
  });

  it('mode=both still broadcasts when the chat half is refused', async () => {
    const { adapter } = await harness({ exchanges: WITHOUT_MAP_EXCHANGE, globalMessageMode: 'both' });
    const out = await adapter.sendMessage({ message: 'both' });
    expect(out.chat).toBe(false);
    expect(out.broadcast).toBe(true);
    expect(out.delivered).toBe(true);
  });

  it('a whisper to one player is unaffected — chat.whispers is declared by text-router', async () => {
    const { adapter } = await harness({ exchanges: WITHOUT_MAP_EXCHANGE, players: [mockPlayer({ flsId: 'A' })] });
    const out = await adapter.sendMessage({ message: 'psst', opts: { recipient: { gameId: 'A' } } });
    expect(out.delivered).toBe(true);
    expect(out.channel).toBe('whisper');
  });
});

describe('listEntities against the plugin shape the live rig actually serves', () => {
  it('unwraps /entities and drops class-named rows', async () => {
    // MEASURED: the plugin answers `{liveActors, entitiesReturned, entities:[…]}`. Treating that object as an array
    // silently dropped all 90 live rows, so `listEntities` answered with the 1-row file catalogue alone.
    const plugin = new DunePluginClient({
      baseUrl: 'http://plugin',
      token: 't',
      fetchImpl: (async () =>
        new Response(
          JSON.stringify({
            liveActors: 2506,
            entitiesReturned: 3,
            entities: [
              { code: 'Atreides Hoplite', name: 'Atreides Hoplite', nameIsClassName: false },
              { code: 'BP_IdleCivilian_C', name: 'BP_IdleCivilian_C', nameIsClassName: true },
              { code: 'Sandworm', name: 'Sandworm', nameIsClassName: false },
            ],
          }),
          { status: 200, headers: { 'content-type': 'application/json' } },
        )) as unknown as typeof fetch,
    });
    expect((await plugin.getEntities()).length).toBe(3);

    const { adapter } = await harness({ plugin });
    const entities = await adapter.listEntities();
    const codes = entities.map((e) => e.code);
    expect(codes).toContain('Atreides Hoplite');
    expect(codes).toContain('Sandworm');
    expect(codes).not.toContain('BP_IdleCivilian_C');
  });
});

describe('getPlayerLocation never reports the map origin as a live position', () => {
  it('falls through to the last saved pawn transform when the plugin answers (0,0,0) from playerState', async () => {
    // MEASURED: during a respawn the plugin loses the pawn and answers `{x:0,y:0,z:0,source:"playerState"}`.
    // Takaro stored that as the player's position. The origin from the weak source is not a position.
    const plugin = new DunePluginClient({
      baseUrl: 'http://plugin',
      token: 't',
      fetchImpl: (async (url: string) =>
        new Response(
          JSON.stringify(
            String(url).includes('/location')
              ? { x: 0, y: 0, z: 0, source: 'playerState' }
              : { count: 1, players: [{ ref: 'acct:1', characterName: 'Tester', accountId: 1 }] },
          ),
          { status: 200, headers: { 'content-type': 'application/json' } },
        )) as unknown as typeof fetch,
    });
    const { adapter } = await harness({ plugin, players: [mockPlayer({ position: { x: 168960, y: 333430, z: 1271 } })] });
    expect(await adapter.getPlayerLocation('6FF6498F4074E3DE')).toEqual({ x: 168960, y: 333430, z: 1271 });
  });
});

describe('teleport read-back tells the truth about WHY it could not verify', () => {
  it('says the plugin never gave a live position, instead of "did not reach the target"', async () => {
    const plugin = new DunePluginClient({
      baseUrl: 'http://plugin',
      token: 't',
      fetchImpl: (async (url: string) =>
        new Response(
          JSON.stringify(
            String(url).includes('/location')
              ? { x: 0, y: 0, z: 0, source: 'playerState' }
              : { count: 1, players: [{ ref: 'acct:1', characterName: 'Tester', accountId: 1 }] },
          ),
          { status: 200, headers: { 'content-type': 'application/json' } },
        )) as unknown as typeof fetch,
    });
    const { adapter } = await harness({ plugin, teleportVerifyWindowMs: 20 });
    const out = await adapter.teleportPlayer({ gameId: '6FF6498F4074E3DE', x: 1, y: 2, z: 3, dimension: null });
    expect(out.verified).toBe(false);
    expect(String(out.reason)).toMatch(/never answered a live pawn position/);
  });
});
