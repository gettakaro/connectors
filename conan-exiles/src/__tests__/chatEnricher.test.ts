import assert from 'node:assert/strict';
import { test } from 'node:test';
import { enrichChatMessageEvent, enrichLogEvent } from '../events/chatEnricher.js';

test('enriches Pippi character chat events with the matching online player identity', async () => {
  const event = {
    type: 'chat-message' as const,
    data: {
      msg: 'hello',
      channel: 'global',
      timestamp: '2026-06-21T08:38:41.754Z',
      player: {
        gameId: 'TestExile',
        name: 'TestExile',
      },
    },
  };

  const enriched = await enrichChatMessageEvent(event, async () => [
    {
      gameId: '76561198000000001',
      name: 'Tester#1000',
      characterName: 'TestExile',
      rconId: '0',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);

  assert.deepEqual(enriched, {
    type: 'chat-message',
    data: {
      msg: 'hello',
      channel: 'global',
      timestamp: '2026-06-21T08:38:41.754Z',
      player: {
        gameId: '76561198000000001',
        name: 'Tester#1000',
        steamId: '76561198000000001',
        platformId: 'steam:76561198000000001',
      },
    },
  });
});

test('leaves chat events unchanged when no online player matches', async () => {
  const event = {
    type: 'chat-message' as const,
    data: {
      msg: 'hello',
      channel: 'global',
      timestamp: '2026-06-21T08:38:41.754Z',
      player: {
        gameId: 'unknown',
        name: 'unknown',
      },
    },
  };

  assert.deepEqual(await enrichChatMessageEvent(event, async () => []), event);
});

test('enriches player death events with the matching online player identity', async () => {
  const event = {
    type: 'player-death' as const,
    data: {
      player: {
        gameId: 'TestExile',
        name: 'TestExile',
      },
      msg: 'Thirst',
      timestamp: '2026-06-21T08:34:22.555Z',
    },
  };

  const enriched = await enrichLogEvent(event, async () => [
    {
      gameId: '76561198000000001',
      name: 'Tester#1000',
      characterName: 'TestExile',
      rconId: '0',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);

  assert.deepEqual(enriched, {
    type: 'player-death',
    data: {
      player: {
        gameId: '76561198000000001',
        name: 'Tester#1000',
        steamId: '76561198000000001',
        platformId: 'steam:76561198000000001',
      },
      msg: 'Thirst',
      timestamp: '2026-06-21T08:34:22.555Z',
    },
  });
});

test('enriches entity killed events with the matching online player identity', async () => {
  const event = {
    type: 'entity-killed' as const,
    data: {
      player: {
        gameId: 'TestExile',
        name: 'TestExile',
      },
      entity: 'Spider',
      weapon: 'Fatality',
      timestamp: '2026-06-21T08:52:16.841Z',
      msg: 'TestExile killed Spider',
    },
  };

  const enriched = await enrichLogEvent(event, async () => [
    {
      gameId: '76561198000000001',
      name: 'Tester#1000',
      characterName: 'TestExile',
      rconId: '0',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);

  assert.deepEqual(enriched, {
    type: 'entity-killed',
    data: {
      player: {
        gameId: '76561198000000001',
        name: 'Tester#1000',
        steamId: '76561198000000001',
        platformId: 'steam:76561198000000001',
      },
      entity: 'Spider',
      weapon: 'Fatality',
      timestamp: '2026-06-21T08:52:16.841Z',
      msg: 'TestExile killed Spider',
    },
  });
});
