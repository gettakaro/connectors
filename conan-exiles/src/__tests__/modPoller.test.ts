import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ModCommandPoller, RconChatModRenderer, type BridgeClient, type RenderedMessage } from '../mod/poller.js';

test('mod command poller renders sendMessage commands and posts success results', async () => {
  const rendered: RenderedMessage[] = [];
  const completed: unknown[] = [];
  const bridge = fakeBridge(
    {
      requestId: 'req-1',
      action: 'sendMessage',
      args: {
        message: 'hello from Takaro',
        recipient: '76561198000000001',
        senderNameOverride: 'Takaro',
      },
    },
    completed,
  );

  const poller = new ModCommandPoller(bridge, {
    sendMessage: async (message) => {
      rendered.push(message);
      return { success: true, sent: true };
    },
  });

  const result = await poller.pollOnce();

  assert.deepEqual(result, { handled: true, action: 'sendMessage', requestId: 'req-1' });
  assert.deepEqual(rendered, [
    {
      requestId: 'req-1',
      message: 'hello from Takaro',
      recipient: '76561198000000001',
      senderNameOverride: 'Takaro',
    },
  ]);
  assert.deepEqual(completed, [
    {
      requestId: 'req-1',
      result: { success: true, sent: true },
    },
  ]);
});

test('mod command poller posts structured failure when renderer fails', async () => {
  const completed: unknown[] = [];
  const bridge = fakeBridge(
    {
      requestId: 'req-2',
      action: 'sendMessage',
      args: {
        message: 'bad render',
      },
    },
    completed,
  );

  const poller = new ModCommandPoller(bridge, {
    sendMessage: async () => {
      throw new Error('renderer unavailable');
    },
  });

  const result = await poller.pollOnce();

  assert.deepEqual(result, { handled: true, action: 'sendMessage', requestId: 'req-2' });
  assert.deepEqual(completed, [
    {
      requestId: 'req-2',
      result: { success: false, error: 'renderer unavailable' },
    },
  ]);
});

test('mod command poller ignores empty polls', async () => {
  const completed: unknown[] = [];
  const bridge = fakeBridge(null, completed);
  const poller = new ModCommandPoller(bridge, {
    sendMessage: async () => {
      throw new Error('should not render');
    },
  });

  const result = await poller.pollOnce();

  assert.deepEqual(result, { handled: false });
  assert.deepEqual(completed, []);
});

test('Pippi RCON renderer sends server-wide messages with Pippi server command', async () => {
  const commands: string[] = [];
  const renderer = new RconChatModRenderer({
    mod: 'pippi',
    rcon: fakeRconOptions(),
    senderName: 'Takaro',
    execute: async (command) => {
      commands.push(command);
      return 'Successfully executed: server Takaro: hello from Takaro';
    },
  });

  const result = await renderer.sendMessage({
    requestId: 'req-pippi',
    message: 'hello | from\nTakaro',
    recipient: null,
    senderNameOverride: 'Takaro',
  });

  assert.deepEqual(commands, ['server Takaro: hello from Takaro']);
  assert.deepEqual(result, {
    success: true,
    sent: true,
    mod: 'pippi',
    command: 'server Takaro: hello from Takaro',
    response: 'Successfully executed: server Takaro: hello from Takaro',
  });
});

test('Pippi RCON renderer maps recipient platform IDs to character names', async () => {
  const commands: string[] = [];
  const renderer = new RconChatModRenderer({
    mod: 'pippi',
    rcon: fakeRconOptions(),
    senderName: 'Takaro',
    execute: async (command) => {
      commands.push(command);
      if (command === 'listplayers') {
        return [
          'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name',
          '  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM',
        ].join('\n');
      }
      return 'Sent message "restart in 5" to player "TestExile"';
    },
  });

  const result = await renderer.sendMessage({
    requestId: 'req-pippi-target',
    message: 'restart in 5',
    recipient: 'steam:76561198000000001',
    senderNameOverride: null,
  });

  assert.deepEqual(commands, ['listplayers', 'directmessage Takaro TestExile restart in 5']);
  assert.deepEqual(result, {
    success: true,
    sent: true,
    mod: 'pippi',
    commands: ['directmessage Takaro TestExile restart in 5'],
    responses: ['Sent message "restart in 5" to player "TestExile"'],
  });
});

test('Amunet RCON renderer formats ast global chat command', async () => {
  const commands: string[] = [];
  const renderer = new RconChatModRenderer({
    mod: 'amunet',
    rcon: fakeRconOptions(),
    senderName: 'Takaro',
    execute: async (command) => {
      commands.push(command);
      return 'ok';
    },
  });

  await renderer.sendMessage({
    requestId: 'req-amunet',
    message: 'restart in 5',
    recipient: null,
    senderNameOverride: null,
  });

  assert.deepEqual(commands, ['ast chat "global" Takaro:restart in 5']);
});

function fakeBridge(command: Awaited<ReturnType<BridgeClient['poll']>>['command'] | null, completed: unknown[]): BridgeClient {
  return {
    poll: async () => (command ? { hasCommand: true, command } : { hasCommand: false }),
    complete: async (requestId, result) => {
      completed.push({ requestId, result });
    },
  };
}

function fakeRconOptions() {
  return {
    host: '127.0.0.1',
    port: 25575,
    password: 'secret',
    timeoutMs: 5000,
  };
}
