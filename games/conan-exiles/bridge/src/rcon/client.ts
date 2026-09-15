import net from 'node:net';

export const RCON_RESPONSE_VALUE = 0;
export const RCON_EXEC_COMMAND = 2;
export const RCON_AUTH_RESPONSE = 2;
export const RCON_AUTH = 3;

export interface RconPacket {
  id: number;
  type: number;
  body: string;
}

export interface RconCommandOptions {
  host: string;
  port: number;
  password: string;
  command: string;
  timeoutMs: number;
}

export function encodePacket(packet: RconPacket): Buffer {
  const body = Buffer.from(packet.body, 'utf8');
  const size = 4 + 4 + body.length + 2;
  const buffer = Buffer.alloc(4 + size);
  buffer.writeInt32LE(size, 0);
  buffer.writeInt32LE(packet.id, 4);
  buffer.writeInt32LE(packet.type, 8);
  body.copy(buffer, 12);
  buffer.writeUInt8(0, 12 + body.length);
  buffer.writeUInt8(0, 13 + body.length);
  return buffer;
}

export function decodePacket(buffer: Buffer): { packet: RconPacket | null; bytesRead: number } {
  if (buffer.length < 4) return { packet: null, bytesRead: 0 };
  const size = buffer.readInt32LE(0);
  const total = 4 + size;
  if (buffer.length < total) return { packet: null, bytesRead: 0 };
  if (size < 10) throw new Error(`Invalid RCON packet size: ${size}`);

  const bodyEnd = total - 2;
  return {
    packet: {
      id: buffer.readInt32LE(4),
      type: buffer.readInt32LE(8),
      body: buffer.subarray(12, bodyEnd).toString('utf8'),
    },
    bytesRead: total,
  };
}

export async function sendRconCommand(options: RconCommandOptions): Promise<string> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: options.host, port: options.port });
    const requestId = 1;
    let buffer = Buffer.alloc(0);
    let authenticated = false;
    let settled = false;

    const finish = (err: Error | null, result = ''): void => {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      socket.destroy();
      if (err) reject(err);
      else resolve(result);
    };

    const timeout = setTimeout(() => {
      finish(new Error(`RCON command timed out after ${options.timeoutMs}ms`));
    }, options.timeoutMs);

    socket.on('connect', () => {
      socket.write(encodePacket({ id: requestId, type: RCON_AUTH, body: options.password }));
    });

    socket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, typeof chunk === 'string' ? Buffer.from(chunk) : chunk]);

      try {
        while (true) {
          const decoded = decodePacket(buffer);
          if (!decoded.packet) break;
          buffer = buffer.subarray(decoded.bytesRead);

          if (!authenticated) {
            if (decoded.packet.id === -1) {
              finish(new Error('RCON authentication failed'));
              return;
            }
            if (decoded.packet.type === RCON_AUTH_RESPONSE || decoded.packet.body.toLowerCase().includes('authenticated')) {
              authenticated = true;
              socket.write(encodePacket({ id: requestId, type: RCON_EXEC_COMMAND, body: options.command }));
            }
            continue;
          }

          if (decoded.packet.id === requestId) {
            finish(null, decoded.packet.body);
            return;
          }
        }
      } catch (err) {
        finish(err as Error);
      }
    });

    socket.on('error', (err) => finish(err));
    socket.on('close', () => {
      if (!settled) finish(new Error('RCON connection closed before response'));
    });
  });
}
