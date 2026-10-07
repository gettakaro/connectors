/**
 * Parity fixtures for the native connector (games/enshrouded/mod/src/native/).
 *
 * Every case runs through the real sidecar code (Bridge + EnshroudedAdapter + mapping + MockPlugin) and the
 * observed outputs are written to mod/tests/fixtures/parity-*.json. The C++ host tests replay the same inputs
 * against the native port (with a C++ port of MockPlugin) and require identical payloads, error texts and plugin
 * requests. src/__tests__/parity.test.ts fails when the committed fixtures drift from what this code produces.
 *
 *   npm run parity-fixtures     # regenerate after an intentional behaviour change
 */
import { Bridge, shouldForwardLog, shouldTailLog } from '../bridge.js';
import { MemoryCursorStore } from '../enshrouded/cursorStore.js';
import { EnshroudedLogParser } from '../enshrouded/logTail.js';
import { mapEntityType, mapPluginEvent } from '../enshrouded/mapping.js';
import { EnshroudedPluginClient } from '../enshrouded/pluginClient.js';
import type { PluginHealth } from '../enshrouded/types.js';
import { createErrorResponse, createGameEvent, createIdentify, createResponse, normalizeArgs } from '../takaro/protocol.js';
import { MockPlugin } from './mockPlugin.js';

const LIMON = '76561198000005875';
const GUEST = '76561198000001111';
const player = { gameId: LIMON, name: 'Limon', steamId: LIMON, peerId: '0(1)' };

/** Overrides applied to a fresh MockPlugin before the steps run (mirrored by the C++ FakePlugin). */
export interface MockSetup {
  health?: PluginHealth;
  addPlayers?: Array<Record<string, unknown>>;
  removePlayers?: string[];
  bans?: Array<Record<string, unknown>>;
  unimplemented?: string[];
  commandResult?: { success: boolean; output: string };
}
export interface ActionStep {
  action: string;
  args: unknown;
  expect?: { payload?: unknown; error?: string };
  /** the last plugin request with this method + path, after the step */
  request?: { method: string; path: string; body?: unknown; found?: boolean };
}
export interface ActionCase {
  name: string;
  setup?: MockSetup;
  steps: ActionStep[];
}

const actionCases: ActionCase[] = [
  { name: 'getPlayers maps IGamePlayer with gameId=steamId, drops online:false', setup: { addPlayers: [{ gameId: '9(9)', name: 'Offline', steamId: '1', online: false }] }, steps: [{ action: 'getPlayers', args: [] }] },
  { name: 'getPlayers with string args', steps: [{ action: 'getPlayers', args: '[]' }, { action: 'getPlayers', args: null }, { action: 'getPlayers', args: '' }] },
  {
    name: 'getPlayer flat, nested, JSON string, steam: prefix, name, platformId, playerRef; unknown -> {}',
    steps: [
      { action: 'getPlayer', args: { gameId: LIMON } },
      { action: 'getPlayer', args: { player: { gameId: LIMON } } },
      { action: 'getPlayer', args: JSON.stringify({ gameId: LIMON }) },
      { action: 'getPlayer', args: { gameId: `steam:${LIMON}` } },
      { action: 'getPlayer', args: { gameId: 'limon' } },
      { action: 'getPlayer', args: { platformId: `steam:${GUEST}` } },
      { action: 'getPlayer', args: { playerRef: { steamId: GUEST } } },
      { action: 'getPlayer', args: { gameId: '123' } },
      { action: 'getPlayer', args: [] },
      { action: 'getPlayer', args: { gameId: null, steamId: null, player: { gameId: LIMON } } },
      { action: 'getPlayer', args: { gameId: '   ' } },
    ],
  },
  {
    name: 'getPlayerLocation resolves steamId to plugin gameId; unknown is a 404 error',
    steps: [
      { action: 'getPlayerLocation', args: { player: { gameId: LIMON } }, request: { method: 'GET', path: `/players/${LIMON}/location` } },
      { action: 'getPlayerLocation', args: { gameId: 'nobody' } },
      { action: 'getPlayerLocation', args: { gameId: 'a b/c' } },
    ],
  },
  { name: 'getPlayerLocation 501 without an event window is an error', setup: { unimplemented: ['GET /players/:id/location'] }, steps: [{ action: 'getPlayerLocation', args: { gameId: LIMON } }] },
  { name: 'getPlayerInventory returns items with string quality', steps: [{ action: 'getPlayerInventory', args: { gameId: LIMON } }, { action: 'getPlayerInventory', args: { gameId: GUEST } }] },
  {
    name: 'giveItem: item key, amount/quantity, quality, explicit nulls, errors',
    steps: [
      { action: 'giveItem', args: { player: { gameId: LIMON }, item: 'Torch', amount: 5, quality: '2' }, request: { method: 'POST', path: '/give' } },
      { action: 'giveItem', args: JSON.stringify({ gameId: LIMON, item: 'Wood', amount: 1 }), request: { method: 'POST', path: '/give' } },
      { action: 'giveItem', args: { gameId: LIMON, item: 'Wood', amount: 3, quality: null }, request: { method: 'POST', path: '/give' } },
      { action: 'giveItem', args: { gameId: LIMON, itemCode: 'Wood', quantity: '7' }, request: { method: 'POST', path: '/give' } },
      { action: 'giveItem', args: { gameId: LIMON, item: { code: 'Torch' }, amount: null, quality: 4 }, request: { method: 'POST', path: '/give' } },
      { action: 'giveItem', args: { gameId: LIMON, code: ' Wood ', amount: 2, quality: '' }, request: { method: 'POST', path: '/give' } },
      { action: 'giveItem', args: { gameId: LIMON } },
      { action: 'giveItem', args: { gameId: LIMON, item: 'Wood', amount: 0 } },
      { action: 'giveItem', args: { gameId: LIMON, item: 'Wood', amount: -2 } },
      { action: 'giveItem', args: { gameId: 'nobody', item: 'Wood', amount: 1 } },
    ],
  },
  { name: 'listItems / listEntities / listLocations', steps: [{ action: 'listItems', args: {} }, { action: 'listEntities', args: {} }, { action: 'listLocations', args: '{}' }] },
  {
    name: 'executeConsoleCommand returns CommandOutput',
    steps: [
      { action: 'executeConsoleCommand', args: { command: 'save' }, request: { method: 'POST', path: '/command' } },
      { action: 'executeConsoleCommand', args: {} },
      { action: 'executeConsoleCommand', args: { command: null } },
    ],
  },
  { name: 'executeConsoleCommand failure', setup: { commandResult: { success: false, output: 'unknown command' } }, steps: [{ action: 'executeConsoleCommand', args: { command: 'nope' } }] },
  { name: 'executeConsoleCommand failure without output', setup: { commandResult: { success: false, output: '' } }, steps: [{ action: 'executeConsoleCommand', args: { command: 'nope' } }] },
  {
    name: 'sendMessage: global, opts.recipient, senderNameOverride, explicit nulls',
    steps: [
      { action: 'sendMessage', args: { message: 'hello all' }, request: { method: 'POST', path: '/message' } },
      { action: 'sendMessage', args: { message: 'Welcome', opts: { recipient: { gameId: LIMON }, senderNameOverride: 'Takaro' } }, request: { method: 'POST', path: '/message' } },
      { action: 'sendMessage', args: { message: 'hi', opts: null }, request: { method: 'POST', path: '/message' } },
      { action: 'sendMessage', args: { message: 'hi', opts: { recipient: null, senderNameOverride: null } }, request: { method: 'POST', path: '/message' } },
      { action: 'sendMessage', args: { message: 'dm', opts: { recipient: { steamId: `steam:${GUEST}` } } }, request: { method: 'POST', path: '/message' } },
      { action: 'sendMessage', args: { message: 'dm2', recipientGameId: 'Limon' }, request: { method: 'POST', path: '/message' } },
      { action: 'sendMessage', args: { message: '  padded  ' }, request: { method: 'POST', path: '/message' } },
      { action: 'sendMessage', args: {} },
      { action: 'sendMessage', args: { message: '   ' } },
    ],
  },
  {
    name: 'teleportPlayer numeric and string coordinates',
    steps: [
      { action: 'teleportPlayer', args: { player: { gameId: LIMON }, x: 1, y: '2', z: 3.5 }, request: { method: 'POST', path: '/teleport' } },
      { action: 'teleportPlayer', args: { gameId: LIMON, x: 1 } },
      { action: 'teleportPlayer', args: { gameId: LIMON, x: -0.25, y: 1e3, z: '-7.5' }, request: { method: 'POST', path: '/teleport' } },
      { action: 'teleportPlayer', args: { gameId: LIMON, x: 'a', y: 1, z: 2 } },
      { action: 'teleportPlayer', args: { gameId: 'nobody', x: 1, y: 2, z: 3 } },
    ],
  },
  {
    name: 'kickPlayer with and without reason',
    steps: [
      { action: 'kickPlayer', args: { player: { gameId: LIMON }, reason: 'afk' }, request: { method: 'POST', path: '/kick' } },
      { action: 'kickPlayer', args: { gameId: GUEST, reason: null }, request: { method: 'POST', path: '/kick' } },
    ],
  },
  {
    name: 'banPlayer / listBans / unbanPlayer round trip incl. offline steamId',
    steps: [
      { action: 'banPlayer', args: { player: { gameId: LIMON }, reason: 'grief', expiresAt: '2030-01-01T00:00:00.000Z' }, request: { method: 'POST', path: '/ban' } },
      { action: 'banPlayer', args: { gameId: '76561198000009999' }, request: { method: 'POST', path: '/ban' } },
      { action: 'listBans', args: {} },
      { action: 'unbanPlayer', args: { gameId: '76561198000009999' }, request: { method: 'POST', path: '/unban' } },
      { action: 'listBans', args: {} },
      { action: 'banPlayer', args: { gameId: `steam:${GUEST}`, reason: null, expiresAt: null }, request: { method: 'POST', path: '/ban' } },
      { action: 'listBans', args: '[]' },
    ],
  },
  {
    name: 'listBans maps the real plugin /bans shape',
    setup: { bans: [{ gameId: LIMON, steamId: LIMON, accountId: '1234', name: 'Limon', characterName: 'Hero', bannedAt: '2026-09-13T20:00:00Z', reason: '', expiresAt: null }] },
    steps: [{ action: 'listBans', args: {} }],
  },
  { name: 'listBans minimal row', setup: { bans: [{ gameId: LIMON, name: 'Limon' }] }, steps: [{ action: 'listBans', args: {} }] },
  { name: 'listBans numeric expiry and nested player', setup: { bans: [{ player: { gameId: GUEST, name: 'Guest' }, reason: 'x', expiresAt: 1893456000000 }] }, steps: [{ action: 'listBans', args: {} }] },
  { name: 'shutdown', steps: [{ action: 'shutdown', args: {}, request: { method: 'POST', path: '/shutdown' } }] },
  {
    name: 'plugin 501 becomes a clear error response and the connector keeps serving',
    setup: { unimplemented: ['POST /teleport', 'GET /players/:id/inventory', 'GET /entities', 'POST /shutdown'] },
    steps: [
      { action: 'teleportPlayer', args: { gameId: LIMON, x: 1, y: 2, z: 3 } },
      { action: 'getPlayerInventory', args: { gameId: LIMON } },
      { action: 'listEntities', args: {} },
      { action: 'shutdown', args: {} },
      { action: 'getPlayers', args: {} },
    ],
  },
  { name: 'unknown action', steps: [{ action: 'flyToMoon', args: {} }] },
  {
    name: 'testReachability reflects plugin /health',
    steps: [{ action: 'testReachability', args: {} }],
  },
  { name: 'testReachability degraded capability', setup: { health: { status: 'ok', capabilities: { players: 'ok', kick: 'degraded', chatEvents: 'unimplemented' } } }, steps: [{ action: 'testReachability', args: {} }] },
  { name: 'testReachability plugin degraded', setup: { health: { status: 'degraded', capabilities: { logEvents: 'degraded' } } }, steps: [{ action: 'testReachability', args: {} }] },
  { name: 'testReachability plugin starting', setup: { health: { status: 'starting' } }, steps: [{ action: 'testReachability', args: {} }] },
  { name: 'testReachability no status', setup: { health: {} as PluginHealth }, steps: [{ action: 'testReachability', args: {} }] },
  { name: 'players list without Limon', setup: { removePlayers: [LIMON] }, steps: [{ action: 'getPlayer', args: { gameId: LIMON } }, { action: 'kickPlayer', args: { gameId: LIMON } }] },
];

function applySetup(mock: MockPlugin, setup: MockSetup | undefined): void {
  if (!setup) return;
  if (setup.health) mock.health = setup.health;
  for (const p of setup.addPlayers ?? []) mock.players.push(p as never);
  for (const id of setup.removePlayers ?? []) mock.players = mock.players.filter((p) => p.gameId !== id);
  if (setup.bans) mock.bans = setup.bans as never;
  for (const u of setup.unimplemented ?? []) mock.unimplemented.add(u);
  if (setup.commandResult) {
    const r = setup.commandResult;
    mock.commandHandler = () => r;
  }
}

async function runActionCase(c: ActionCase): Promise<ActionCase> {
  const mock = new MockPlugin();
  await mock.start();
  try {
    applySetup(mock, c.setup);
    const bridge = new Bridge({
      plugin: new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token, timeoutMs: 2000 }),
      takaro: { send: () => true, sendGameEvent: () => true },
      cursorStore: new MemoryCursorStore(),
      logFile: '/nonexistent',
      logTailMode: 'never',
    });
    const steps: ActionStep[] = [];
    let n = 0;
    for (const s of c.steps) {
      const reply = await bridge.handleRequest({ type: 'request', requestId: `req-${++n}`, payload: { action: s.action, args: s.args } });
      const step: ActionStep = { action: s.action, args: s.args, expect: reply?.error !== undefined ? { error: reply.error } : { payload: reply?.payload } };
      if (s.request) {
        const r = mock.lastRequest(s.request.method, s.request.path);
        step.request = r ? { method: r.method, path: r.path, body: r.body, found: true } : { method: s.request.method, path: s.request.path, found: false };
      }
      steps.push(step);
    }
    bridge.stopEvents();
    return { name: c.name, ...(c.setup ? { setup: c.setup } : {}), steps };
  } finally {
    await mock.stop();
  }
}

const eventInputs: Array<{ type: string; data: unknown }> = [
  { type: 'player-connected', data: { player } },
  { type: 'player-disconnected', data: player },
  { type: 'player-connected', data: { player: { gameId: '9(9)', name: 'NoSteam' } } },
  { type: 'player-connected', data: { player: { gameId: 'x', name: 'P', platformId: 'steam:76561198000000042', ip: '1.2.3.4', ping: 30 } } },
  { type: 'player-connected', data: { player: { gameId: LIMON, name: 'Limon', steamId: LIMON, peerId: '0(1)', group: 'Admins', permissions: ['CanKickBan'], connectedAt: '2026-09-13T00:00:00.000Z', online: true } } },
  { type: 'chat-message', data: { player, message: 'hi', channel: 'Global' } },
  { type: 'chat-message', data: { msg: 'sys', channel: 'team' } },
  { type: 'chat-message', data: { msg: 'gg', channel: 'global', chatType: 0, senderName: 'Limon', player } },
  { type: 'chat-message', data: { text: '  spaced ', channel: 'WHISPER' } },
  { type: 'chat-message', data: {} },
  { type: 'player-death', data: { player, position: { x: 1, y: 2, z: 3 }, attacker: { gameId: 'x', name: 'Bob', steamId: '76561198000000042' } } },
  { type: 'player-death', data: { player, killerEntity: 'Enemy_Fogger_Depleted', position: { x: 1, y: 2, z: 3 } } },
  { type: 'player-death', data: { player } },
  { type: 'player-death', data: { player, attacker: { gameId: LIMON, name: 'Bob' }, killerEntity: 'X' } },
  { type: 'player-death', data: { player, killer: { code: 'Wolf_Alpha' } } },
  { type: 'player-death', data: { player, attacker: { foo: 1 }, killerEntity: 'Scavenger' } },
  { type: 'player-death', data: { player, position: { x: 'a', y: 2, z: 3 } } },
  { type: 'player-death', data: { player, position: null } },
  { type: 'entity-killed', data: { player, entity: 'Scavenger', weapon: 'Sword_Iron' } },
  { type: 'entity-killed', data: { player, entity: { code: 'Wolf' } } },
  { type: 'entity-killed', data: { player } },
  { type: 'log', data: { msg: 'line' } },
  { type: 'log', data: { msg: '[server] Saved', level: 'info' } },
  { type: 'log', data: 'raw' },
  { type: 'log', data: { line: 'from line' } },
  { type: 'log', data: { level: 'info' } },
  { type: 'log', data: 42 },
  { type: 'player-sync', data: {} },
  { type: 'player-connected', data: { player: {} } },
  { type: 'player-connected', data: {} },
  // Native plugin 0.6 payloads: display names, the killing weapon and the raw codes beside them.
  {"type": "entity-killed", "data": {"entity": "Rat", "entityCode": "Enemy_Wildbeast_Rat_hook", "entityNameSource": "code", "weapon": "Scrappy Sword", "weaponSource": "item", "weaponItemId": 1275936258, "weaponPideId": 4711, "sourceEntityId": 16, "victimEntityId": 2402, "templateGuid": "00000000-0000-0000-0000-000000000000", "killerName": "Limon", "weaponCategory": 1404206905, "weaponCategoryName": "Sword", "player": {"gameId": "76561198000005875", "name": "Limon", "steamId": "76561198000005875", "peerId": "0(1)"}}},
  {"type": "entity-killed", "data": {"entity": "Rat", "entityCode": "Enemy_Wildbeast_Rat_hook", "entityNameSource": "code", "weapon": "Forest Longbow", "weaponSource": "item", "weaponItemId": 327157552, "weaponPideId": 4711, "sourceEntityId": 16, "victimEntityId": 2402, "templateGuid": "00000000-0000-0000-0000-000000000000", "killerName": "Limon", "weaponCategory": 1382431183, "weaponCategoryName": "Bow", "player": {"gameId": "76561198000005875", "name": "Limon", "steamId": "76561198000005875", "peerId": "0(1)"}}},
  {"type": "entity-killed", "data": {"entity": "Rat", "entityCode": "Enemy_Wildbeast_Rat_hook", "entityNameSource": "code", "weapon": "Unarmed", "weaponSource": "category", "weaponItemId": 0, "weaponPideId": 0, "sourceEntityId": 16, "victimEntityId": 2402, "templateGuid": "00000000-0000-0000-0000-000000000000", "killerName": "Limon", "weaponCategory": 3128520336, "weaponCategoryName": "Unarmed", "player": {"gameId": "76561198000005875", "name": "Limon", "steamId": "76561198000005875", "peerId": "0(1)"}}},
  {"type": "player-death", "data": {"entityId": 2402, "playerName": "Limon", "player": {"gameId": "76561198000005875", "name": "Limon", "steamId": "76561198000005875", "peerId": "0(1)"}, "position": {"x": 1, "y": 2, "z": 3}, "killerEntity": "Enemy_Fogger_Heavy_BossHealthBar", "killerEntityName": "Fell Thunderbrute"}},
  {"type": "entity-killed", "data": {"entity": "Fell Thunderbrute", "entityCode": "Enemy_Fogger_Heavy_BossHealthBar", "entityNameSource": "client", "weapon": "Apex Machete", "weaponSource": "item", "player": {"gameId": "76561198000005875", "name": "Limon", "steamId": "76561198000005875", "peerId": "0(1)"}}},
];

const normalizeInputs: unknown[] = [[], {}, null, '', '[]', '{}', '{"gameId":"1"}', 'not json', { a: 1 }, '  {"a":[1,2]}  ', 42, true, '"str"', '[1,2]'];

const forwardLogInputs: Array<{ mode: 'all' | 'filtered' | 'none'; msg: unknown }> = [
  { mode: 'filtered', msg: '-------------- Session ----------------' },
  { mode: 'filtered', msg: '  m#0(128): up 0 (0), down 0 (0)' },
  { mode: 'filtered', msg: "[server] Player 'Limon' logged in with Permissions:" },
  { mode: 'all', msg: 'Machines:' },
  { mode: 'none', msg: '[server] Saved' },
  { mode: 'filtered', msg: 'Machines:' },
  { mode: 'filtered', msg: 'Machines: 2' },
  { mode: 'filtered', msg: '[ecss] Stats: 1' },
  { mode: 'filtered', msg: '[Water] tick' },
  { mode: 'filtered', msg: 'Could not prune enough replication states (12)' },
  { mode: 'filtered', msg: '   ' },
  { mode: 'filtered', msg: '' },
  { mode: 'filtered', msg: 42 },
  { mode: 'filtered', msg: '----' },
  { mode: 'filtered', msg: 'x -----' },
];

const tailInputs: Array<{ mode: 'auto' | 'always' | 'never'; health: PluginHealth | null }> = [
  { mode: 'never', health: null },
  { mode: 'always', health: { status: 'ok' } },
  { mode: 'auto', health: null },
  { mode: 'auto', health: { status: 'ok', capabilities: { logEvents: 'ok', players: 'ok', chatEvents: 'unimplemented' } } },
  { mode: 'auto', health: { status: 'ok', capabilities: { logEvents: 'degraded', players: 'degraded' } } },
  { mode: 'auto', health: { status: 'ok', capabilities: { logEvents: 'ok', players: 'degraded' } } },
  { mode: 'auto', health: { status: 'starting' } },
  { mode: 'auto', health: { status: 'DEGRADED', capabilities: {} } },
  { mode: 'auto', health: { status: 'ok' } },
];

const SID = '76561198000005875';
const JOIN = [
  `[I 00:03:52,650] [online] Session accepted with peer (steamid:${SID})`,
  `[I 00:03:52,650] [online] Added peer 0(1) (steamid:${SID})`,
  '[E 00:03:53,170] [online] Begin auth session with peer 0(1)',
  `[I 00:03:54,478] [online] Client '${SID}' authenticated by steam`,
  '[I 00:03:54,548] [session] Remote player added. Player handle: 0(0)',
  "[I 00:04:24,062] [server] Machine '1': Player '0(0)' logged in",
  "[I 00:04:24,063] [server] Player 'Limon' logged in with Permissions:",
  '[I 00:04:24,063] \t - CanKickBan',
];
const LEAVE = [
  "[I 00:07:46,469] [Server] Sending Character Savegame 'Limon' Size:22'899",
  "[I 00:07:46,564] [server] Remove Entity for Player 'Limon'",
  "[I 00:07:46,566] [server] Remove Player 'Limon'",
  '[I 00:07:46,704] [online] Disconnecting peer 0(1)',
  '[I 00:07:46,704] [online] Removed peer 0(1)',
];
const logSequences: Array<{ name: string; lines: string[] }> = [
  { name: 'join then leave', lines: [...JOIN, ...LEAVE] },
  {
    name: 'two overlapping joins by machine index',
    lines: [
      '[I 1] [online] Added peer 0(1) (steamid:111)',
      '[I 2] [online] Added peer 1(2) (steamid:222)',
      "[I 3] [server] Machine '2': Player '1(0)' logged in",
      "[I 3] [server] Player 'Second' logged in with Permissions:",
      "[I 4] [server] Machine '1': Player '0(0)' logged in",
      "[I 4] [server] Player 'First' logged in with Permissions:",
    ],
  },
  { name: 'peer removed without Remove Player; unnamed drop silent', lines: [...JOIN, '[I 9] [online] Removed peer 0(1)', '[I 1] [online] Added peer 0(3) (steamid:333)', '[I 2] [online] Removed peer 0(3)'] },
  { name: 'ignored lines and CRLF', lines: ["[I 1] [Server] Sending Character Savegame 'Limon' Size:1", '[I 2] [session] Congestion x\r', "[I 3] [server] Player 'Ghost' logged in with Permissions:\r"] },
  { name: 'login without machine line picks oldest unnamed', lines: ['[I 1] [online] Added peer 0(5) (steamid:555)', '[I 2] [online] Added peer 0(6) (steamid:666)', "[I 3] [server] Player 'A' logged in with Permissions:", "[I 4] [server] Player 'B' logged in with Permissions:", "[I 5] [server] Remove Player 'B'"] },
  { name: 'name with quote', lines: ['[I 1] [online] Added peer 0(1) (steamid:777)', "[I 2] [server] Machine '1': Player '0(0)' logged in", "[I 3] [server] Player 'Al'ice' logged in with Permissions:", "[I 4] [server] Remove Player 'Al'ice'"] },
];

export async function buildParityFixtures(): Promise<Record<string, unknown>> {
  const actions: ActionCase[] = [];
  for (const c of actionCases) actions.push(await runActionCase(c));

  const events = eventInputs.map((input) => {
    try {
      return { input, expect: mapPluginEvent(input) };
    } catch (err) {
      return { input, error: (err as Error).message };
    }
  });

  const protocol = {
    normalizeArgs: normalizeInputs.map((input) => ({ input, expect: normalizeArgs(input) })),
    frames: [
      { kind: 'identify', input: { identityToken: 'id', registrationToken: 'reg', serverName: 'S' }, expect: createIdentify({ identityToken: 'id', registrationToken: 'reg', serverName: 'S' }) },
      { kind: 'identify', input: { identityToken: 'id', registrationToken: '', serverName: '' }, expect: createIdentify({ identityToken: 'id', registrationToken: '' }) },
      { kind: 'response', input: { requestId: 'r', payload: null }, expect: createResponse('r', null) },
      { kind: 'response', input: { requestId: 'r', payload: [1] }, expect: createResponse('r', [1]) },
      { kind: 'error', input: { requestId: 'r', error: 'boom' }, expect: createErrorResponse('r', 'boom') },
      { kind: 'gameEvent', input: { type: 'log', data: { msg: 'x' } }, expect: createGameEvent('log', { msg: 'x' }) },
    ],
    entityTypes: ['Enemy', 'friendly', null, 'animal', 'Shroud_Boss', 'npc', 'Pet', 'hostile'].map((input) => ({ input, expect: mapEntityType(input) })),
  };

  const logs = {
    forward: forwardLogInputs.map((i) => ({ ...i, expect: shouldForwardLog(i.mode, { msg: i.msg }) })),
    tail: tailInputs.map((i) => ({ ...i, expect: shouldTailLog(i.mode, i.health) })),
    parser: logSequences.map((s) => {
      const p = new EnshroudedLogParser();
      return { ...s, expect: s.lines.flatMap((l) => p.feed(l)) };
    }),
  };

  return { actions: { cases: actions }, events: { cases: events }, protocol, logs };
}
