import type {
  PluginCommandResult,
  PluginEventsResponse,
  PluginHealth,
  PluginInventoryItem,
  PluginPlayer,
  Position,
} from './types.js';

export type FetchImpl = (input: string | URL, init?: RequestInit) => Promise<Response>;

export class PluginHttpError extends Error {
  constructor(
    readonly status: number,
    readonly path: string,
    readonly body: string,
  ) {
    super(`Enshrouded plugin HTTP ${status} for ${path}${body ? `: ${truncate(body)}` : ''}`);
  }
}

/** Plugin returned 501: the capability exists in the contract but the in-game hook is not implemented. */
export class PluginUnimplementedError extends PluginHttpError {
  constructor(path: string, body: string) {
    super(501, path, body);
    this.message = `Enshrouded plugin has not implemented ${path} (HTTP 501)${body ? `: ${truncate(body)}` : ''}`;
  }
}

export class PluginUnreachableError extends Error {}

export interface PluginClientOptions {
  baseUrl: string;
  token: string;
  timeoutMs?: number;
  fetchImpl?: FetchImpl;
}

export class EnshroudedPluginClient {
  private readonly baseUrl: string;
  private readonly token: string;
  private readonly timeoutMs: number;
  private readonly fetchImpl: FetchImpl;

  constructor(options: PluginClientOptions) {
    this.baseUrl = options.baseUrl.replace(/\/+$/, '');
    this.token = options.token;
    this.timeoutMs = options.timeoutMs ?? 10000;
    this.fetchImpl = options.fetchImpl ?? fetch;
  }

  health(): Promise<PluginHealth> {
    return this.request('GET', '/health');
  }
  getPlayers(): Promise<PluginPlayer[]> {
    return this.request('GET', '/players');
  }
  getPlayer(gameId: string): Promise<PluginPlayer> {
    return this.request('GET', `/players/${encodeURIComponent(gameId)}`);
  }
  getPlayerLocation(gameId: string): Promise<Position> {
    return this.request('GET', `/players/${encodeURIComponent(gameId)}/location`);
  }
  getPlayerInventory(gameId: string): Promise<PluginInventoryItem[]> {
    return this.request('GET', `/players/${encodeURIComponent(gameId)}/inventory`);
  }
  getEvents(since: number): Promise<PluginEventsResponse> {
    return this.request('GET', `/events?since=${encodeURIComponent(String(since))}`);
  }
  sendMessage(text: string, recipientGameId?: string): Promise<unknown> {
    return this.request('POST', '/message', recipientGameId ? { text, recipientGameId } : { text });
  }
  teleport(gameId: string, x: number, y: number, z: number): Promise<unknown> {
    return this.request('POST', '/teleport', { gameId, x, y, z });
  }
  give(gameId: string, code: string, amount: number, quality?: string): Promise<unknown> {
    return this.request('POST', '/give', quality !== undefined ? { gameId, code, amount, quality } : { gameId, code, amount });
  }
  kick(gameId: string, reason?: string): Promise<unknown> {
    return this.request('POST', '/kick', omitUndefined({ gameId, reason }));
  }
  ban(gameId: string, reason?: string, expiresAt?: string): Promise<unknown> {
    return this.request('POST', '/ban', omitUndefined({ gameId, reason, expiresAt }));
  }
  unban(gameId: string): Promise<unknown> {
    return this.request('POST', '/unban', { gameId });
  }
  getBans(): Promise<unknown[]> {
    return this.request('GET', '/bans');
  }
  getItems(): Promise<unknown[]> {
    return this.request('GET', '/items');
  }
  getEntities(): Promise<unknown[]> {
    return this.request('GET', '/entities');
  }
  getLocations(): Promise<unknown[]> {
    return this.request('GET', '/locations');
  }
  command(command: string): Promise<PluginCommandResult> {
    return this.request('POST', '/command', { command });
  }
  shutdown(): Promise<unknown> {
    return this.request('POST', '/shutdown', {});
  }

  private async request<T>(method: 'GET' | 'POST', path: string, body?: unknown): Promise<T> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    let response: Response;
    try {
      response = await this.fetchImpl(`${this.baseUrl}${path}`, {
        method,
        headers: {
          Authorization: `Bearer ${this.token}`,
          ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}),
        },
        body: body !== undefined ? JSON.stringify(body) : undefined,
        signal: controller.signal,
      });
    } catch (err) {
      const reason = controller.signal.aborted ? `timed out after ${this.timeoutMs}ms` : (err as Error).message;
      throw new PluginUnreachableError(`Enshrouded plugin unreachable at ${this.baseUrl}${path}: ${reason}`);
    } finally {
      clearTimeout(timer);
    }
    const raw = await response.text();
    const cleanPath = path.split('?')[0];
    if (response.status === 501) throw new PluginUnimplementedError(cleanPath, errorText(raw));
    if (!response.ok) throw new PluginHttpError(response.status, cleanPath, errorText(raw));
    if (!raw) return {} as T;
    try {
      return JSON.parse(raw) as T;
    } catch {
      throw new PluginHttpError(response.status, cleanPath, `invalid JSON: ${truncate(raw)}`);
    }
  }
}

function errorText(raw: string): string {
  try {
    const parsed = JSON.parse(raw) as { error?: unknown; message?: unknown };
    if (typeof parsed.error === 'string') return parsed.error;
    if (typeof parsed.message === 'string') return parsed.message;
  } catch {
    /* plain text */
  }
  return raw;
}

function truncate(value: string): string {
  return value.length > 300 ? `${value.slice(0, 300)}...` : value;
}

function omitUndefined<T extends Record<string, unknown>>(value: T): T {
  return Object.fromEntries(Object.entries(value).filter(([, v]) => v !== undefined)) as T;
}
