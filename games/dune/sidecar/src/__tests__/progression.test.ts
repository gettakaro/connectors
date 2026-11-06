import { describe, expect, it } from 'vitest';
import { mapPlayer, mapProgression } from '../dune/mapping.js';
import type { DuneProgressionRows } from '../dune/types.js';
import { MAP_DIMENSIONS } from '../dune/mapDimensions.js';
import { buildManifest } from '../takaro/capabilities.js';
import { sanitizeGameEvent } from '../takaro/eventWhitelist.js';
import { mockPlayer } from '../testing/mockBattlegroup.js';
import { harness } from './helpers.js';

const KEY_RE = /^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9-]*)*$/;
const level = (totalSkillPoints: number | null, keystoneBonusSkillPoints: number | null = 0, totalXpEarned: number | null = 2054) => ({
  totalXpEarned,
  totalSkillPoints,
  keystoneBonusSkillPoints,
});

async function answer(progression: DuneProgressionRows | undefined) {
  const h = await harness({ players: [mockPlayer({ ...(progression ? { progression } : {}) })] });
  const flsId = h.battlegroup.players[0]!.flsId;
  return h.adapter.handleAction('getPlayerProgression', { gameId: flsId });
}

describe('getPlayerProgression', () => {
  it('declares the capability as server-owned', () => {
    const capability = buildManifest(MAP_DIMENSIONS).filter((c) => c.name === 'players.progression');
    expect(capability).toEqual([{ name: 'players.progression', parameters: { source: 'server' } }]);
  });

  it('answers null for a player the game does not know, never an error', async () => {
    const h = await harness();
    await expect(h.adapter.handleAction('getPlayerProgression', { gameId: 'ffffffffffffffff' })).resolves.toBeNull();
  });

  it('answers an empty list for a known player whose character has no level component yet', async () => {
    await expect(answer(undefined)).resolves.toEqual([]);
  });

  it('reports the character level from skill points, with the total XP', async () => {
    await expect(answer({ character: level(5), specializations: [] })).resolves.toEqual([
      { key: 'character', name: 'Level', level: 6, experience: 2054 },
    ]);
  });

  it('a new character with no skill points is level 1', async () => {
    expect(mapProgression({ character: level(0, 0, 0), specializations: [] })).toEqual([
      { key: 'character', name: 'Level', level: 1, experience: 0 },
    ]);
  });

  it('keystone bonus points do not raise the level', () => {
    expect(mapProgression({ character: level(209, 10), specializations: [] })[0]?.level).toBe(200);
  });

  it('reports specializations with the fraction of their level as progress', async () => {
    await expect(
      answer({
        character: level(5),
        specializations: [
          { track: 'Combat', level: 28.5, xp: 9000 },
          { track: 'Crafting', level: 3, xp: 120 },
        ],
      }),
    ).resolves.toEqual([
      { key: 'character', name: 'Level', level: 6, experience: 2054 },
      { key: 'specialization.combat', name: 'Combat Specialization', level: 28, experience: 9000, progress: 0.5 },
      { key: 'specialization.crafting', name: 'Crafting Specialization', level: 3, experience: 120, progress: 0 },
    ]);
  });

  it('every track the game can report has a valid key', () => {
    const tracks = mapProgression({
      character: level(5),
      specializations: ['Crafting', 'Gathering', 'Exploration', 'Combat', 'Sabotage'].map((track) => ({ track, level: 1, xp: 0 })),
    });
    expect(tracks).toHaveLength(6);
    for (const track of tracks) expect(track.key).toMatch(KEY_RE);
    expect(new Set(tracks.map((t) => t.key)).size).toBe(6);
  });

  it('drops what cannot be a valid track, because one bad track fails the whole reading', () => {
    expect(
      mapProgression({
        character: level(null),
        specializations: [
          { track: 'Invalid', level: 3, xp: 1 },
          { track: 'Combat', level: null, xp: 1 },
          { track: 'Sabotage', level: -1, xp: 1 },
        ],
      }),
    ).toEqual([]);
    expect(mapProgression({ character: level(2, 5), specializations: [] })).toEqual([]);
  });

  it('leaves experience out when the save has no counter', () => {
    expect(mapProgression({ character: level(5, 0, null), specializations: [{ track: 'Combat', level: 2, xp: null }] })).toEqual([
      { key: 'character', name: 'Level', level: 6 },
      { key: 'specialization.combat', name: 'Combat Specialization', level: 2, progress: 0 },
    ]);
  });

  it('does not query tables a build does not have', async () => {
    const h = await harness();
    delete h.battlegroup.columns.fgl_entities;
    delete h.battlegroup.columns.specialization_tracks;
    await h.pg.probe();
    await expect(h.adapter.handleAction('getPlayerProgression', { gameId: h.battlegroup.players[0]!.flsId })).resolves.toEqual([]);
  });

  it('reads one character: the most recently played one that is not deleted', async () => {
    const h = await harness();
    await h.adapter.handleAction('getPlayerProgression', { gameId: h.battlegroup.players[0]!.flsId });
    const lookup = h.battlegroup.queries.find((q) => q.sql.includes('AS controller_id'));
    expect(lookup?.sql).toContain(`character_state::text <> 'Deleted'`);
    expect(lookup?.sql).toContain('ORDER BY ps.last_avatar_activity DESC');
    expect(lookup?.sql).toContain('LIMIT 1');
  });

  it('a fractional level never becomes a track', () => {
    expect(mapProgression({ character: level(5, 0.5), specializations: [] })).toEqual([]);
  });

  it('a build without the controller column answers an empty list', async () => {
    const h = await harness();
    h.battlegroup.columns.player_state = h.battlegroup.columns.player_state!.filter((c) => c !== 'player_controller_id');
    await h.pg.probe();
    await expect(h.adapter.handleAction('getPlayerProgression', { gameId: h.battlegroup.players[0]!.flsId })).resolves.toEqual([]);
  });

  it('a deleted character never answers for the player, and one player reads their most recent character', async () => {
    const h = await harness();
    await h.adapter.handleAction('getPlayer', { gameId: h.battlegroup.players[0]!.flsId });
    const roster = h.battlegroup.queries.find((q) => q.sql.includes('AS fls_id'));
    expect(roster?.sql).toContain(`ps.character_state::text <> 'Deleted'`);
    expect(roster?.sql).toMatch(/ORDER BY ps\.last_avatar_activity DESC/);
  });
});

describe('characters', () => {
  const player = { characterId: '2', characterName: 'Ariste' };

  it('declares server-held characters that death does not end', () => {
    expect(buildManifest(MAP_DIMENSIONS).filter((c) => c.name === 'players.characters')).toEqual([
      { name: 'players.characters', parameters: { held: 'server' } },
    ]);
  });

  it('getPlayers and getPlayer carry the character from its row id, not its name', async () => {
    const h = await harness({ players: [mockPlayer(player)] });
    const flsId = h.battlegroup.players[0]!.flsId;
    const expected = { id: '2', name: 'Ariste' };
    expect(((await h.adapter.handleAction('getPlayers', {})) as { character?: unknown }[])[0]?.character).toEqual(expected);
    expect(((await h.adapter.handleAction('getPlayer', { gameId: flsId })) as { character?: unknown }).character).toEqual(expected);
  });

  it('a renamed character keeps its id', () => {
    expect(mapPlayer({ flsId: 'A', characterId: '2', characterName: 'New name' }).character).toEqual({ id: '2', name: 'New name' });
  });

  it('a build without the row id reports no character', () => {
    expect(mapPlayer({ flsId: 'A', characterName: 'Ariste' }).character).toBeUndefined();
  });

  it('stays on the player in every pushed event', () => {
    const p = mapPlayer({ flsId: 'A', characterId: '2', characterName: 'Ariste' });
    for (const type of ['player-connected', 'player-disconnected', 'player-death', 'chat-message', 'entity-killed'] as const) {
      const { data, removed } = sanitizeGameEvent(type, { player: p, msg: 'x' });
      expect((data as { player: Record<string, unknown> }).player.character).toEqual({ id: '2', name: 'Ariste' });
      expect(removed).toEqual([]);
    }
  });

});
