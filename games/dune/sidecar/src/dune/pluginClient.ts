import type { PluginEventsResponse, PluginHealth, PluginLocation, PluginPlayersResponse } from './types.js';

export type FetchImpl = (input: string | URL, init?: RequestInit) => Promise<Response>;

export class PluginHttpError extends Error {
  constructor(
    readonly status: number,
    readonly path: string,
    readonly body: string,
  ) {
    super(`Dune plugin HTTP ${status} for ${path}${body ? `: ${truncate(body)}` : ''}`);
  }
}

export class PluginUnreachableError extends Error {}

export interface PluginClientOptions {
  baseUrl: string;
  token: string;
  timeoutMs?: number;
  fetchImpl?: FetchImpl;
}

/**
 * The optional native plugin (`libtakaro-dune.so`).
 *
 * Everything the sidecar needs for the 17 actions and 4 of the 6 events comes out of Postgres and RabbitMQ; the plugin
 * only fills the gaps that no out-of-process source has: entity-killed, death attribution (killer/weapon), a LIVE
 * player position, and precise connect/disconnect. When it is absent — which is the supported configuration on a
 * stock Funcom install — every one of those degrades explicitly rather than failing: see
 * `adapter.capabilities()` and the `/health` body.
 */
export class DunePluginClient {
  private readonly baseUrl: string;
  private readonly token: string;
  private readonly timeoutMs: number;
  private readonly fetchImpl: FetchImpl;

  constructor(options: PluginClientOptions) {
    this.baseUrl = (options.baseUrl ?? '').replace(/\/+$/, '');
    this.token = options.token ?? '';
    this.timeoutMs = options.timeoutMs ?? 10000;
    this.fetchImpl = options.fetchImpl ?? fetch;
  }

  /** False when no `DUNE_PLUGIN_URL` is configured: the connector then never even tries. */
  enabled(): boolean {
    return Boolean(this.baseUrl);
  }

  health(): Promise<PluginHealth> {
    return this.request('GET', '/health');
  }

  /**
   * LIVE pawn location — the only source that tracks a moving player.
   *
   * `ref` is the PLUGIN's handle, not the FLS id: the plugin has never seen an FLS id. Callers get
   * the ref from `PluginJoin.refFor(flsId)`; passing an FLS id straight through is a guaranteed 404,
   * which is why `adapter.getPlayerLocation` resolves it first and falls back rather than retrying.
   * The answer always carries `source`, so a caller can say which of the three chain steps answered.
   */
  getPlayerLocation(ref: string): Promise<PluginLocation> {
    return this.request('GET', `/players/${encodeURIComponent(ref)}/location`);
  }

  /** Every player the plugin has seen through PostLogin, with the Postgres row ids to join on. */
  getPlayers(): Promise<PluginPlayersResponse> {
    return this.request('GET', '/players');
  }

  getEvents(since: number, limit?: number): Promise<PluginEventsResponse> {
    const q = `since=${encodeURIComponent(String(since))}${limit ? `&limit=${encodeURIComponent(String(limit))}` : ''}`;
    return this.request('GET', `/events?${q}`);
  }

  /**
   * The live bestiary. The plugin answers an OBJECT (`{liveActors, entitiesReturned, entities:[…]}`), not a bare
   * array — returning it unwrapped made every caller's `Array.isArray` check fail and silently drop all 90 rows.
   */
  async getEntities(timeoutMs?: number): Promise<unknown[]> {
    const body = await this.request<unknown>('GET', '/entities', undefined, timeoutMs);
    if (Array.isArray(body)) return body;
    const rows = (body as { entities?: unknown })?.entities;
    return Array.isArray(rows) ? rows : [];
  }

  private async request<T>(method: 'GET' | 'POST', path: string, body?: unknown, timeoutMs = this.timeoutMs): Promise<T> {
    if (!this.baseUrl) throw new PluginUnreachableError('No Dune plugin configured (DUNE_PLUGIN_URL is empty)');
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    let response: Response;
    try {
      response = await this.fetchImpl(`${this.baseUrl}${path}`, {
        method,
        headers: {
          ...(this.token ? { Authorization: `Bearer ${this.token}` } : {}),
          ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}),
        },
        body: body !== undefined ? JSON.stringify(body) : undefined,
        signal: controller.signal,
      });
    } catch (err) {
      const reason = controller.signal.aborted ? `timed out after ${timeoutMs}ms` : (err as Error).message;
      throw new PluginUnreachableError(`Dune plugin unreachable at ${this.baseUrl}${path}: ${reason}`);
    } finally {
      clearTimeout(timer);
    }
    const raw = await response.text();
    const cleanPath = path.split('?')[0];
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
