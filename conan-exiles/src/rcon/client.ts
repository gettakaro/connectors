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

export interface RconConnectionOptions {
  host: string;
  port: number;
  password: string;
  timeoutMs: number;
}

export interface RconCommandOptions extends RconConnectionOptions {
  command: string;
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

interface PendingCommand {
  resolve: (result: string) => void;
  reject: (err: Error) => void;
  timeout: NodeJS.Timeout;
}

export class RconClient {
  private socket: net.Socket | null = null;
  private authenticated = false;
  private connecting: Promise<void> | null = null;
  private pending: PendingCommand | null = null;
  private readonly requestId = 1;

  constructor(private readonly options: RconConnectionOptions) {}

  async run(command: string): Promise<string> {
    if (this.pending) throw new Error('RCON client already has a command in flight');
    await this.ensureConnected();
    return new Promise<string>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.failConnection(new Error(`RCON command timed out after ${this.options.timeoutMs}ms`));
      }, this.options.timeoutMs);
      this.pending = { resolve, reject, timeout };
      this.socket!.write(encodePacket({ id: this.requestId, type: RCON_EXEC_COMMAND, body: command }));
    });
  }

  close(): void {
    this.failConnection(new Error('RCON client closed'));
  }

  private ensureConnected(): Promise<void> {
    if (this.socket && !this.socket.destroyed && this.authenticated) return Promise.resolve();
    if (this.connecting) return this.connecting;

    const socket = net.createConnection({ host: this.options.host, port: this.options.port });
    this.socket = socket;
    this.authenticated = false;
    let buffer = Buffer.alloc(0);

    this.connecting = new Promise<void>((resolve, reject) => {
      let readySettled = false;
      const authTimeout = setTimeout(() => {
        failReady(new Error(`RCON authentication timed out after ${this.options.timeoutMs}ms`));
      }, this.options.timeoutMs);
      const failReady = (err: Error): void => {
        if (readySettled) return;
        readySettled = true;
        clearTimeout(authTimeout);
        this.connecting = null;
        this.resetSocket(socket);
        reject(err);
      };
      const finishReady = (): void => {
        if (readySettled) return;
        readySettled = true;
        clearTimeout(authTimeout);
        this.connecting = null;
        resolve();
      };

      socket.on('connect', () => {
        socket.write(encodePacket({ id: this.requestId, type: RCON_AUTH, body: this.options.password }));
      });
      socket.on('data', (chunk) => {
        buffer = Buffer.concat([buffer, typeof chunk === 'string' ? Buffer.from(chunk) : chunk]);
        try {
          while (true) {
            const decoded = decodePacket(buffer);
            if (!decoded.packet) break;
            buffer = buffer.subarray(decoded.bytesRead);
            if (!this.authenticated) {
              if (decoded.packet.id === -1) {
                failReady(new Error('RCON authentication failed'));
                return;
              }
              if (
                decoded.packet.type === RCON_AUTH_RESPONSE
                || decoded.packet.body.toLowerCase().includes('authenticated')
              ) {
                this.authenticated = true;
                finishReady();
              }
              continue;
            }
            if (decoded.packet.id === this.requestId && this.pending) {
              const pending = this.pending;
              this.pending = null;
              clearTimeout(pending.timeout);
              pending.resolve(decoded.packet.body);
            }
          }
        } catch (err) {
          if (!readySettled) failReady(err as Error);
          else this.failConnection(err as Error);
        }
      });
      socket.on('error', (err) => {
        if (!readySettled) failReady(err);
        else this.failConnection(err);
      });
      socket.on('close', () => {
        const err = new Error('RCON connection closed before response');
        if (!readySettled) failReady(err);
        else if (this.socket === socket) this.failConnection(err);
      });
    });
    return this.connecting;
  }

  private failConnection(err: Error): void {
    const pending = this.pending;
    this.pending = null;
    if (pending) {
      clearTimeout(pending.timeout);
      pending.reject(err);
    }
    const socket = this.socket;
    if (socket) this.resetSocket(socket);
  }

  private resetSocket(socket: net.Socket): void {
    if (this.socket !== socket) return;
    this.socket = null;
    this.authenticated = false;
    socket.destroy();
  }
}

export async function sendRconCommand(options: RconCommandOptions): Promise<string> {
  const client = new RconClient(options);
  try {
    return await client.run(options.command);
  } finally {
    client.close();
  }
}
