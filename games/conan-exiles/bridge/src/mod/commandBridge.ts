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
  requireSourceAttribution?: boolean;
  requiredSourcePattern?: RegExp;
  validateGameEvent?: (type: string, data: unknown) => Promise<void> | void;
  emitGameEvent?: (type: GameEventType, data: unknown) => void;
}

interface PendingCommand {
  resolve: (result: unknown) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
  command: ModCommand;
}

interface CompletedCommandTrace {
  requestId: string;
  action: string;
  source: string | null;
  completedAt: string;
  message: string | null;
  hasRecipient: boolean;
  recipient: string | null;
  resultSuccess: boolean | null;
  resultError: string | null;
}

interface EventTrace {
  type: string;
  source: string | null;
  receivedAt: string;
  message: string | null;
  playerGameId: string | null;
  playerSteamId: string | null;
  playerPlatformId: string | null;
  playerName: string | null;
}

export class ModCommandBridge {
  private readonly queue: ModCommand[] = [];
  private readonly pending = new Map<string, PendingCommand>();
  private readonly resultTimeoutMs: number;
  private readonly connectedTtlMs: number;
  private readonly requireSourceAttribution: boolean;
  private readonly requiredSourcePattern: RegExp | null;
  private readonly validateGameEvent?: (type: string, data: unknown) => Promise<void> | void;
  private lastPollAt = 0;
  private lastPollSource: string | null = null;
  private lastResultAt = 0;
  private lastResultSource: string | null = null;
  private lastEventAt = 0;
  private lastEventSource: string | null = null;
  private lastEventType: string | null = null;
  private readonly recentResults: CompletedCommandTrace[] = [];
  private readonly recentEvents: EventTrace[] = [];
  private emitGameEvent?: (type: GameEventType, data: unknown) => void;

  constructor(options: ModCommandBridgeOptions = {}) {
    this.resultTimeoutMs = options.resultTimeoutMs ?? 10000;
    this.connectedTtlMs = options.connectedTtlMs ?? 30000;
    this.requireSourceAttribution = options.requireSourceAttribution ?? false;
    this.requiredSourcePattern = options.requiredSourcePattern ?? (this.requireSourceAttribution ? /^TakaroConan($|[ /:@+-])/i : null);
    this.validateGameEvent = options.validateGameEvent;
    this.emitGameEvent = options.emitGameEvent;
  }

  isConnected(): boolean {
    return this.lastPollAt > 0 && Date.now() - this.lastPollAt <= this.connectedTtlMs;
  }

  status(): {
    connected: boolean;
    pendingCommands: number;
    pendingResults: number;
    lastPollAt: string | null;
    lastPollSource: string | null;
    lastResultAt: string | null;
    lastResultSource: string | null;
    lastEventAt: string | null;
    lastEventSource: string | null;
    lastEventType: string | null;
    recentResults: CompletedCommandTrace[];
    recentEvents: EventTrace[];
    sourceAttributionRequired: boolean;
    gameEventValidationEnabled: boolean;
  } {
    return {
      connected: this.isConnected(),
      pendingCommands: this.queue.length,
      pendingResults: this.pending.size,
      lastPollAt: this.lastPollAt ? new Date(this.lastPollAt).toISOString() : null,
      lastPollSource: this.lastPollSource,
      lastResultAt: this.lastResultAt ? new Date(this.lastResultAt).toISOString() : null,
      lastResultSource: this.lastResultSource,
      lastEventAt: this.lastEventAt ? new Date(this.lastEventAt).toISOString() : null,
      lastEventSource: this.lastEventSource,
      lastEventType: this.lastEventType,
      recentResults: [...this.recentResults],
      recentEvents: [...this.recentEvents],
      sourceAttributionRequired: this.requireSourceAttribution,
      gameEventValidationEnabled: Boolean(this.validateGameEvent),
    };
  }

  sendMessage(message: string, recipient: string | null, senderNameOverride: string | null = null): Promise<unknown> {
    return this.enqueue('sendMessage', {
      message,
      ...(recipient ? { recipient } : {}),
      ...(senderNameOverride ? { senderNameOverride } : {}),
    });
  }

  pollCommand(source: string | null = null): ModCommand | null {
    this.lastPollAt = Date.now();
    this.lastPollSource = source;
    return this.queue.shift() ?? null;
  }

  complete(requestId: string, result: unknown, source: string | null = null): boolean {
    const pending = this.pending.get(requestId);
    if (!pending) return false;
    clearTimeout(pending.timer);
    this.pending.delete(requestId);
    this.lastResultAt = Date.now();
    this.lastResultSource = source;
    this.recordCompletedCommand(pending.command, result, source);
    pending.resolve(result);
    return true;
  }

  async handleHttpRequest(req: http.IncomingMessage, res: http.ServerResponse): Promise<boolean> {
    const pathname = req.url?.split('?')[0];

    if (req.method === 'GET' && pathname === '/mod/poll') {
      const source = sourceFromRequest(req, !this.requireSourceAttribution);
      const sourceError = this.validateSource(source);
      if (sourceError) {
        sendJson(res, 400, { error: sourceError });
        return true;
      }
      const command = this.pollCommand(source);
      sendJson(res, 200, command ? { hasCommand: true, command } : { hasCommand: false });
      return true;
    }

    if (req.method === 'POST' && pathname === '/mod/result') {
      const source = sourceFromRequest(req, !this.requireSourceAttribution);
      const sourceError = this.validateSource(source);
      if (sourceError) {
        sendJson(res, 400, { error: sourceError });
        return true;
      }
      const body = await readJson(req);
      const requestId = typeof body.requestId === 'string' ? body.requestId : '';
      if (!requestId) {
        sendJson(res, 400, { error: 'Missing requestId' });
        return true;
      }
      const known = this.complete(requestId, body.result ?? {}, source);
      sendJson(res, known ? 200 : 404, known ? { success: true } : { error: 'Unknown requestId' });
      return true;
    }

    if (req.method === 'POST' && pathname === '/mod/event') {
      const source = sourceFromRequest(req, !this.requireSourceAttribution);
      const sourceError = this.validateSource(source);
      if (sourceError) {
        sendJson(res, 400, { error: sourceError });
        return true;
      }
      const body = await readJson(req);
      if (typeof body.type !== 'string') {
        sendJson(res, 400, { error: 'Missing event type' });
        return true;
      }
      try {
        await this.validateGameEvent?.(body.type, body.data);
      } catch (err) {
        sendJson(res, 400, { error: err instanceof Error ? err.message : String(err) });
        return true;
      }
      this.recordEventSource(body.type, body.data, source);
      this.emitGameEvent?.(body.type as GameEventType, body.data ?? {});
      sendJson(res, 200, { success: true });
      return true;
    }

    return false;
  }

  private enqueue(action: string, args: Record<string, unknown>): Promise<unknown> {
    const requestId = randomUUID();
    const command = { requestId, action, args };
    this.queue.push(command);

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error(`Conan mod bridge command ${requestId} timed out after ${this.resultTimeoutMs}ms`));
      }, this.resultTimeoutMs);
      this.pending.set(requestId, { resolve, reject, timer, command });
    });
  }

  private recordCompletedCommand(command: ModCommand, result: unknown, source: string | null): void {
    const completedAt = new Date(this.lastResultAt).toISOString();
    const resultRecord = recordValue(result);
    const trace: CompletedCommandTrace = {
      requestId: command.requestId,
      action: command.action,
      source,
      completedAt,
      message: stringValue(command.args.message),
      hasRecipient: command.args.recipient !== undefined && command.args.recipient !== null,
      recipient: recipientValue(command.args.recipient),
      resultSuccess: booleanValue(resultRecord?.success),
      resultError: stringValue(resultRecord?.error),
    };
    this.recentResults.push(trace);
    trimRecent(this.recentResults);
  }

  private validateSource(source: string | null): string | null {
    if (this.requireSourceAttribution && !source) {
      return 'Missing mod source attribution';
    }
    if (source && this.requiredSourcePattern && !this.requiredSourcePattern.test(source)) {
      return 'Invalid mod source attribution';
    }
    return null;
  }

  private recordEventSource(type: string, data: unknown, source: string | null): void {
    this.lastEventAt = Date.now();
    this.lastEventSource = source;
    this.lastEventType = type;
    const eventData = recordValue(data);
    const player = recordValue(eventData?.player);
    this.recentEvents.push({
      type,
      source,
      receivedAt: new Date(this.lastEventAt).toISOString(),
      message: stringValue(eventData?.message),
      playerGameId: stringValue(player?.gameId),
      playerSteamId: stringValue(player?.steamId),
      playerPlatformId: stringValue(player?.platformId),
      playerName: stringValue(player?.name ?? player?.playerName),
    });
    trimRecent(this.recentEvents);
  }
}

function trimRecent<T>(items: T[], max = 10): void {
  while (items.length > max) items.shift();
}

function recordValue(value: unknown): Record<string, unknown> | null {
  return value && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : null;
}

function stringValue(value: unknown): string | null {
  return typeof value === 'string' && value ? value : null;
}

function booleanValue(value: unknown): boolean | null {
  return typeof value === 'boolean' ? value : null;
}

function recipientValue(value: unknown): string | null {
  if (typeof value === 'string' && value) return value;
  const record = recordValue(value);
  return stringValue(record?.gameId) ?? stringValue(record?.steamId) ?? stringValue(record?.platformId) ?? stringValue(record?.name);
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

function sourceFromRequest(req: http.IncomingMessage, allowUserAgent = true): string | null {
  const url = new URL(req.url || '/', 'http://127.0.0.1');
  const querySource = url.searchParams.get('source');
  if (querySource) return cleanSource(querySource);
  const headerSource = req.headers['x-takaro-mod-source'];
  if (typeof headerSource === 'string' && headerSource) return cleanSource(headerSource);
  if (!allowUserAgent) return null;
  const userAgent = req.headers['user-agent'];
  if (typeof userAgent === 'string' && userAgent) return cleanSource(userAgent);
  return null;
}

function cleanSource(value: string): string {
  return value.replace(/[^\w .:/@+-]/g, '').slice(0, 120);
}
