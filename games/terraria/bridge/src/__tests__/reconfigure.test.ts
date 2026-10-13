import assert from 'node:assert/strict';
import { test } from 'node:test';
import { WebSocketServer } from 'ws';
import { TakaroWsClient } from '../takaro/client.js';
import type { IdentifyPayload, WsMessage } from '../takaro/protocol.js';

/** A stand-in Takaro that accepts one registration token and records every identify. */
async function fakeTakaro(validToken: string) {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const identifies: IdentifyPayload[] = [];
  server.on('connection', (socket) => {
    socket.on('message', (raw) => {
      const message = JSON.parse(raw.toString()) as WsMessage;
      if (message.type !== 'identify') return;
      const payload = message.payload as IdentifyPayload;
      identifies.push(payload);
      const ok = payload.registrationToken === validToken;
      socket.send(JSON.stringify({
        type: 'identifyResponse',
        payload: ok ? { gameServerId: `gs-${payload.identityToken}` } : { error: { message: 'Invalid registrationToken provided' } },
      }));
      if (!ok) socket.close();
    });
  });
  return {
    url: `ws://127.0.0.1:${address.port}`,
    identifies,
    async close() {
      for (const socket of server.clients) socket.terminate();
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
  };
}

async function waitFor(check: () => boolean, timeoutMs = 3000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('timed out');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

const payload = (registrationToken: string, identityToken = 'id-1'): IdentifyPayload => ({
  registrationToken,
  identityToken,
  name: 'Terraria Test',
});

test('without a registration token the client does not connect; saving one connects at once', async () => {
  const takaro = await fakeTakaro('good');
  const client = new TakaroWsClient(takaro.url, payload(''));
  try {
    client.connect();
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(takaro.identifies.length, 0);
    assert.equal(client.active(), false);

    assert.equal(client.reconfigure(takaro.url, payload('good')), true);
    await waitFor(() => client.identified());
    assert.equal(client.getGameServerId(), 'gs-id-1');
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test('a rejected token in backoff is replaced without waiting for the backoff', async () => {
  const takaro = await fakeTakaro('good');
  // A one-minute backoff: only a reconnect that skips it can pass the 3 s wait below.
  const client = new TakaroWsClient(takaro.url, payload('wrong'), 60_000, 60_000);
  const errors: unknown[] = [];
  client.on('identifyError', (error) => errors.push(error));
  try {
    client.connect();
    await waitFor(() => errors.length === 1);
    // The fake drops the socket after a failed identify, like Takaro; the client now waits a minute.
    await waitFor(() => client.active() && !client.identified() && takaro.identifies.length === 1);
    await new Promise((resolve) => setTimeout(resolve, 50));
    client.reconfigure(takaro.url, payload('good'));
    await waitFor(() => client.identified());
    assert.equal(takaro.identifies.at(-1)?.registrationToken, 'good');
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test("a dropped socket's late close does not disconnect or reschedule the new one", async () => {
  const takaro = await fakeTakaro('good');
  const client = new TakaroWsClient(takaro.url, payload('good', 'id-1'), 50, 50);
  let disconnects = 0;
  client.on('disconnected', () => disconnects++);
  try {
    client.connect();
    await waitFor(() => client.identified());
    client.reconfigure(takaro.url, payload('good', 'id-2'));
    await waitFor(() => client.getGameServerId() === 'gs-id-2');
    const settled = disconnects;
    // Long enough for the old socket's close and any reconnect it might have scheduled.
    await new Promise((resolve) => setTimeout(resolve, 300));
    assert.equal(client.getGameServerId(), 'gs-id-2');
    assert.equal(disconnects, settled);
    assert.deepEqual(takaro.identifies.map((entry) => entry.identityToken), ['id-1', 'id-2']);
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test('the same settings again do not reconnect', async () => {
  const takaro = await fakeTakaro('good');
  const client = new TakaroWsClient(takaro.url, payload('good'));
  try {
    client.connect();
    await waitFor(() => client.identified());
    assert.equal(client.reconfigure(takaro.url, payload('good')), false);
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(takaro.identifies.length, 1);
  } finally {
    client.shutdown();
    await takaro.close();
  }
});

test('shutdown wins over a reconfigure: nothing reconnects afterwards', async () => {
  const takaro = await fakeTakaro('good');
  const client = new TakaroWsClient(takaro.url, payload('good'), 20, 20);
  try {
    client.connect();
    await waitFor(() => client.identified());
    client.shutdown();
    client.reconfigure(takaro.url, payload('good', 'id-2'));
    await new Promise((resolve) => setTimeout(resolve, 200));
    assert.equal(client.active(), false);
    assert.equal(takaro.identifies.length, 1);
  } finally {
    await takaro.close();
  }
});
