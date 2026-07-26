import assert from 'node:assert/strict';
import net from 'node:net';
import type { AddressInfo } from 'node:net';
import { after, test } from 'node:test';
import {
  RCON_AUTH,
  RCON_AUTH_RESPONSE,
  RCON_EXEC_COMMAND,
  RCON_RESPONSE_VALUE,
  RconClient,
  decodePacket,
  encodePacket,
  sendRconCommand,
} from '../rcon/client.js';

const servers: net.Server[] = [];

after(async () => {
  await Promise.all(
    servers.map(
      (server) =>
        new Promise<void>((resolve) => {
          server.close(() => resolve());
        }),
    ),
  );
});

test('encodes and decodes RCON packets', () => {
  const encoded = encodePacket({ id: 42, type: RCON_EXEC_COMMAND, body: 'listplayers' });
  const decoded = decodePacket(encoded);

  assert.equal(decoded.bytesRead, encoded.length);
  assert.deepEqual(decoded.packet, {
    id: 42,
    type: RCON_EXEC_COMMAND,
    body: 'listplayers',
  });
});

test('returns null packet when buffer is incomplete', () => {
  const encoded = encodePacket({ id: 1, type: RCON_EXEC_COMMAND, body: 'help' });

  assert.equal(decodePacket(encoded.subarray(0, 6)).packet, null);
});

test('authenticates and executes a command against an RCON server', async () => {
  const server = await startFakeRconServer('secret', {
    listplayers: '0. Alice | 76561198000000001',
  });
  const address = server.address() as AddressInfo;

  const response = await sendRconCommand({
    host: '127.0.0.1',
    port: address.port,
    password: 'secret',
    command: 'listplayers',
    timeoutMs: 1000,
  });

  assert.equal(response, '0. Alice | 76561198000000001');
});

test('accepts Conan-style auth response type before executing a command', async () => {
  const server = await startFakeRconServer('secret', {
    help: 'Commands: listplayers',
  }, RCON_AUTH_RESPONSE, 0, 'auth');
  const address = server.address() as AddressInfo;

  const response = await sendRconCommand({
    host: '127.0.0.1',
    port: address.port,
    password: 'secret',
    command: 'help',
    timeoutMs: 1000,
  });

  assert.equal(response, 'Commands: listplayers');
});

test('reuses one authenticated RCON connection for sequential commands', async () => {
  let connectionCount = 0;
  const server = await startFakeRconServer('secret', {
    help: 'Commands: listplayers',
    listplayers: '0. Alice | 76561198000000001',
  }, RCON_AUTH_RESPONSE, 0, 'auth', () => { connectionCount += 1; });
  const address = server.address() as AddressInfo;
  const client = new RconClient({ host: '127.0.0.1', port: address.port, password: 'secret', timeoutMs: 1000 });

  try {
    assert.equal(await client.run('help'), 'Commands: listplayers');
    assert.equal(await client.run('listplayers'), '0. Alice | 76561198000000001');
    assert.equal(connectionCount, 1);
  } finally {
    client.close();
  }
});

test('rejects invalid RCON credentials', async () => {
  const server = await startFakeRconServer('secret', {});
  const address = server.address() as AddressInfo;

  await assert.rejects(
    () =>
      sendRconCommand({
        host: '127.0.0.1',
        port: address.port,
        password: 'wrong',
        command: 'help',
        timeoutMs: 1000,
      }),
    /RCON authentication failed/,
  );
});

async function startFakeRconServer(
  password: string,
  responses: Record<string, string>,
  authResponseType = RCON_AUTH_RESPONSE,
  authSuccessId: number | null = null,
  commandResponseId: 'command' | 'auth' = 'command',
  onConnection: (() => void) | null = null,
): Promise<net.Server> {
  const server = net.createServer((socket) => {
    onConnection?.();
    let buffer = Buffer.alloc(0);

    socket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, typeof chunk === 'string' ? Buffer.from(chunk) : chunk]);

      while (true) {
        const decoded = decodePacket(buffer);
        if (!decoded.packet) break;
        buffer = buffer.subarray(decoded.bytesRead);

        if (decoded.packet.type === RCON_AUTH) {
          const ok = decoded.packet.body === password;
          const id = ok ? authSuccessId ?? decoded.packet.id : -1;
          socket.write(encodePacket({ id, type: authResponseType, body: ok ? 'Authenticated.' : '' }));
        }

        if (decoded.packet.type === RCON_EXEC_COMMAND) {
          socket.write(
            encodePacket({
              id: commandResponseId === 'auth' ? 1 : decoded.packet.id,
              type: RCON_RESPONSE_VALUE,
              body: responses[decoded.packet.body] ?? `ran:${decoded.packet.body}`,
            }),
          );
        }
      }
    });
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  servers.push(server);
  return server;
}
