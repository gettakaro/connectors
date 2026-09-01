import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { parseLogLine } from '../logs/logParser.js';
import { LogTailer } from '../logs/logTailer.js';
import { PlayerPoller } from '../events/playerPoller.js';

test('parses log, chat, connect, and disconnect lines', () => {
  assert.deepEqual(parseLogLine('2026-06-21 10:00:00 [Info] Server started'), {
    type: 'log',
    data: { message: 'Server started', level: 'Info', timestamp: '2026-06-21 10:00:00' },
  });
  assert.deepEqual(parseLogLine('2026-06-21 19:59:26 - TShock: INFO: Broadcast: <TestPlayer> $settp h'), {
    type: 'chat-message',
    data: {
      player: { gameId: 'TestPlayer', name: 'TestPlayer', platformId: 'terraria:TestPlayer' },
      message: '$settp h',
      timestamp: '2026-06-21 19:59:26',
    },
  });
  assert.deepEqual(parseLogLine('2026-06-21 20:03:30 - HandlerCollection`1: INFO: Broadcast: <TestPlayer> $tplist'), {
    type: 'chat-message',
    data: {
      player: { gameId: 'TestPlayer', name: 'TestPlayer', platformId: 'terraria:TestPlayer' },
      message: '$tplist',
      timestamp: '2026-06-21 20:03:30',
    },
  });
  assert.deepEqual(parseLogLine('[Server API] Guide has joined.'), {
    type: 'player-connected',
    data: { player: { gameId: 'Guide', name: 'Guide', platformId: 'terraria:Guide' } },
  });
  assert.deepEqual(parseLogLine('Guide: hello world'), {
    type: 'chat-message',
    data: { player: { gameId: 'Guide', name: 'Guide', platformId: 'terraria:Guide' }, message: 'hello world' },
  });
  assert.deepEqual(parseLogLine('Guide has left.'), {
    type: 'player-disconnected',
    data: { player: { gameId: 'Guide', name: 'Guide', platformId: 'terraria:Guide' } },
  });
});

test('parses structured Takaro event markers from TShock plugin logs', () => {
  assert.deepEqual(parseLogLine('2026-06-21 10:00:00 [Info] TAKARO_EVENT {"type":"player-death","data":{"player":{"gameId":"TestPlayer","name":"TestPlayer","platformId":"terraria:TestPlayer"},"reason":"TestPlayer was slain","damage":42,"pvp":false}}'), {
    type: 'player-death',
    data: {
      player: { gameId: 'TestPlayer', name: 'TestPlayer', platformId: 'terraria:TestPlayer' },
      reason: 'TestPlayer was slain',
      damage: 42,
      pvp: false,
    },
  });

  assert.deepEqual(parseLogLine('TAKARO_EVENT {"type":"entity-killed","data":{"entity":{"gameId":"npc:3","name":"Zombie","platformId":"terraria:npc:3","type":3}}}'), {
    type: 'entity-killed',
    data: {
      entity: { gameId: 'npc:3', name: 'Zombie', platformId: 'terraria:npc:3', type: 3 },
    },
  });
});

test('poller emits connect and disconnect deltas from player snapshots', async () => {
  const snapshots = [
    [],
    [{ gameId: 'guide-user', name: 'Guide', platformId: 'terraria:guide-user' }],
    [],
  ];
  const events: unknown[] = [];
  const poller = new PlayerPoller(async () => snapshots.shift() || [], (event) => events.push(event), 10);

  await poller.pollOnce();
  await poller.pollOnce();
  await poller.pollOnce();

  assert.deepEqual(events, [
    { type: 'player-connected', data: { player: { gameId: 'guide-user', name: 'Guide', platformId: 'terraria:guide-user' } } },
    { type: 'player-disconnected', data: { player: { gameId: 'guide-user', name: 'Guide', platformId: 'terraria:guide-user' } } },
  ]);
});

test('log tailer emits new parsed lines', async () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'terraria-logs-'));
  const log = path.join(dir, 'server.log');
  writeFileSync(log, 'Guide: hello\n');
  const events: unknown[] = [];
  const tailer = new LogTailer(log, (event) => events.push(event), 1000, false);

  await tailer.pollOnce();

  assert.equal(events.length, 1);
});

test('log tailer starts at end by default to avoid replaying historical chat commands', async () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'terraria-logs-'));
  const log = path.join(dir, 'server.log');
  writeFileSync(log, '2026-06-21 19:59:26 - TShock: INFO: Broadcast: <TestPlayer> $settp h\n');
  const events: unknown[] = [];
  const tailer = new LogTailer(log, (event) => events.push(event));

  await tailer.pollOnce();

  assert.equal(events.length, 0);
});
