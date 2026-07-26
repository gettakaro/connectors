import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseListPlayers } from '../conan/parsers.js';
import {
  ConanClientModChatBridge,
  encodeConanDataCommandArgument,
  quoteConanEventArgument,
} from '../mod/clientModChatBridge.js';

const TWO_PLAYERS = parseListPlayers([
  'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name',
  '  0 | AliceChar | Alice#1000 | A-1HFFLI28NN | 76561198000000001 |         STEAM',
  '  3 | BobChar | Bob#2000 | B-1HFFLI28NN | 76561198000000002 |         STEAM',
].join('\n'));

test('quotes and encodes safe DataCmd arguments without ambiguous percent sequences', () => {
  assert.equal(quoteConanEventArgument('Takaro Ops'), '"Takaro Ops"');
  assert.equal(encodeConanDataCommandArgument('100% ready in 5 minutes'), '100%25%20ready%20in%205%20minutes');
  for (const unsafe of ['line one\nline two', 'next;quit', 'next|quit', 'next&&quit']) {
    assert.throws(() => quoteConanEventArgument(unsafe), /unsafe Conan event argument/i);
  }
});

test('dispatches server-wide messages to every player through TakaroChat DataCmd', async () => {
  const commands: string[] = [];
  const bridge = new ConanClientModChatBridge(recordingExecutor(commands), async () => TWO_PLAYERS);

  const result = await bridge.sendMessage('Restart in 5 minutes', null, 'Takaro');

  assert.deepEqual(commands, [
    'con 0 dc TakaroChat "Takaro" "Restart%20in%205%20minutes"',
    'con 3 dc TakaroChat "Takaro" "Restart%20in%205%20minutes"',
  ]);
  assert.equal(result.dispatchAccepted, true);
  assert.equal(result.deliveryVerified, false);
  assert.deepEqual(result.targetIds, ['76561198000000001', '76561198000000002']);
  assertNoLegacyChatCommands(commands);
});

test('dispatches targeted messages by stable Steam identity', async () => {
  const commands: string[] = [];
  const bridge = new ConanClientModChatBridge(recordingExecutor(commands), async () => TWO_PLAYERS);

  const result = await bridge.sendMessage('Private warning', 'steam:76561198000000002', 'Takaro Admin');

  assert.deepEqual(commands, ['con 3 dc TakaroChat "Takaro%20Admin" "Private%20warning"']);
  assert.deepEqual(result.targetIds, ['76561198000000002']);
  assertNoLegacyChatCommands(commands);
});

test('rejects missing, ambiguous, and malformed player targets before unsafe dispatch', async () => {
  const commands: string[] = [];
  const noPlayers = new ConanClientModChatBridge(recordingExecutor(commands), async () => []);
  await assert.rejects(() => noPlayers.sendMessage('Anyone there?', null, 'Takaro'), /no online Conan players/i);

  const duplicateNames = TWO_PLAYERS.map((player) => ({ ...player, name: 'SharedName' }));
  const ambiguous = new ConanClientModChatBridge(recordingExecutor(commands), async () => duplicateNames);
  await assert.rejects(() => ambiguous.sendMessage('Private', 'SharedName', 'Takaro'), /ambiguous/i);

  const malformed = new ConanClientModChatBridge(recordingExecutor(commands), async () => [
    { ...TWO_PLAYERS[0]!, rconId: '0;quit' },
  ]);
  await assert.rejects(() => malformed.sendMessage('Guard', null, 'Takaro'), /invalid RCON player index/i);
  assert.deepEqual(commands, []);
});

test('requires exact Conan RCON acknowledgement and reports delivery honestly', async () => {
  const command = 'con 0 dc TakaroChat "Takaro" "Exact%20response"';
  for (const response of [
    'Unknown command: dc',
    'Successfully executed: unrelated command',
    `Successfully executed: ${command}\nFailure: renderer missing`,
  ]) {
    const bridge = new ConanClientModChatBridge(async () => response, async () => [TWO_PLAYERS[0]!]);
    await assert.rejects(() => bridge.sendMessage('Exact response', null, 'Takaro'), /RCON.*(?:exactly accept|Unknown command)/i);
  }

  const bridge = new ConanClientModChatBridge(async () => `  Successfully executed: ${command}\r\n`, async () => [TWO_PLAYERS[0]!]);
  const result = await bridge.sendMessage('Exact response', null, 'Takaro');
  assert.equal(result.dispatchAccepted, true);
  assert.equal(result.deliveryVerified, false);
  assert.match(result.verificationReason, /client delivery is not acknowledged/i);
});

test('publishes only completed latest-attempt diagnostics', async () => {
  const older = deferred<string>();
  const newer = deferred<string>();
  const started: string[] = [];
  const bridge = new ConanClientModChatBridge(async (command) => {
    started.push(command);
    return command.includes('Older%20request') ? older.promise : newer.promise;
  }, async () => [TWO_PLAYERS[0]!]);

  const olderResult = bridge.sendMessage('Older request', null, 'Takaro');
  await waitFor(() => started.length === 1);
  const newerResult = bridge.sendMessage('Newer request', null, 'Takaro');
  await waitFor(() => started.length === 2);
  assert.equal(bridge.status().lastAttemptAt, null);

  newer.resolve(`Successfully executed: ${started[1]}`);
  await newerResult;
  const latest = bridge.status();
  assert.equal(latest.lastMessage, 'Newer request');
  assert.equal(latest.dispatchAccepted, true);
  assert.equal(latest.deliveryVerified, false);

  older.resolve(`Successfully executed: ${started[0]}`);
  await olderResult;
  assert.deepEqual(bridge.status(), latest);
});

function recordingExecutor(commands: string[]): (command: string) => Promise<string> {
  return async (command) => {
    commands.push(command);
    return `Successfully executed: ${command}`;
  };
}

function assertNoLegacyChatCommands(commands: string[]): void {
  for (const command of commands) {
    assert.doesNotMatch(command, /Pippi/i);
    assert.doesNotMatch(command, /^(?:server|directmessage|broadcast)\b/i);
  }
}

function deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void } {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

async function waitFor(predicate: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 50; attempt += 1) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  throw new Error('Timed out waiting for asynchronous test condition');
}
