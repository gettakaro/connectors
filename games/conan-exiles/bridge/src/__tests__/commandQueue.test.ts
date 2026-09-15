import assert from 'node:assert/strict';
import { test } from 'node:test';
import { RconCommandQueue } from '../rcon/commandQueue.js';

test('serializes RCON command execution', async () => {
  const calls: string[] = [];
  let active = 0;
  let maxActive = 0;
  const queue = new RconCommandQueue(async (command) => {
    active += 1;
    maxActive = Math.max(maxActive, active);
    calls.push(`start:${command}`);
    await new Promise((resolve) => setTimeout(resolve, command === 'first' ? 20 : 1));
    calls.push(`end:${command}`);
    active -= 1;
    return `ok:${command}`;
  });

  const results = await Promise.all([queue.run('first'), queue.run('second'), queue.run('third')]);

  assert.deepEqual(results, ['ok:first', 'ok:second', 'ok:third']);
  assert.equal(maxActive, 1);
  assert.deepEqual(calls, [
    'start:first',
    'end:first',
    'start:second',
    'end:second',
    'start:third',
    'end:third',
  ]);
});
