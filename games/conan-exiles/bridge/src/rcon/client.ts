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

export type RconConnectionOptions = Omit<RconCommandOptions, 'command'>;

/**
 * One authenticated RCON socket reused for every command.
 *
 * Conan's karma system charges each new RCON connection; a connection per command at the
 * bridge's poll rate drains it in about an hour, after which every connection is denied for
 * ten minutes and every RCON-backed action fails with `write EPIPE`. Commands are sent one
 * at a time: Conan answers with the auth packet id, so a reply cannot be matched to a
 * request by id and the next reply on the socket is the answer to the outstanding command.
 * Any socket error, close or timeout drops the connection and the next command reconnects.
 */
export class PersistentRconClient {
  private socket: net.Socket | null = null;
  private connecting: Promise<net.Socket> | null = null;
  private buffer = Buffer.alloc(0);
  private pending: { resolve: (body: string) => void; reject: (err: Error) => void } | null = null;
  private tail: Promise<unknown> = Promise.resolve();

  constructor(private readonly options: RconConnectionOptions) {}

  run(command: string): Promise<string> {
    const result = this.tail.then(() => this.exchange(command));
    this.tail = result.catch(() => undefined);
    return result;
  }

  close(): void {
    this.drop(new Error('RCON client closed'));
  }

  private async exchange(command: string): Promise<string> {
    const socket = await this.connect();
    return new Promise<string>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.drop(new Error(`RCON command timed out after ${this.options.timeoutMs}ms`));
      }, this.options.timeoutMs);
      this.pending = {
        resolve: (body) => {
          clearTimeout(timeout);
          resolve(body);
        },
        reject: (err) => {
          clearTimeout(timeout);
          reject(err);
        },
      };
      socket.write(encodePacket({ id: 1, type: RCON_EXEC_COMMAND, body: command }));
    });
  }

  private connect(): Promise<net.Socket> {
    if (this.socket) return Promise.resolve(this.socket);
    if (this.connecting) return this.connecting;

    this.connecting = new Promise<net.Socket>((resolve, reject) => {
      const socket = net.createConnection({ host: this.options.host, port: this.options.port });
      let authenticated = false;
      const fail = (err: Error): void => {
        clearTimeout(timeout);
        socket.destroy();
        this.connecting = null;
        reject(err);
      };
      const timeout = setTimeout(
        () => fail(new Error(`RCON authentication timed out after ${this.options.timeoutMs}ms`)),
        this.options.timeoutMs,
      );

      this.buffer = Buffer.alloc(0);
      socket.on('connect', () => {
        socket.write(encodePacket({ id: 1, type: RCON_AUTH, body: this.options.password }));
      });
      socket.on('data', (chunk) => {
        this.buffer = Buffer.concat([this.buffer, typeof chunk === 'string' ? Buffer.from(chunk) : chunk]);
        try {
          while (true) {
            const decoded = decodePacket(this.buffer);
            if (!decoded.packet) break;
            this.buffer = this.buffer.subarray(decoded.bytesRead);

            if (!authenticated) {
              if (decoded.packet.id === -1) {
                fail(new Error('RCON authentication failed'));
                return;
              }
              if (
                decoded.packet.type === RCON_AUTH_RESPONSE ||
                decoded.packet.body.toLowerCase().includes('authenticated')
              ) {
                authenticated = true;
                clearTimeout(timeout);
                this.socket = socket;
                this.connecting = null;
                resolve(socket);
              }
              continue;
            }

            const pending = this.pending;
            this.pending = null;
            pending?.resolve(decoded.packet.body);
          }
        } catch (err) {
          this.drop(err as Error);
        }
      });
      socket.on('error', (err) => {
        if (authenticated) this.drop(err);
        else fail(err);
      });
      socket.on('close', () => {
        if (authenticated) this.drop(new Error('RCON connection closed before response'));
        else fail(new Error('RCON connection closed before authentication'));
      });
    });
    return this.connecting;
  }

  private drop(err: Error): void {
    const socket = this.socket;
    this.socket = null;
    this.buffer = Buffer.alloc(0);
    socket?.destroy();
    const pending = this.pending;
    this.pending = null;
    pending?.reject(err);
  }
}
