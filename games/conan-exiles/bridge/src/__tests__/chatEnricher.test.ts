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
        gameId: 'werwerwer',
        name: 'werwerwer',
      },
    },
  };

  const enriched = await enrichChatMessageEvent(event, async () => [
    {
      gameId: '76561198000735875',
      name: 'Limon#67642',
      characterName: 'werwerwer',
      rconId: '0',
      steamId: '76561198000735875',
      platformId: 'steam:76561198000735875',
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
        gameId: '76561198000735875',
        name: 'Limon#67642',
        steamId: '76561198000735875',
        platformId: 'steam:76561198000735875',
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
        gameId: 'werwerwer',
        name: 'werwerwer',
      },
      msg: 'Thirst',
      timestamp: '2026-06-21T08:34:22.555Z',
    },
  };

  const enriched = await enrichLogEvent(event, async () => [
    {
      gameId: '76561198000735875',
      name: 'Limon#67642',
      characterName: 'werwerwer',
      rconId: '0',
      steamId: '76561198000735875',
      platformId: 'steam:76561198000735875',
      online: true,
    },
  ]);

  assert.deepEqual(enriched, {
    type: 'player-death',
    data: {
      player: {
        gameId: '76561198000735875',
        name: 'Limon#67642',
        steamId: '76561198000735875',
        platformId: 'steam:76561198000735875',
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
        gameId: 'werwerwer',
        name: 'werwerwer',
      },
      entity: 'Spider',
      weapon: 'Fatality',
      timestamp: '2026-06-21T08:52:16.841Z',
      msg: 'werwerwer killed Spider',
    },
  };

  const enriched = await enrichLogEvent(event, async () => [
    {
      gameId: '76561198000735875',
      name: 'Limon#67642',
      characterName: 'werwerwer',
      rconId: '0',
      steamId: '76561198000735875',
      platformId: 'steam:76561198000735875',
      online: true,
    },
  ]);

  assert.deepEqual(enriched, {
    type: 'entity-killed',
    data: {
      player: {
        gameId: '76561198000735875',
        name: 'Limon#67642',
        steamId: '76561198000735875',
        platformId: 'steam:76561198000735875',
      },
      entity: 'Spider',
      weapon: 'Fatality',
      timestamp: '2026-06-21T08:52:16.841Z',
      msg: 'werwerwer killed Spider',
    },
  });
});
