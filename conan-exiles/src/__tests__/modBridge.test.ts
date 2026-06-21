import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ModCommandBridge } from '../mod/commandBridge.js';

test('mod command bridge queues sendMessage commands and resolves results', async () => {
  const bridge = new ModCommandBridge({ resultTimeoutMs: 1000 });

  const pending = bridge.sendMessage('hello from Takaro', '76561198000000001');
  const polled = bridge.pollCommand();

  assert.equal(bridge.isConnected(), true);
  assert.equal(polled?.action, 'sendMessage');
  assert.deepEqual(polled?.args, {
    message: 'hello from Takaro',
    recipient: '76561198000000001',
  });

  bridge.complete(polled!.requestId, { success: true, sent: true });

  assert.deepEqual(await pending, { success: true, sent: true });
});

test('mod command bridge reports unavailable when no mod has polled yet', async () => {
  const bridge = new ModCommandBridge({ resultTimeoutMs: 1000 });

  assert.equal(bridge.isConnected(), false);
});
