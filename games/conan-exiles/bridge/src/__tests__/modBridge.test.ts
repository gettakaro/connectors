import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { ModCommandBridge } from '../mod/commandBridge.js';

test('mod command bridge queues sendMessage commands and resolves results', async () => {
  const bridge = new ModCommandBridge({ resultTimeoutMs: 1000 });

  const pending = bridge.sendMessage('hello from Takaro', '76561198000735875');
  const polled = bridge.pollCommand();

  assert.equal(bridge.isConnected(), true);
  assert.equal(polled?.action, 'sendMessage');
  assert.deepEqual(polled?.args, {
    message: 'hello from Takaro',
    recipient: '76561198000735875',
  });

  bridge.complete(polled!.requestId, { success: true, sent: true });

  assert.deepEqual(await pending, { success: true, sent: true });
});

test('mod command bridge reports unavailable when no mod has polled yet', async () => {
  const bridge = new ModCommandBridge({ resultTimeoutMs: 1000 });

  assert.equal(bridge.isConnected(), false);
});

test('mod command bridge exposes poll and result source attribution', async () => {
  const bridge = new ModCommandBridge({ resultTimeoutMs: 1000 });

  const pending = bridge.sendMessage('source check', null);
  const polled = bridge.pollCommand('TakaroConan/1.0');

  assert.equal(polled?.action, 'sendMessage');
  assert.equal(bridge.status().connected, true);
  assert.equal(bridge.status().pendingCommands, 0);
  assert.equal(bridge.status().pendingResults, 1);
  assert.equal(bridge.status().lastPollSource, 'TakaroConan/1.0');
  assert.equal(bridge.status().lastResultAt, null);
  assert.equal(bridge.status().lastResultSource, null);

  bridge.complete(polled!.requestId, { success: true, sent: true }, 'TakaroConan/1.0');

  assert.deepEqual(await pending, { success: true, sent: true });
  assert.match(bridge.status().lastResultAt ?? '', /^\d{4}-\d{2}-\d{2}T/);
  assert.equal(bridge.status().lastResultSource, 'TakaroConan/1.0');
  assert.equal(bridge.status().recentResults.length, 1);
  assert.equal(bridge.status().recentResults[0]?.action, 'sendMessage');
  assert.equal(bridge.status().recentResults[0]?.message, 'source check');
  assert.equal(bridge.status().recentResults[0]?.source, 'TakaroConan/1.0');
  assert.equal(bridge.status().recentResults[0]?.resultSuccess, true);
});

test('mod command bridge records HTTP source attribution for poll, result, and event', async () => {
  const events: Array<{ type: string; data: unknown }> = [];
  const bridge = new ModCommandBridge({
    resultTimeoutMs: 1000,
    emitGameEvent: (type, data) => events.push({ type, data }),
  });
  const server = http.createServer((req, res) => {
    void bridge.handleHttpRequest(req, res);
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  assert(address && typeof address !== 'string');
  const baseUrl = `http://127.0.0.1:${address.port}`;

  try {
    const pending = bridge.sendMessage('http source check', null);
    const pollResponse = await fetch(`${baseUrl}/mod/poll?source=TakaroConan/1.0`);
    const pollBody = (await pollResponse.json()) as { command: { requestId: string } };

    assert.equal(pollResponse.status, 200);
    assert.equal(bridge.status().lastPollSource, 'TakaroConan/1.0');

    const resultResponse = await fetch(`${baseUrl}/mod/result`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        requestId: pollBody.command.requestId,
        result: { success: true, sent: true },
      }),
    });

    assert.equal(resultResponse.status, 200);
    assert.deepEqual(await pending, { success: true, sent: true });
    assert.match(bridge.status().lastResultAt ?? '', /^\d{4}-\d{2}-\d{2}T/);
    assert.equal(bridge.status().lastResultSource, 'TakaroConan/1.0');
    assert.equal(bridge.status().recentResults[0]?.message, 'http source check');
    assert.equal(bridge.status().recentResults[0]?.source, 'TakaroConan/1.0');

    const eventResponse = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        type: 'chat-message',
        data: {
          message: 'hello from Conan',
          player: {
            gameId: '76561198000735875',
            platformId: 'steam:76561198000735875',
            name: 'Limon#67642',
          },
        },
      }),
    });

    assert.equal(eventResponse.status, 200);
    assert.deepEqual(events, [{
      type: 'chat-message',
      data: {
        message: 'hello from Conan',
        player: {
          gameId: '76561198000735875',
          platformId: 'steam:76561198000735875',
          name: 'Limon#67642',
        },
      },
    }]);
    assert.equal(bridge.status().lastEventSource, 'TakaroConan/1.0');
    assert.equal(bridge.status().lastEventType, 'chat-message');
    assert.match(bridge.status().lastEventAt ?? '', /^\d{4}-\d{2}-\d{2}T/);
    assert.equal(bridge.status().recentEvents[0]?.message, 'hello from Conan');
    assert.equal(bridge.status().recentEvents[0]?.playerGameId, '76561198000735875');
    assert.equal(bridge.status().recentEvents[0]?.playerPlatformId, 'steam:76561198000735875');
    assert.equal(bridge.status().recentEvents[0]?.source, 'TakaroConan/1.0');
  } finally {
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

test('mod command bridge can require source attribution for poll, result, and event', async () => {
  const events: Array<{ type: string; data: unknown }> = [];
  const bridge = new ModCommandBridge({
    resultTimeoutMs: 1000,
    requireSourceAttribution: true,
    emitGameEvent: (type, data) => events.push({ type, data }),
  });
  const server = http.createServer((req, res) => {
    void bridge.handleHttpRequest(req, res);
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  assert(address && typeof address !== 'string');
  const baseUrl = `http://127.0.0.1:${address.port}`;

  try {
    assert.equal(bridge.status().sourceAttributionRequired, true);

    const anonymousPoll = await fetch(`${baseUrl}/mod/poll`);
    assert.equal(anonymousPoll.status, 400);

    const userAgentOnlyPoll = await fetch(`${baseUrl}/mod/poll`, {
      headers: { 'user-agent': 'TakaroConan/1.0' },
    });
    assert.equal(userAgentOnlyPoll.status, 400);

    const wrongSourcePoll = await fetch(`${baseUrl}/mod/poll?source=FakeConanMod/1.0`);
    assert.equal(wrongSourcePoll.status, 400);

    const pending = bridge.sendMessage('strict source check', null);
    const sourcedPoll = await fetch(`${baseUrl}/mod/poll?source=TakaroConan/1.0`);
    assert.equal(sourcedPoll.status, 200);
    const pollBody = (await sourcedPoll.json()) as { command: { requestId: string } };
    assert.equal(bridge.status().lastPollSource, 'TakaroConan/1.0');

    const anonymousResult = await fetch(`${baseUrl}/mod/result`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        requestId: pollBody.command.requestId,
        result: { success: true, sent: true },
      }),
    });
    assert.equal(anonymousResult.status, 400);
    assert.equal(bridge.status().pendingResults, 1);

    const userAgentOnlyResult = await fetch(`${baseUrl}/mod/result`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'user-agent': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        requestId: pollBody.command.requestId,
        result: { success: true, sent: true },
      }),
    });
    assert.equal(userAgentOnlyResult.status, 400);
    assert.equal(bridge.status().pendingResults, 1);

    const wrongSourceResult = await fetch(`${baseUrl}/mod/result`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'FakeConanMod/1.0',
      },
      body: JSON.stringify({
        requestId: pollBody.command.requestId,
        result: { success: true, sent: true },
      }),
    });
    assert.equal(wrongSourceResult.status, 400);
    assert.equal(bridge.status().pendingResults, 1);

    const sourcedResult = await fetch(`${baseUrl}/mod/result`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        requestId: pollBody.command.requestId,
        result: { success: true, sent: true },
      }),
    });
    assert.equal(sourcedResult.status, 200);
    assert.deepEqual(await pending, { success: true, sent: true });
    assert.equal(bridge.status().lastResultSource, 'TakaroConan/1.0');

    const anonymousEvent = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        type: 'chat-message',
        data: { message: 'anonymous should be rejected' },
      }),
    });
    assert.equal(anonymousEvent.status, 400);
    assert.deepEqual(events, []);

    const userAgentOnlyEvent = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'user-agent': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        type: 'chat-message',
        data: { message: 'user-agent source should be rejected' },
      }),
    });
    assert.equal(userAgentOnlyEvent.status, 400);
    assert.deepEqual(events, []);

    const wrongSourceEvent = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'FakeConanMod/1.0',
      },
      body: JSON.stringify({
        type: 'chat-message',
        data: { message: 'wrong source should be rejected' },
      }),
    });
    assert.equal(wrongSourceEvent.status, 400);
    assert.deepEqual(events, []);

    const sourcedEvent = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        type: 'chat-message',
        data: {
          message: 'sourced event',
          player: { gameId: '76561198000735875', name: 'Limon#67642' },
        },
      }),
    });
    assert.equal(sourcedEvent.status, 200);
    assert.equal(events.length, 1);
    assert.equal(bridge.status().lastEventSource, 'TakaroConan/1.0');
    assert.equal(bridge.status().recentEvents[0]?.message, 'sourced event');
  } finally {
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

test('mod command bridge rejects events that fail strict validation before forwarding', async () => {
  const events: Array<{ type: string; data: unknown }> = [];
  const bridge = new ModCommandBridge({
    validateGameEvent: (type, data) => {
      const record = data && typeof data === 'object' && !Array.isArray(data) ? data as Record<string, unknown> : null;
      if (type === 'chat-message' && record?.message !== 'allowed') throw new Error('invalid test event');
    },
    emitGameEvent: (type, data) => events.push({ type, data }),
  });
  const server = http.createServer((req, res) => {
    void bridge.handleHttpRequest(req, res);
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  assert(address && typeof address !== 'string');
  const baseUrl = `http://127.0.0.1:${address.port}`;

  try {
    assert.equal(bridge.status().gameEventValidationEnabled, true);
    const rejected = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        type: 'chat-message',
        data: { message: 'spoofed' },
      }),
    });
    assert.equal(rejected.status, 400);
    assert.deepEqual(events, []);
    assert.equal(bridge.status().lastEventType, null);

    const accepted = await fetch(`${baseUrl}/mod/event`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-takaro-mod-source': 'TakaroConan/1.0',
      },
      body: JSON.stringify({
        type: 'chat-message',
        data: { message: 'allowed' },
      }),
    });
    assert.equal(accepted.status, 200);
    assert.equal(events.length, 1);
    assert.equal(bridge.status().lastEventType, 'chat-message');
  } finally {
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});
