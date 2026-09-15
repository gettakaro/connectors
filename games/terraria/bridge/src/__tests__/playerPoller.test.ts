import assert from 'node:assert/strict';
import { test } from 'node:test';
import { PlayerPoller } from '../events/playerPoller.js';
import type { GameEvent } from '../takaro/protocol.js';
import type { TShockPlayer } from '../tshock/client.js';

const guide: TShockPlayer = { gameId: 'guide-user', name: 'Guide', platformId: 'terraria:guide-user' };
const merchant: TShockPlayer = { gameId: 'merchant-user', name: 'Merchant', platformId: 'terraria:merchant-user' };

function connectionRefused(): Error {
  const err = new Error('fetch failed');
  (err as Error & { code?: string }).code = 'ECONNREFUSED';
  return err;
}

interface Harness {
  poller: PlayerPoller;
  events: GameEvent[];
  setSnapshot: (players: TShockPlayer[]) => void;
  setFailure: (err: Error | null) => void;
}

function createHarness(initial: TShockPlayer[] = []): Harness {
  const events: GameEvent[] = [];
  let snapshot = initial;
  let failure: Error | null = null;
  const poller = new PlayerPoller(
    async () => {
      if (failure) throw failure;
      return snapshot;
    },
    (event) => events.push(event),
    10,
  );
  return {
    poller,
    events,
    setSnapshot: (players) => {
      snapshot = players;
    },
    setFailure: (err) => {
      failure = err;
    },
  };
}

test('a rejecting loadPlayers does not throw out of pollOnce', async () => {
  const harness = createHarness();
  harness.setFailure(connectionRefused());

  await assert.doesNotReject(() => harness.poller.pollOnce());
  assert.deepEqual(harness.events, []);
  assert.equal(harness.poller.lastPollAt, null);
});

test('a failed poll emits no disconnect events for previously seen players', async () => {
  const harness = createHarness([guide, merchant]);

  await harness.poller.pollOnce();
  assert.equal(harness.events.length, 2);
  assert.ok(harness.events.every((event) => event.type === 'player-connected'));

  harness.setFailure(connectionRefused());
  await harness.poller.pollOnce();
  await harness.poller.pollOnce();

  assert.equal(harness.events.filter((event) => event.type === 'player-disconnected').length, 0);
  assert.equal(harness.events.length, 2);
});

test('a failed poll leaves lastPollAt unchanged', async () => {
  const harness = createHarness([guide]);

  await harness.poller.pollOnce();
  const lastPollAt = harness.poller.lastPollAt;
  assert.ok(lastPollAt);

  harness.setFailure(connectionRefused());
  await harness.poller.pollOnce();

  assert.equal(harness.poller.lastPollAt, lastPollAt);
});

test('a successful poll after an outage diffs against pre-failure state without churn', async () => {
  const harness = createHarness([guide]);

  await harness.poller.pollOnce();
  assert.deepEqual(harness.events, [{ type: 'player-connected', data: { player: guide } }]);

  harness.setFailure(connectionRefused());
  await harness.poller.pollOnce();
  await harness.poller.pollOnce();

  // TShock comes back with the same player still online.
  harness.setFailure(null);
  await harness.poller.pollOnce();

  assert.deepEqual(harness.events, [{ type: 'player-connected', data: { player: guide } }]);
  assert.ok(harness.poller.lastPollAt);
});

test('players who actually left during an outage are reported once TShock recovers', async () => {
  const harness = createHarness([guide, merchant]);

  await harness.poller.pollOnce();
  harness.setFailure(connectionRefused());
  await harness.poller.pollOnce();

  harness.setFailure(null);
  harness.setSnapshot([guide]);
  await harness.poller.pollOnce();

  assert.deepEqual(harness.events.filter((event) => event.type === 'player-disconnected'), [
    { type: 'player-disconnected', data: { player: merchant } },
  ]);
});

test('normal connect and disconnect diffing is unchanged', async () => {
  const harness = createHarness([]);

  await harness.poller.pollOnce();
  harness.setSnapshot([guide]);
  await harness.poller.pollOnce();
  harness.setSnapshot([]);
  await harness.poller.pollOnce();

  assert.deepEqual(harness.events, [
    { type: 'player-connected', data: { player: guide } },
    { type: 'player-disconnected', data: { player: guide } },
  ]);
});

test('reset clears diff state so a fresh session re-emits connects', async () => {
  const harness = createHarness([guide]);

  await harness.poller.pollOnce();
  harness.poller.reset();
  assert.equal(harness.poller.lastPollAt, null);

  await harness.poller.pollOnce();

  assert.deepEqual(harness.events, [
    { type: 'player-connected', data: { player: guide } },
    { type: 'player-connected', data: { player: guide } },
  ]);
});

test('an unhandled rejection from start() does not escape the interval callback', async () => {
  const harness = createHarness([guide]);
  harness.setFailure(connectionRefused());
  const rejections: unknown[] = [];
  const onRejection = (reason: unknown): void => {
    rejections.push(reason);
  };
  process.on('unhandledRejection', onRejection);
  try {
    harness.poller.start();
    await new Promise((resolve) => setTimeout(resolve, 40));
    harness.poller.stop();
    await new Promise((resolve) => setImmediate(resolve));
  } finally {
    process.off('unhandledRejection', onRejection);
  }

  assert.deepEqual(rejections, []);
  assert.equal(harness.poller.lastPollAt, null);
});

test('repeated identical failures are logged once, and new errors and recovery are logged', async () => {
  const harness = createHarness([guide]);
  const errors: string[] = [];
  const infos: string[] = [];
  const originalError = console.error;
  const originalLog = console.log;
  console.error = (message: string) => errors.push(message);
  console.log = (message: string) => infos.push(message);
  try {
    harness.setFailure(connectionRefused());
    await harness.poller.pollOnce();
    await harness.poller.pollOnce();
    await harness.poller.pollOnce();

    // Same error three times in a row: only the first is logged.
    assert.equal(errors.length, 1);
    assert.match(errors[0], /Player poll failed: fetch failed/);

    // A different error breaks through the throttle immediately.
    harness.setFailure(new Error('502 Bad Gateway'));
    await harness.poller.pollOnce();
    assert.equal(errors.length, 2);
    assert.match(errors[1], /502 Bad Gateway/);
    assert.match(errors[1], /2 similar failures suppressed/);

    // Recovery is always logged, and resets the throttle.
    harness.setFailure(null);
    await harness.poller.pollOnce();
    assert.equal(infos.length, 1);
    assert.match(infos[0], /Player poll recovered/);

    harness.setFailure(connectionRefused());
    await harness.poller.pollOnce();
    assert.equal(errors.length, 3);
  } finally {
    console.error = originalError;
    console.log = originalLog;
  }
});

test('lastPollOk tracks the current poll outcome and lastPollAt still freezes on failure', async () => {
  const harness = createHarness([guide]);

  assert.equal(harness.poller.lastPollOk, null, 'no poll has run yet');

  await harness.poller.pollOnce();
  const successStamp = harness.poller.lastPollAt;
  assert.equal(harness.poller.lastPollOk, true);
  assert.ok(successStamp);

  harness.setFailure(connectionRefused());
  await harness.poller.pollOnce();
  assert.equal(harness.poller.lastPollOk, false, 'a failed poll must flip reachability immediately');
  assert.equal(harness.poller.lastPollAt, successStamp, 'lastPollAt stays frozen on failure');

  harness.setFailure(null);
  await harness.poller.pollOnce();
  assert.equal(harness.poller.lastPollOk, true, 'recovery must flip it back');
  assert.ok(harness.poller.lastPollAt, 'a recovered poll re-stamps lastPollAt');
});
