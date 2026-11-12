import { describe, expect, it } from 'vitest';
import { GAME_SERVER_ACTIONS, normalizeArgs } from '../takaro/protocol.js';
import type { TakaroPlayer } from '../dune/types.js';
import { harness, type Harness } from './helpers.js';

const FLS = '6FF6498F4074E3DE';

/**
 * Takaro modules send an explicit JSON `null` for every optional argument, while the dashboard, the MCP and REST omit
 * the key entirely. A connector that tests "is the key present" and then reads the value throws — and the module tells
 * the player it succeeded. This matrix runs every action through absent, explicit-null and wrong-type arguments and
 * requires that nothing ever throws a TypeError and nothing silently no-ops.
 */
const NULL_ARG_CASES: Record<string, Record<string, unknown>[]> = {
  testReachability: [{}, { unexpected: null }],
  getPlayers: [{}, { filters: null }],
  getPlayer: [{ gameId: FLS }, { gameId: FLS, extra: null }, { player: { gameId: FLS, steamId: null, platformId: null } }],
  getPlayerLocation: [{ gameId: FLS }, { gameId: FLS, dimension: null }],
  getPlayerInventory: [{ gameId: FLS }, { gameId: FLS, opts: null }],
  getPlayerProgression: [{ gameId: FLS }, { gameId: FLS, opts: null }],
  giveItem: [
    { gameId: FLS, item: 'WaterFlask' },
    { gameId: FLS, item: 'WaterFlask', amount: null, quality: null, dimension: null },
    { gameId: FLS, item: 'WaterFlask', amount: 2, quality: '3' },
    { gameId: FLS, item: 'WaterFlask', quality: 0 },
  ],
  listItems: [{}, { search: null }, { search: '' }],
  listEntities: [{}, { search: null }],
  listLocations: [{}, { opts: null }],
  executeConsoleCommand: [{ command: 'players' }, { command: 'help', opts: null }],
  sendMessage: [
    { message: 'hello' },
    { message: 'hello', opts: null },
    { message: 'hello', opts: { recipient: null, senderNameOverride: null } },
    { message: 'hello', opts: { recipient: { gameId: FLS }, senderNameOverride: null } },
  ],
  teleportPlayer: [
    { gameId: FLS, x: 1, y: 2, z: 3 },
    { gameId: FLS, x: 1, y: 2, z: 3, dimension: null, yaw: null },
    { gameId: FLS, x: 1, y: 2, z: 3, dimension: 'overworld' },
  ],
  kickPlayer: [{ gameId: FLS }, { gameId: FLS, reason: null }, { gameId: FLS, reason: '' }],
  banPlayer: [
    { gameId: FLS },
    { gameId: FLS, reason: null, expiresAt: null },
    { gameId: FLS, reason: '', expiresAt: '' },
    { gameId: FLS, reason: 'r', expiresAt: 0 },
  ],
  unbanPlayer: [{ gameId: FLS }, { gameId: FLS, reason: null }],
  listBans: [{}, { opts: null }],
  shutdown: [{}, { reason: null }],
};

describe('null-args matrix', () => {
  it('covers every one of the 18 actions', () => {
    expect(Object.keys(NULL_ARG_CASES).sort()).toEqual([...GAME_SERVER_ACTIONS].sort());
  });

  for (const [action, cases] of Object.entries(NULL_ARG_CASES)) {
    it(`${action} handles absent, explicit-null and typed optional args`, async () => {
      for (const args of cases) {
        // A fresh connector per case: a ban or a kick from one case must not change the next.
        const h: Harness = await harness({ shutdownCmd: 'true', execRunner: async () => ({ code: 0, stdout: '', stderr: '' }) });
        let result: unknown;
        try {
          result = await h.adapter.handleAction(action, args);
        } catch (err) {
          // The only acceptable failure is a deliberate, explained ActionError — never a TypeError from reading null.
          expect(err).toBeInstanceOf(Error);
          expect((err as Error).constructor.name).not.toBe('TypeError');
          expect((err as Error).message).not.toMatch(/of (?:null|undefined)|is not a function/);
          continue;
        }
        expect(result).not.toBeUndefined();
        if (action === 'getPlayer') {
          const player = result as TakaroPlayer;
          expect(typeof player.gameId).toBe('string');
          expect(player.gameId.length).toBeGreaterThan(0);
        }
      }
    });
  }

  it('args arrive as [], {}, a JSON string, "" or null and all normalise to an object', () => {
    expect(normalizeArgs([])).toEqual({});
    expect(normalizeArgs(null)).toEqual({});
    expect(normalizeArgs('')).toEqual({});
    expect(normalizeArgs('{"gameId":"x"}')).toEqual({ gameId: 'x' });
    expect(normalizeArgs('not json')).toEqual({});
  });
});
