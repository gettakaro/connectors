import type http from 'node:http';
import { randomUUID } from 'node:crypto';
import type { GameEventType } from '../takaro/protocol.js';

export interface ModCommand {
  requestId: string;
  action: string;
  args: Record<string, unknown>;
}

export interface ModCommandBridgeOptions {
  resultTimeoutMs?: number;
  connectedTtlMs?: number;
  emitGameEvent?: (type: GameEventType, data: unknown) => void;
}

interface PendingCommand {
  resolve: (result: unknown) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

export class ModCommandBridge {
  private readonly queue: ModCommand[] = [];
  private readonly pending = new Map<string, PendingCommand>();
  private readonly resultTimeoutMs: number;
  private readonly connectedTtlMs: number;
  private lastPollAt = 0;
  private emitGameEvent?: (type: GameEventType, data: unknown) => void;

  constructor(options: ModCommandBridgeOptions = {}) {
    this.resultTimeoutMs = options.resultTimeoutMs ?? 10000;
    this.connectedTtlMs = options.connectedTtlMs ?? 30000;
    this.emitGameEvent = options.emitGameEvent;
  }

  isConnected(): boolean {
    return this.lastPollAt > 0 && Date.now() - this.lastPollAt <= this.connectedTtlMs;
  }

  status(): { connected: boolean; pendingCommands: number; pendingResults: number; lastPollAt: string | null } {
    return {
      connected: this.isConnected(),
      pendingCommands: this.queue.length,
      pendingResults: this.pending.size,
      lastPollAt: this.lastPollAt ? new Date(this.lastPollAt).toISOString() : null,
    };
  }

  sendMessage(message: string, recipient: string | null, senderNameOverride: string | null = null): Promise<unknown> {
    return this.enqueue('sendMessage', {
      message,
      ...(recipient ? { recipient } : {}),
      ...(senderNameOverride ? { senderNameOverride } : {}),
    });
  }

  pollCommand(): ModCommand | null {
    this.lastPollAt = Date.now();
    return this.queue.shift() ?? null;
  }

  complete(requestId: string, result: unknown): boolean {
    const pending = this.pending.get(requestId);
    if (!pending) return false;
    clearTimeout(pending.timer);
    this.pending.delete(requestId);
    pending.resolve(result);
    return true;
  }

  async handleHttpRequest(req: http.IncomingMessage, res: http.ServerResponse): Promise<boolean> {
    const pathname = req.url?.split('?')[0];

    if (req.method === 'GET' && pathname === '/mod/poll') {
      const command = this.pollCommand();
      sendJson(res, 200, command ? { hasCommand: true, command } : { hasCommand: false });
      return true;
    }

    if (req.method === 'POST' && pathname === '/mod/result') {
      const body = await readJson(req);
      const requestId = typeof body.requestId === 'string' ? body.requestId : '';
      if (!requestId) {
        sendJson(res, 400, { error: 'Missing requestId' });
        return true;
      }
      const known = this.complete(requestId, body.result ?? {});
      sendJson(res, known ? 200 : 404, known ? { success: true } : { error: 'Unknown requestId' });
      return true;
    }

    if (req.method === 'POST' && pathname === '/mod/event') {
      const body = await readJson(req);
      if (typeof body.type !== 'string') {
        sendJson(res, 400, { error: 'Missing event type' });
        return true;
      }
      this.emitGameEvent?.(body.type as GameEventType, body.data ?? {});
      sendJson(res, 200, { success: true });
      return true;
    }

    return false;
  }

  private enqueue(action: string, args: Record<string, unknown>): Promise<unknown> {
    const requestId = randomUUID();
    this.queue.push({ requestId, action, args });

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error(`Conan mod bridge command ${requestId} timed out after ${this.resultTimeoutMs}ms`));
      }, this.resultTimeoutMs);
      this.pending.set(requestId, { resolve, reject, timer });
    });
  }
}

async function readJson(req: http.IncomingMessage): Promise<Record<string, unknown>> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }
  if (chunks.length === 0) return {};
  return JSON.parse(Buffer.concat(chunks).toString('utf8')) as Record<string, unknown>;
}

function sendJson(res: http.ServerResponse, statusCode: number, body: unknown): void {
  const raw = JSON.stringify(body);
  res.statusCode = statusCode;
  res.setHeader('Content-Type', 'application/json');
  res.setHeader('Content-Length', Buffer.byteLength(raw));
  res.end(raw);
}
