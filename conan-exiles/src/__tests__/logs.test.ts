import assert from 'node:assert/strict';
import { appendFileSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { parseLogLine } from '../logs/logParser.js';
import { LogTailer } from '../logs/logTailer.js';

test('parses generic log lines as log events', () => {
  assert.deepEqual(parseLogLine('[2026.06.20-10.00.00] Server started'), [
    {
      type: 'log',
      data: {
        msg: '[2026.06.20-10.00.00] Server started',
        timestamp: '2026-06-20T10:00:00.000Z',
      },
    },
  ]);
});

test('parses best-effort chat lines as chat-message and log events', () => {
  assert.deepEqual(
    parseLogLine('[Chat] Alice: hello world', { now: () => new Date('2026-06-20T10:00:00.000Z') }),
    [
      {
        type: 'chat-message',
        data: {
          msg: 'hello world',
          channel: 'global',
          timestamp: '2026-06-20T10:00:00.000Z',
          player: { name: 'Alice' },
        },
      },
      {
        type: 'log',
        data: {
          msg: '[Chat] Alice: hello world',
          timestamp: '2026-06-20T10:00:00.000Z',
        },
      },
    ],
  );
});

test('parses live Conan ChatWindow lines as chat-message events', () => {
  assert.deepEqual(
    parseLogLine('[2026.06.20-21.06.10:804][802]ChatWindow: Character TestExile (uid 109, player 76561198000000001) said: hey'),
    [
      {
        type: 'chat-message',
        data: {
          msg: 'hey',
          channel: 'global',
          timestamp: '2026-06-20T21:06:10.804Z',
          player: {
            gameId: '76561198000000001',
            name: 'TestExile',
            steamId: '76561198000000001',
            platformId: 'steam:76561198000000001',
          },
        },
      },
      {
        type: 'log',
        data: {
          msg: '[2026.06.20-21.06.10:804][802]ChatWindow: Character TestExile (uid 109, player 76561198000000001) said: hey',
          timestamp: '2026-06-20T21:06:10.804Z',
        },
      },
    ],
  );
});

test('parses Enhanced Pippi ChatWindow lines as chat-message events', () => {
  assert.deepEqual(
    parseLogLine('[2026.06.21-07.53.06:817][Pippi]ChatWindow: Character TestExile said: hey back'),
    [
      {
        type: 'chat-message',
        data: {
          msg: 'hey back',
          channel: 'global',
          timestamp: '2026-06-21T07:53:06.817Z',
          player: {
            gameId: 'TestExile',
            name: 'TestExile',
          },
        },
      },
      {
        type: 'log',
        data: {
          msg: '[2026.06.21-07.53.06:817][Pippi]ChatWindow: Character TestExile said: hey back',
          timestamp: '2026-06-21T07:53:06.817Z',
        },
      },
    ],
  );
});

test('parses Conan player death lines as player-death events', () => {
  const line =
    '[2026.06.21-08.34.22:555][987]ConanSandbox: Warning: KillCharacterWithRagdoll_Implementation. KillerNameInput: TestExile CauseOfDeath: Thirst. IsThrall: 0 Name: BasePlayerChar_C_2147234785 CharacterName: TestExile';

  assert.deepEqual(parseLogLine(line), [
    {
      type: 'player-death',
      data: {
        player: {
          gameId: 'TestExile',
          name: 'TestExile',
        },
        msg: 'Thirst',
        timestamp: '2026-06-21T08:34:22.555Z',
      },
    },
    {
      type: 'log',
      data: {
        msg: line,
        timestamp: '2026-06-21T08:34:22.555Z',
      },
    },
  ]);
});

test('parses real Conan suicide lines as player-death without attacker', () => {
  const line =
    '[2026.06.21-18.30.16:203][437]ConanSandbox: Warning: KillCharacterWithRagdoll_Implementation. KillerNameInput: yourself CauseOfDeath: Suicide. IsThrall: 0 Name: BasePlayerChar_C_2147164501 CharacterName: TestExile';

  assert.deepEqual(parseLogLine(line), [
    {
      type: 'player-death',
      data: {
        player: {
          gameId: 'TestExile',
          name: 'TestExile',
        },
        msg: 'Suicide',
        timestamp: '2026-06-21T18:30:16.203Z',
      },
    },
    {
      type: 'log',
      data: {
        msg: line,
        timestamp: '2026-06-21T18:30:16.203Z',
      },
    },
  ]);
});

test('treats Conan NPC death lines without a player killer as log-only events', () => {
  const line =
    '[2026.06.21-08.52.16:841][950]ConanSandbox: Warning: KillCharacterWithRagdoll_Implementation. KillerNameInput:  CauseOfDeath: None. IsThrall: 0 Name: BP_NPC_Wildlife_Spider_Green_C_2147226326 CharacterName: Spider';

  assert.deepEqual(parseLogLine(line), [
    {
      type: 'log',
      data: {
        msg: line,
        timestamp: '2026-06-21T08:52:16.841Z',
      },
    },
  ]);
});

test('treats Conan NPC-vs-NPC death lines as log-only events', () => {
  const line =
    '[2026.06.21-18.26.08:416][ 74]ConanSandbox: Warning: KillCharacterWithRagdoll_Implementation. KillerNameInput: NPC_PREFIX_Wildlife_Siptah_ThunnHa_Normal CauseOfDeath: Combat. IsThrall: 0 Name: BP_NPC_Wildlife_RocknoseRocky2_C_2147235983 CharacterName: Rocknose';

  assert.deepEqual(parseLogLine(line), [
    {
      type: 'log',
      data: {
        msg: line,
        timestamp: '2026-06-21T18:26:08.416Z',
      },
    },
  ]);
});

test('parses Conan player-attributed NPC death lines as entity-killed events', () => {
  const line =
    '[2026.06.21-08.52.16:841][950]ConanSandbox: Warning: KillCharacterWithRagdoll_Implementation. KillerNameInput: TestExile CauseOfDeath: Fatality. IsThrall: 0 Name: BP_NPC_Wildlife_Spider_Green_C_2147226326 CharacterName: Spider';

  assert.deepEqual(parseLogLine(line), [
    {
      type: 'entity-killed',
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
    },
    {
      type: 'log',
      data: {
        msg: line,
        timestamp: '2026-06-21T08:52:16.841Z',
      },
    },
  ]);
});

test('treats Enhanced Pippi duplicate channel lines as log-only events', () => {
  assert.deepEqual(
    parseLogLine('[2026.06.21-08.36.10:686][Pippi]PippiChat: TestExile said in channel [Global]: hey'),
    [
      {
        type: 'log',
        data: {
          msg: '[2026.06.21-08.36.10:686][Pippi]PippiChat: TestExile said in channel [Global]: hey',
          timestamp: '2026-06-21T08:36:10.686Z',
        },
      },
    ],
  );
});

test('log tailer starts at end and emits only appended lines', async () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'conan-logs-'));
  const file = path.join(dir, 'ConanSandbox.log');
  const events: unknown[] = [];
  writeFileSync(file, 'old line\n');

  const tailer = new LogTailer(file, (event) => events.push(event), 1000, {
    now: () => new Date('2026-06-20T10:00:00.000Z'),
  });
  await tailer.pollOnce();
  appendFileSync(file, '[Chat] Alice: hello\nnew raw line\n');
  await tailer.pollOnce();

  assert.deepEqual(events, [
    {
      type: 'chat-message',
      data: {
        msg: 'hello',
        channel: 'global',
        timestamp: '2026-06-20T10:00:00.000Z',
        player: { name: 'Alice' },
      },
    },
    {
      type: 'log',
      data: {
        msg: '[Chat] Alice: hello',
        timestamp: '2026-06-20T10:00:00.000Z',
      },
    },
    {
      type: 'log',
      data: {
        msg: 'new raw line',
        timestamp: '2026-06-20T10:00:00.000Z',
      },
    },
  ]);

  rmSync(dir, { recursive: true, force: true });
});
