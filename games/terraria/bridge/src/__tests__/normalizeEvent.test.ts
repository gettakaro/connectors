import assert from 'node:assert/strict';
import { test } from 'node:test';
import { normalizeGameEvent } from '../events/normalizeEvent.js';

test('normalizes Terraria plugin death events to Takaro player-death schema', () => {
  assert.deepEqual(normalizeGameEvent('player-death', {
    player: { gameId: 'TestPlayer', name: 'TestPlayer', platformId: 'terraria:hash', ip: '127.0.0.1', tshockIndex: 0 },
    reason: 'TestPlayer was slain by Zombie.',
    damage: 12,
    pvp: false,
  }), {
    type: 'player-death',
    data: {
      player: { gameId: 'TestPlayer', name: 'TestPlayer', ip: '127.0.0.1' },
      msg: 'TestPlayer was slain by Zombie.',
    },
  });
});

test('normalizes entity-killed and log events to Takaro schema', () => {
  assert.deepEqual(normalizeGameEvent('entity-killed', {
    entity: { gameId: 'npc:4', name: 'Zombie', platformId: 'terraria:npc:4' },
    killer: null,
  }), {
    type: 'entity-killed',
    data: { entity: 'Zombie', weapon: 'unknown' },
  });

  assert.deepEqual(normalizeGameEvent('log', { message: 'Server started', level: 'Info', timestamp: '2026-06-21 10:00:00' }), {
    type: 'log',
    data: { msg: 'Server started', timestamp: '2026-06-21 10:00:00' },
  });
});

test('normalizes online player snapshots to strict Takaro player DTOs', () => {
  assert.deepEqual(normalizeGameEvent('player-connected', {
    player: { gameId: 'TestPlayer', name: 'TestPlayer', group: 'guest', active: true, state: 10, team: 0 },
  }), {
    type: 'player-connected',
    data: { player: { gameId: 'TestPlayer', name: 'TestPlayer' } },
  });
});

test('carries the death attacker through when the plugin resolves one', () => {
  // The plugin resolves the killer via PlayerDeathReason.TryGetCausingEntity; without
  // it every death arrived with no attacker even when the game could name the killer.
  assert.deepEqual(normalizeGameEvent('player-death', {
    player: { gameId: 'TestPlayer', name: 'TestPlayer', platformId: 'terraria:hash' },
    attacker: { gameId: 'npc:7', name: 'Demon Eye', platformId: 'terraria:npc:7' },
    reason: 'TestPlayer was cut down the middle by Demon Eye.',
  }), {
    type: 'player-death',
    data: {
      player: { gameId: 'TestPlayer', name: 'TestPlayer' },
      attacker: { gameId: 'npc:7', name: 'Demon Eye' },
      msg: 'TestPlayer was cut down the middle by Demon Eye.',
    },
  });
});

test('omits the attacker for a death with no killer', () => {
  // A fall or drowning death legitimately has none; inventing one would be worse.
  assert.deepEqual(normalizeGameEvent('player-death', {
    player: { gameId: 'TestPlayer', name: 'TestPlayer', platformId: 'terraria:hash' },
    attacker: null,
    reason: 'TestPlayer fell to their death.',
  }), {
    type: 'player-death',
    data: {
      player: { gameId: 'TestPlayer', name: 'TestPlayer' },
      msg: 'TestPlayer fell to their death.',
    },
  });
});
