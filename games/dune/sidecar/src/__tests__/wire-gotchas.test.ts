import { describe, expect, it } from 'vitest';
import { COMMANDS, HELP_TEXT, tokenize } from '../dune/commands.js';
import { loadConfig, inventoryTypesOf, pgUrlOf } from '../dune/config.js';
import { isFlsId, isFuncomId, isSteamId64, mapPlayer, stripPlatform } from '../dune/identity.js';
import { redactSecrets } from '../logger.js';
import { redactLog, shouldForwardLog } from '../dune/logTail.js';
import { causeFromLifeState, formatUeTimestamp, mapChannel, parseUeTimestamp } from '../dune/mapping.js';
import { quoteIdent } from '../dune/pg.js';
import { DunePluginClient } from '../dune/pluginClient.js';
import type { CommandResult } from '../dune/commands.js';
import { mockPlayer } from '../testing/mockBattlegroup.js';
import { harness } from './helpers.js';

const FLS = '6FF6498F4074E3DE';

describe('identity mapping', () => {
  it('gameId is the FLS id, steamId the platform id, platformId prefixed, name the character name', () => {
    expect(mapPlayer(mockPlayer())).toEqual({
      gameId: FLS,
      name: 'Tester',
      steamId: '76561190000000001',
      platformId: 'steam:76561190000000001',
    });
  });

  it('falls back through funcomId → platformId → characterName so getPlayer always has an id', () => {
    expect(mapPlayer({ funcomId: 'PLAYER#1', characterName: 'Chani' })).toMatchObject({ gameId: 'PLAYER#1', name: 'Chani' });
    expect(mapPlayer({ characterName: 'Chani' })).toEqual({ gameId: 'Chani', name: 'Chani' });
    expect(() => mapPlayer({})).toThrow(/no usable identifier/);
  });

  it('does not claim a non-Steam platform id is a Steam id', () => {
    const player = mapPlayer({ flsId: FLS, platformId: 'abc-123', platformName: 'Epic', characterName: 'X' });
    expect(player.steamId).toBeUndefined();
    expect(player.platformId).toBe('epic:abc-123');
  });

  it('only ever emits a platformId Takaro accepts (`<platform>:<id>`, [A-Za-z0-9_-] on both sides)', () => {
    // No platform name: a bare id has no colon and would make Takaro drop the whole event.
    expect(mapPlayer({ flsId: FLS, platformId: 'abc-123', characterName: 'X' }).platformId).toBeUndefined();
    // Platform name with spaces/dots is normalised into a valid prefix.
    expect(mapPlayer({ flsId: FLS, platformId: 'abc_123', platformName: 'Xbox Live', characterName: 'X' }).platformId).toBe(
      'xboxlive:abc_123',
    );
    // An id Takaro's pattern cannot hold is left out, never sent malformed.
    expect(mapPlayer({ flsId: FLS, platformId: 'a|b', platformName: 'psn', characterName: 'X' }).platformId).toBeUndefined();
    expect(mapPlayer({ flsId: FLS, platformId: 'abc', platformName: '???', characterName: 'X' }).platformId).toBeUndefined();
    // The Steam path is unchanged: raw SteamID64 in steamId, canonical `steam:<id64>` in platformId.
    const steam = mapPlayer({ flsId: FLS, platformId: '76561190000000001', platformName: 'steam', characterName: 'X' });
    expect(steam).toMatchObject({ gameId: FLS, steamId: '76561190000000001', platformId: 'steam:76561190000000001' });
    for (const p of [steam]) expect(p.platformId).toMatch(/^[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+$/);
  });

  it('recognises each id shape and strips the platform prefix', () => {
    expect(isFlsId(FLS)).toBe(true);
    expect(isFlsId('too-short')).toBe(false);
    expect(isFuncomId('PLAYER#12345')).toBe(true);
    expect(isFuncomId('PLAYER')).toBe(false);
    expect(isSteamId64('76561190000000001')).toBe(true);
    expect(isSteamId64('123')).toBe(false);
    expect(stripPlatform('steam:76561190000000001')).toBe('76561190000000001');
    expect(stripPlatform(FLS)).toBe(FLS);
  });
});

describe('channel and timestamp mapping', () => {
  it('maps Dune channels onto Takaro\'s four, in both enum spellings', () => {
    expect(mapChannel('Whispers')).toBe('whisper');
    expect(mapChannel('ETextChatChannelType::Whispers')).toBe('whisper');
    expect(mapChannel('Map')).toBe('global');
    expect(mapChannel('Proximity')).toBe('team');
    expect(mapChannel('Guild')).toBe('team');
    expect(mapChannel('Party')).toBe('team');
    expect(mapChannel(undefined)).toBe('global');
  });

  it('round-trips UE timestamps', () => {
    expect(parseUeTimestamp('2026.05.21-02.43.11')).toBe('2026-05-21T02:43:11.000Z');
    expect(parseUeTimestamp('nope')).toBeNull();
    expect(formatUeTimestamp(new Date('2026-05-21T02:43:11Z'))).toBe('2026.05.21-02.43.11');
  });

  it('names only the causes life_state actually tells us', () => {
    expect(causeFromLifeState('DeadBySandworm')).toBe('sandworm');
    expect(causeFromLifeState('DeadByCoriolis')).toBe('coriolis storm');
    // A plain `Dead` says nothing about how: do not invent a cause.
    expect(causeFromLifeState('Dead')).toBeNull();
    expect(causeFromLifeState('Alive')).toBeNull();
    expect(causeFromLifeState(null)).toBeNull();
  });
});

describe('secret redaction in every log path', () => {
  it('never lets a GM auth token, a DB/AMQP password or a Takaro token reach a log line', () => {
    expect(redactSecrets('{"Version":2,"AuthToken":"abc123","MessageContent":"{}"}')).toBe(
      '{"Version":2,"AuthToken":"<redacted>","MessageContent":"{}"}',
    );
    expect(redactSecrets('connecting to amqps://fls:brokerpass@game-rmq:31982/')).toContain('amqps://fls:<redacted>@');
    expect(redactSecrets('postgres://dune:dbpass@postgres:5432/dune')).toContain('postgres://dune:<redacted>@');
    expect(redactSecrets('{"identityToken":"tok","registrationToken":"reg"}')).toBe(
      '{"identityToken":"<redacted>","registrationToken":"<redacted>"}',
    );
    expect(redactSecrets('ServerCommandsAuthToken=deadbeef')).toBe('ServerCommandsAuthToken=<redacted>');
    expect(redactSecrets('DatabasePassword=hunter2 and more')).toContain('DatabasePassword=<redacted>');
    expect(redactSecrets('nothing secret here')).toBe('nothing secret here');
  });

  it('redacts the battlegroup command line before a log line can be forwarded', () => {
    expect(redactLog('-ini:engine:[FuncomLiveServices]:ServiceAuthToken=jwt.goes.here')).toContain('[redacted]');
    expect(redactLog('Login request ?p=am9pbnBhc3M= accepted')).toContain('?p=[redacted]');
    expect(redactLog('Ticket=AAAABBBBCCCC')).toBe('Ticket=[redacted]');
    // A secret mentioned with no key=value shape is dropped wholesale rather than leaked.
    expect(redactLog('the AuthToken is printed like this: jwt.goes.here')).toMatch(/redacted/);
  });

  it('filters Unreal noise out of forwarded log events but keeps real lines', () => {
    expect(shouldForwardLog('filtered', 'LogHttp: Verbose: request finished')).toBe(false);
    expect(shouldForwardLog('filtered', 'LogDune: Server command received')).toBe(true);
    expect(shouldForwardLog('all', 'LogHttp: anything')).toBe(true);
    expect(shouldForwardLog('none', 'LogDune: anything')).toBe(false);
  });
});

describe('config', () => {
  it('has safe defaults and rejects an invalid enum rather than guessing', () => {
    const config = loadConfig({});
    expect(config.gmPublisher).toBe('amqp');
    expect(config.gmExchange).toBe('heartbeats');
    expect(config.gmRoutingKey).toBe('notifications');
    expect(config.gmUserId).toBe('fls');
    expect(config.gmAppId).toBe('fls_backend');
    expect(config.gmPlayerIdKind).toBe('fls');
    expect(config.chatWire.timestampField).toBe('m_TimeStamp');
    expect(config.transferGraceMs).toBe(45_000);
    expect(config.rmqTlsInsecure).toBe(true);
    for (const [key, value] of [
      ['DUNE_GM_PUBLISHER', 'sftp'],
      ['DUNE_GM_PLAYER_ID_KIND', 'uuid'],
      ['DUNE_GLOBAL_MESSAGE_MODE', 'smoke-signal'],
      ['DUNE_CHAT_TIMESTAMP_FORMAT', 'unix'],
      ['DUNE_CHAT_CHANNEL_ENUM_FORM', 'medium'],
      ['DUNE_CHAT_CONTENT_KEY', 'CONTENT'],
      ['DUNE_LOG_EVENTS', 'some'],
    ] as const) {
      expect(() => loadConfig({ [key]: value })).toThrow(new RegExp(key));
    }
  });

  it('never guesses the database name — it has no fixed value across builds', () => {
    expect(pgUrlOf({ DUNE_PG_HOST: 'postgres' })).toBe('');
    expect(pgUrlOf({ DUNE_PG_HOST: 'postgres', DUNE_PG_DATABASE: 'dune_sb_1_4_0_0', DUNE_PG_PASSWORD: 'p@ss/word', DUNE_PG_USER: 'dune' })).toBe(
      'postgres://dune:p%40ss%2Fword@postgres:5432/dune_sb_1_4_0_0',
    );
    expect(pgUrlOf({ DUNE_PG_URL: 'postgres://explicit/db' })).toBe('postgres://explicit/db');
  });

  it('inventory types are configurable and validated', () => {
    // Only the three PHYSICAL carry inventories are reported. 14/27 are emotes and 29 is a contract item; they
    // showed up in Takaro's inventory screen as things the player "has" and cannot be given or dropped.
    expect(inventoryTypesOf({})).toEqual([0, 1, 15]);
    expect(inventoryTypesOf({ DUNE_INVENTORY_TYPES: '0, 14,14' })).toEqual([0, 14]);
    expect(() => inventoryTypesOf({ DUNE_INVENTORY_TYPES: 'backpack' })).toThrow(/no valid integers/);
  });

  it('a SQL identifier is validated before it is ever interpolated', () => {
    expect(quoteIdent('player_state')).toBe('"player_state"');
    expect(() => quoteIdent('players"; DROP TABLE x --')).toThrow(/Refusing to interpolate/);
  });
});

describe('executeConsoleCommand', () => {
  it('an unknown command answers {success:false}, never an error frame', async () => {
    const h = await harness();
    const result = (await h.adapter.handleAction('executeConsoleCommand', { command: 'rcon-style-nonsense' })) as CommandResult;
    expect(result.success).toBe(false);
    expect(result.rawResult).toBe('');
    expect(result.errorMessage).toMatch(/Unknown command/);
    // Takaro needs a CommandOutput payload here; an error frame becomes a 400 "responded with bad data".
    expect(result).toHaveProperty('rawResult');
  });

  it('help lists exactly the documented command set', async () => {
    const h = await harness();
    const result = (await h.adapter.handleAction('executeConsoleCommand', { command: 'help' })) as CommandResult;
    expect(result.success).toBe(true);
    expect(result.rawResult).toBe(HELP_TEXT);
    for (const name of COMMANDS) expect(HELP_TEXT).toContain(name);
  });

  it('players, say, whisper, give, tp, kick, ban, unban, bans and gm all reach the right seam', async () => {
    const h = await harness();
    const run = async (command: string): Promise<CommandResult> =>
      (await h.adapter.handleAction('executeConsoleCommand', { command })) as CommandResult;

    expect((await run('players')).rawResult).toContain(`Tester (${FLS})`);
    expect((await run('say hello everyone')).success).toBe(true);
    expect(h.battlegroup.publishes.at(-1)!.exchange).toBe('chat.map');
    expect((await run(`whisper ${FLS} a private line`)).success).toBe(true);
    expect(h.battlegroup.publishes.at(-1)!.exchange).toBe('chat.whispers');
    expect((await run('broadcast Maintenance | back in 5')).success).toBe(true);
    expect(h.gmSent().at(-1)).toMatchObject({ BroadcastType: 'Generic' });
    expect((await run(`give ${FLS} WaterFlask 3`)).success).toBe(true);
    expect(h.gmSent().at(-1)).toMatchObject({ ServerCommand: 'AddItemToInventory', Quantity: 3 });
    expect((await run(`tp ${FLS} 1 2 3`)).success).toBe(true);
    expect(h.gmSent().at(-1)).toMatchObject({ ServerCommand: 'TeleportToExact', X: 1, Y: 2, Z: 3 });
    expect((await run(`spawnvehicle ${FLS} Sandbike T6_Combat 1 2 3`)).success).toBe(true);
    expect(h.gmSent().at(-1)).toMatchObject({ ServerCommand: 'SpawnVehicleAt', ClassName: 'Sandbike' });
    expect((await run(`ban ${FLS} being rude 2030-01-01T00:00:00Z`)).success).toBe(true);
    expect((await run('bans')).rawResult).toContain('2030-01-01T00:00:00.000Z');
    expect((await run(`unban ${FLS}`)).success).toBe(true);
    expect((await run('bans')).rawResult).toBe('No bans.');
    expect((await run('gm AwardXP {"PlayerId":"' + FLS + '","Experience":10}')).success).toBe(true);
    expect(h.gmSent().at(-1)).toEqual({ ServerCommand: 'AwardXP', PlayerId: FLS, Experience: 10 });
  });

  it('a malformed gm payload and a missing argument are command failures, not crashes', async () => {
    const h = await harness();
    const run = async (command: string): Promise<CommandResult> =>
      (await h.adapter.handleAction('executeConsoleCommand', { command })) as CommandResult;
    expect((await run('gm AwardXP {not json}')).errorMessage).toMatch(/not valid JSON/);
    expect((await run('tp')).errorMessage).toMatch(/Usage: tp/);
    expect((await run('give')).errorMessage).toMatch(/Usage: give/);
    expect((await run('')).errorMessage).toMatch(/Empty command/);
    expect((await run('gm "Award XP" {}')).errorMessage).toMatch(/not a valid ServerCommand/);
  });

  it('tokenize honours quotes so a message can contain spaces', () => {
    expect(tokenize('whisper FLS "hello there"')).toEqual(['whisper', 'FLS', 'hello there']);
    expect(tokenize('  say    spaced   out ')).toEqual(['say', 'spaced', 'out']);
  });
});

describe('capability degradation', () => {
  it('reports exactly what is available, and names the plugin-only gaps as absent', async () => {
    const withoutPlugin = await harness();
    expect(withoutPlugin.adapter.capabilities()).toMatchObject({
      gmPublisher: 'amqp',
      gmAuthToken: true,
      chatConsumer: true,
      rmqConnected: true,
      pgProbed: true,
      plugin: false,
      livePlayerLocation: 'chat-origin-or-last-saved',
      entityKilled: 'absent',
      deathAttribution: 'life-state-only',
      bans: 'connector-enforced',
      shutdownHook: false,
    });

    const withPlugin = await harness({
      plugin: new DunePluginClient({ baseUrl: 'http://plugin', token: 't' }),
      shutdownCmd: 'docker stop x',
    });
    expect(withPlugin.adapter.capabilities()).toMatchObject({
      plugin: true,
      // The chain is named in full, because "plugin" alone hid the fact that a plugin 404 silently
      // falls back to a chat origin and then to the last SAVED database row (lane L2).
      livePlayerLocation: 'plugin-then-chat-origin-then-last-saved',
      entityKilled: 'plugin',
      deathAttribution: 'plugin-then-life-state',
      shutdownHook: true,
    });
  });

  it('every action that needs a GM token fails with a clear reason when none is configured', async () => {
    const h = await harness({ gmAuthToken: '' });
    expect(h.adapter.capabilities()).toMatchObject({ gmAuthToken: false });
    for (const action of ['giveItem', 'teleportPlayer', 'kickPlayer'] as const) {
      const args = action === 'giveItem' ? { gameId: FLS, item: 'x' } : { gameId: FLS, x: 1, y: 2, z: 3 };
      await expect(h.adapter.handleAction(action, args)).rejects.toThrow(/DUNE_GM_AUTH_TOKEN/);
    }
    // ...and testReachability says so instead of reporting a healthy server.
    expect((await h.adapter.testReachability()).reason).toMatch(/DUNE_GM_AUTH_TOKEN/);
  });

  it('the schema probe lets a build without the optional columns still answer', async () => {
    const h = await harness();
    // A build with no life_state / death_location / actors.transform at all.
    h.battlegroup.columns = {
      ...h.battlegroup.columns,
      player_state: ['character_name', 'online_status', 'account_id', 'player_pawn_id'],
      actors: ['map'],
      farm_state: [],
      world_partition: [],
    };
    await h.pg.probe();
    expect(h.pg.has('player_state', 'life_state')).toBe(false);

    const players = await h.adapter.getPlayers();
    expect(players[0].gameId).toBe(FLS);
    // No readiness tables: reachability degrades to "the database answers", and says so.
    const reach = await h.adapter.testReachability();
    expect(reach.connectable).toBe(true);
    expect(reach.reason).toMatch(/reported DB reachability only/);
    // No saved transform and no chat origin: the location action fails rather than inventing a position.
    await expect(h.adapter.getPlayerLocation(FLS)).rejects.toThrow(/No location/);
  });
});

describe('SQL safety', () => {
  it('every player identifier reaches Postgres as a bound parameter, never inlined', async () => {
    const h = await harness();
    const injection = "x' OR 1=1; DROP TABLE dune.player_state --";
    await h.adapter.getPlayer(injection);
    const roster = h.battlegroup.queries.filter((q) => q.sql.includes('"player_state" ps'));
    expect(roster.length).toBeGreaterThan(0);
    for (const query of roster) {
      expect(query.sql).not.toContain('DROP TABLE');
      expect(query.sql).toMatch(/\$\d/);
    }
    expect(roster.some((q) => q.params.includes(injection))).toBe(true);
  });
});
