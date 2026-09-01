import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ACTION_COVERAGE } from '../takaro/coverage.js';
import { PlayerPoller } from '../events/playerPoller.js';
import { HealthServer, type HealthStatus } from '../health/server.js';
import type { TShockPlayer } from '../tshock/client.js';

test('serves health and coverage endpoints', async () => {
  const health = new HealthServer(0, () => ({
    ok: true,
    takaroIdentified: true,
    gameServerId: 'gs_123',
    tshockReachable: true,
    lastPollAt: '2026-06-21T00:00:00.000Z',
  }));

  await health.start();
  try {
    const base = `http://127.0.0.1:${health.port()}`;
    const healthBody = await (await fetch(`${base}/health`)).json() as { gameServerId: string };
    const coverageBody = await (await fetch(`${base}/coverage`)).json() as { actions: unknown };

    assert.equal(healthBody.gameServerId, 'gs_123');
    assert.deepEqual(coverageBody.actions, ACTION_COVERAGE);
  } finally {
    await health.stop();
  }
});

/**
 * Mirrors the /health wiring in src/index.ts: reachability comes from the poller's latest
 * outcome, falling back to the startup probe until the first poll completes.
 */
function createReachabilityHarness(startupReachable: boolean) {
  let failure: Error | null = null;
  const poller = new PlayerPoller(
    async () => {
      if (failure) throw failure;
      return [] as TShockPlayer[];
    },
    () => {},
    60_000,
  );
  const status = (): HealthStatus => {
    const tshockReachable = poller.lastPollOk ?? startupReachable;
    return {
      ok: tshockReachable,
      takaroIdentified: true,
      gameServerId: 'gs_123',
      tshockReachable,
      lastPollAt: poller.lastPollAt,
    };
  };
  return {
    poller,
    status,
    setFailure: (err: Error | null) => { failure = err; },
  };
}

async function readHealth(health: HealthServer): Promise<HealthStatus> {
  return await (await fetch(`http://127.0.0.1:${health.port()}/health`)).json() as HealthStatus;
}

test('health reports tshockReachable false and ok false after a poll failure', async () => {
  const harness = createReachabilityHarness(true);
  const health = new HealthServer(0, harness.status);
  await health.start();
  try {
    harness.setFailure(new Error('fetch failed'));
    await harness.poller.pollOnce();

    const body = await readHealth(health);

    assert.equal(body.tshockReachable, false);
    assert.equal(body.ok, false);
  } finally {
    await health.stop();
  }
});

test('health returns to reachable after a subsequent successful poll', async () => {
  const harness = createReachabilityHarness(true);
  const health = new HealthServer(0, harness.status);
  await health.start();
  try {
    harness.setFailure(new Error('fetch failed'));
    await harness.poller.pollOnce();
    assert.equal((await readHealth(health)).tshockReachable, false);

    harness.setFailure(null);
    await harness.poller.pollOnce();
    const body = await readHealth(health);

    assert.equal(body.tshockReachable, true);
    assert.equal(body.ok, true);
    assert.ok(body.lastPollAt, 'a successful poll should stamp lastPollAt');
  } finally {
    await health.stop();
  }
});

test('health falls back to the startup probe before any poll has run', async () => {
  const reachable = createReachabilityHarness(true);
  const unreachable = createReachabilityHarness(false);
  const healthUp = new HealthServer(0, reachable.status);
  const healthDown = new HealthServer(0, unreachable.status);
  await healthUp.start();
  await healthDown.start();
  try {
    const up = await readHealth(healthUp);
    assert.equal(up.tshockReachable, true, 'a reachable startup probe must not read as down');
    assert.equal(up.ok, true);
    assert.equal(up.lastPollAt, null);

    const down = await readHealth(healthDown);
    assert.equal(down.tshockReachable, false);
    assert.equal(down.ok, false);
  } finally {
    await healthUp.stop();
    await healthDown.stop();
  }
});

test('a Takaro reconnect reset does not strand a stale reachable reading', async () => {
  // poller.reset() runs on every reconnect; lastPollOk must clear with it so /health
  // reports the startup baseline rather than a reading from before the disconnect.
  const harness = createReachabilityHarness(false);
  const health = new HealthServer(0, harness.status);
  await health.start();
  try {
    await harness.poller.pollOnce();
    assert.equal((await readHealth(health)).tshockReachable, true);

    harness.poller.reset();
    const body = await readHealth(health);

    assert.equal(body.tshockReachable, false);
    assert.equal(body.lastPollAt, null);
  } finally {
    await health.stop();
  }
});
