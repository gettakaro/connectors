import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ACTION_COVERAGE } from '../takaro/coverage.js';
import { HealthServer } from '../health/server.js';

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
