import net from 'node:net';
import {
  RCON_AUTH,
  RCON_AUTH_RESPONSE,
  RCON_EXEC_COMMAND,
  RCON_RESPONSE_VALUE,
  decodePacket,
  encodePacket,
} from '../../rcon/client.js';

export interface FakeRconServer {
  server: net.Server;
  port: number;
  /** Every command the server was asked to run, in the order it saw them. */
  commands: string[];
  /** How many TCP connections the server accepted. */
  connections(): number;
  /** Drop every open connection, as Conan does when it restarts. */
  dropConnections(): void;
  close(): Promise<void>;
}

/**
 * A minimal RCON server with Conan's quirks, shared by the protocol and contract tests.
 *
 * Conan answers an authentication packet with id 0 and type 2 and the body
 * `Authenticated.`, and then reuses the auth id on the command reply — a shape a strict
 * RCON client rejects. The defaults here reproduce the real server; the optional
 * arguments let the protocol test drive the other shapes the client must also accept.
 */
export async function startFakeRconServer(
  password: string,
  responses: Record<string, string>,
  authResponseType = RCON_AUTH_RESPONSE,
  authSuccessId: number | null = null,
  commandResponseId: 'command' | 'auth' = 'command',
): Promise<FakeRconServer> {
  const commands: string[] = [];
  const sockets = new Set<net.Socket>();
  let accepted = 0;
  const server = net.createServer((socket) => {
    accepted += 1;
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let buffer = Buffer.alloc(0);

    socket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, typeof chunk === 'string' ? Buffer.from(chunk) : chunk]);

      while (true) {
        const decoded = decodePacket(buffer);
        if (!decoded.packet) break;
        buffer = buffer.subarray(decoded.bytesRead);

        if (decoded.packet.type === RCON_AUTH) {
          const ok = decoded.packet.body === password;
          const id = ok ? (authSuccessId ?? decoded.packet.id) : -1;
          socket.write(encodePacket({ id, type: authResponseType, body: ok ? 'Authenticated.' : '' }));
        }

        if (decoded.packet.type === RCON_EXEC_COMMAND) {
          commands.push(decoded.packet.body);
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
    socket.on('error', () => undefined);
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('the fake RCON server bound no port');

  return {
    server,
    port: address.port,
    commands,
    connections: () => accepted,
    dropConnections: () => {
      for (const socket of sockets) socket.destroy();
    },
    close: () => new Promise<void>((resolve) => server.close(() => resolve())),
  };
}
