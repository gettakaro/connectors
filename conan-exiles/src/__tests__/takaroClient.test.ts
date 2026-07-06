import assert from 'node:assert/strict';
import { once } from 'node:events';
import { test } from 'node:test';
import { WebSocketServer } from 'ws';
import { TakaroWsClient } from '../takaro/client.js';

test('Takaro client disables reconnect after identify credential failure', async () => {
  const wss = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await once(wss, 'listening');
  const address = wss.address();
  assert.notEqual(typeof address, 'string');
  const port = typeof address === 'object' && address ? address.port : 0;

  let connections = 0;
  wss.on('connection', (ws) => {
    connections += 1;
    ws.on('message', () => {
      ws.send(JSON.stringify({ type: 'connected' }));
      ws.send(
        JSON.stringify({
          type: 'identifyResponse',
          payload: {
            error: {
              name: 'BadRequestError',
              message: 'Invalid registrationToken provided',
              http: 400,
            },
          },
        }),
      );
    });
  });

  const client = new TakaroWsClient(
    `ws://127.0.0.1:${port}`,
    {
      identityToken: 'Conan Test',
      registrationToken: 'bad-token',
      name: 'Conan Test',
    },
    10,
    10,
  );

  client.connect();
  await once(client, 'identifyFailed');
  await new Promise((resolve) => setTimeout(resolve, 50));

  assert.equal(connections, 1);
  assert.equal(client.identified(), false);
  assert.equal(client.getGameServerId(), null);
  assert.match(client.getLastIdentifyError()?.message ?? '', /Invalid registrationToken/);

  client.shutdown();
  await new Promise<void>((resolve) => wss.close(() => resolve()));
});
