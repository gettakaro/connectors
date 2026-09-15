import assert from 'node:assert/strict';
import { test } from 'node:test';
import { HealthServer } from '../health/server.js';
import { PlayerPoller, type EmittedGameEvent } from '../events/playerPoller.js';
import { ModCommandBridge } from '../mod/commandBridge.js';
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

test('health server exposes mod bridge source markers and recent traces', async () => {
  const bridge = new ModCommandBridge({ resultTimeoutMs: 1000 });
  const server = new HealthServer(0, () => ({
    ok: true,
    takaroIdentified: true,
    gameServerId: 'game-server-id',
    rconConfigured: true,
    logTailers: 0,
    modBridge: bridge.status(),
  }), (req, res) => bridge.handleHttpRequest(req, res));
  await server.start();

  try {
    const baseUrl = `http://127.0.0.1:${server.port()}`;
    const pending = bridge.sendMessage('health trace', null);
    const pollResponse = await fetch(`${baseUrl}/mod/poll?source=TakaroConan/1.0`);
    const pollBody = (await pollResponse.json()) as { command: { requestId: string } };
    assert.equal(pollResponse.status, 200);

    const resultResponse = await fetch(`${baseUrl}/mod/result`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        requestId: pollBody.command.requestId,
        result: { success: true },
      }),
    });
    assert.equal(resultResponse.status, 200);
    await pending;

    const eventResponse = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        type: 'chat-message',
        data: {
          message: 'health inbound trace',
          player: { gameId: '76561198000735875', platformId: 'steam:76561198000735875', name: 'Limon#67642' },
        },
      }),
    });
    assert.equal(eventResponse.status, 200);

    const healthResponse = await fetch(`${baseUrl}/health`);
    const body = await healthResponse.json() as {
      modBridge?: {
        lastPollSource?: string;
        lastResultSource?: string;
        lastEventSource?: string;
        lastEventType?: string;
        recentResults?: Array<{ source?: string; message?: string; resultSuccess?: boolean }>;
        recentEvents?: Array<{ source?: string; message?: string; playerPlatformId?: string }>;
      };
    };

    assert.equal(healthResponse.status, 200);
    assert.equal(body.modBridge?.lastPollSource, 'TakaroConan/1.0');
    assert.equal(body.modBridge?.lastResultSource, 'TakaroConan/1.0');
    assert.equal(body.modBridge?.lastEventSource, 'TakaroConan/1.0');
    assert.equal(body.modBridge?.lastEventType, 'chat-message');
    assert.equal(body.modBridge?.recentResults?.[0]?.source, 'TakaroConan/1.0');
    assert.equal(body.modBridge?.recentResults?.[0]?.message, 'health trace');
    assert.equal(body.modBridge?.recentResults?.[0]?.resultSuccess, true);
    assert.equal(body.modBridge?.recentEvents?.[0]?.source, 'TakaroConan/1.0');
    assert.equal(body.modBridge?.recentEvents?.[0]?.message, 'health inbound trace');
    assert.equal(body.modBridge?.recentEvents?.[0]?.playerPlatformId, 'steam:76561198000735875');
  } finally {
    await server.stop();
  }
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
