import assert from 'node:assert/strict';
import { test } from 'node:test';
import { PositionPoller } from '../events/positionPoller.js';

test('position polling does not emit a stale response or burst on reconnect', async () => {
  let resolveFirst: ((value: { timestamp: string; positions: [] }) => void) | undefined;
  const emitted: number[] = [];
  let loads = 0;
  const poller = new PositionPoller(
    () => {
      loads++;
      if (loads === 1) return new Promise<{ timestamp: string; positions: [] }>((resolve) => { resolveFirst = resolve; });
      return Promise.resolve({ timestamp: new Date().toISOString(), positions: [] });
    },
    () => emitted.push(Date.now()),
    100,
  );

  poller.start();
  await new Promise((resolve) => setTimeout(resolve, 10));
  poller.stop();
  resolveFirst?.({ timestamp: new Date().toISOString(), positions: [] });
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(emitted.length, 0, 'an old in-flight poll cannot emit after disconnect');

  poller.start();
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(emitted.length, 1);
  poller.stop();
  poller.start();
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(emitted.length, 1, 'reconnect cannot violate the declared interval');
  poller.stop();
});
