import assert from 'node:assert/strict';
import { test } from 'node:test';
import { HealthServer } from '../health/server.js';
import { PlayerPoller, type EmittedGameEvent } from '../events/playerPoller.js';
import type { ConanPlayer } from '../conan/parsers.js';

test('player poller emits connected events for players already online on startup', async () => {
  const events: EmittedGameEvent[] = [];
  const poller = new PlayerPoller(sequence([[player('1', 'Alice')]]), (event) => events.push(event), 1000);

  await poller.pollOnce();

  assert.deepEqual(events, [
    {
      type: 'player-connected',
      data: { player: { gameId: '1', name: 'Alice' } },
    },
  ]);
});

test('player poller emits connect and disconnect deltas after initial poll', async () => {
  const events: EmittedGameEvent[] = [];
  const poller = new PlayerPoller(
    sequence([[player('1', 'Alice')], [player('1', 'Alice'), player('2', 'Bob')], [player('2', 'Bob')]]),
    (event) => events.push(event),
    1000,
  );

  await poller.pollOnce();
  await poller.pollOnce();
  await poller.pollOnce();

  assert.deepEqual(events, [
    {
      type: 'player-connected',
      data: { player: { gameId: '1', name: 'Alice' } },
    },
    {
      type: 'player-connected',
      data: { player: { gameId: '2', name: 'Bob' } },
    },
    {
      type: 'player-disconnected',
      data: { player: { gameId: '1', name: 'Alice' } },
    },
  ]);
});

test('player poller omits connector-only fields from player events', async () => {
  const events: EmittedGameEvent[] = [];
  const poller = new PlayerPoller(
    sequence([[{ ...player('1', 'Alice'), online: true, characterName: 'AliceChar', rconId: '0' }], []]),
    (event) => events.push(event),
    1000,
  );

  await poller.pollOnce();
  await poller.pollOnce();

  assert.deepEqual(events, [
    {
      type: 'player-connected',
      data: { player: { gameId: '1', name: 'Alice' } },
    },
    {
      type: 'player-disconnected',
      data: { player: { gameId: '1', name: 'Alice' } },
    },
  ]);
});

test('player poller re-emits current players after reset', async () => {
  const events: EmittedGameEvent[] = [];
  const poller = new PlayerPoller(sequence([[player('1', 'Alice')], [player('1', 'Alice')]]), (event) => events.push(event), 1000);

  await poller.pollOnce();
  poller.reset();
  await poller.pollOnce();

  assert.deepEqual(events, [
    {
      type: 'player-connected',
      data: { player: { gameId: '1', name: 'Alice' } },
    },
    {
      type: 'player-connected',
      data: { player: { gameId: '1', name: 'Alice' } },
    },
  ]);
});

test('health server reports current state', async () => {
  const server = new HealthServer(0, () => ({
    ok: true,
    takaroIdentified: true,
    gameServerId: 'game-server-id',
    rconConfigured: true,
    logTailers: 2,
  }));
  await server.start();

  const response = await fetch(`http://127.0.0.1:${server.port()}/health`);
  const body = await response.json();

  assert.equal(response.status, 200);
  assert.deepEqual(body, {
    ok: true,
    takaroIdentified: true,
    gameServerId: 'game-server-id',
    rconConfigured: true,
    logTailers: 2,
  });

  await server.stop();
});

function player(gameId: string, name: string): ConanPlayer {
  return {
    gameId,
    name,
    online: true,
  };
}

function sequence(values: ConanPlayer[][]): () => Promise<ConanPlayer[]> {
  let index = 0;
  return async () => values[Math.min(index++, values.length - 1)]!;
}
