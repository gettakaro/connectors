export interface NativePlayer { steamId: string; name: string; [key: string]: unknown }
export interface NativeHealth { status: string; bootId: string; [key: string]: unknown }
export interface NativePosition { x: number; y: number; z: number }
export interface NativeItem { code: string; name: string; amount?: number; description?: string; quality?: string }
export interface NativeItemPage { items: NativeItem[]; offset: number; nextOffset: number; total: number; complete: boolean }
export interface NativeEntity { code: string; name: string }
export interface NativeEntityPage { items: NativeEntity[]; offset: number; nextOffset: number; total: number; complete: boolean }
export interface NativeLocation { code: string; name: string; position: NativePosition; sizeX: number; sizeY: number; sizeZ: number }
export interface NativeEvent {
  seq: number;
  type: 'log' | 'chat-message' | 'player-connected' | 'player-disconnected' | 'player-death' | 'entity-killed';
  ts: string | number;
  data: { player?: NativePlayer; msg?: string; channel?: string; killer?: NativePlayer; entity?: string | { code?: string; name?: string }; weapon?: string };
}
export interface NativeEvents {
  bootId: string;
  events: NativeEvent[];
  seq: number;
  latestSeq: number;
  hasMore: boolean;
  truncated: boolean;
}

export class NativeHttpError extends Error {
  constructor(readonly status: number, readonly route: string) {
    super(`ARK native endpoint ${route} returned HTTP ${status}`);
  }
}

export class NativeClient {
  constructor(
    private readonly baseUrl: string,
    private readonly token: string,
    private readonly timeoutMs = 10000,
    private readonly fetchImpl: typeof fetch = fetch,
  ) {}

  health(requestId?: string): Promise<NativeHealth> { return this.request('GET', '/health', undefined, requestId); }
  players(requestId?: string): Promise<NativePlayer[]> { return this.request('GET', '/players', undefined, requestId); }
  player(steamId: string, requestId?: string): Promise<NativePlayer | null> {
    return this.request('GET', `/players/${encodeURIComponent(steamId)}`, undefined, requestId);
  }
  playerLocation(steamId: string, requestId?: string): Promise<NativePosition> {
    return this.request('GET', `/players/${encodeURIComponent(steamId)}/location`, undefined, requestId);
  }
  playerInventory(steamId: string, requestId?: string): Promise<NativeItem[]> {
    return this.request('GET', `/players/${encodeURIComponent(steamId)}/inventory`, undefined, requestId);
  }
  giveItem(steamId: string, value: { code: string; amount: number; quality: number; blueprint: boolean }, requestId?: string): Promise<{ success: boolean }> {
    return this.request('POST', `/players/${encodeURIComponent(steamId)}/give-item`, JSON.stringify(value), requestId, 'application/json');
  }
  teleport(steamId: string, value: NativePosition, requestId?: string): Promise<{ success: boolean }> {
    return this.request('POST', `/players/${encodeURIComponent(steamId)}/teleport`, JSON.stringify(value), requestId, 'application/json');
  }
  itemPage(offset: number, limit: number, requestId?: string): Promise<NativeItemPage> {
    return this.request('GET', `/items?offset=${offset}&limit=${limit}`, undefined, requestId);
  }
  entityPage(offset: number, limit: number, requestId?: string): Promise<NativeEntityPage> {
    return this.request('GET', `/entities?offset=${offset}&limit=${limit}`, undefined, requestId);
  }
  locations(requestId?: string): Promise<NativeLocation[]> {
    return this.request('GET', '/locations', undefined, requestId);
  }
  moderate(steamId: string, action: 'kick' | 'ban' | 'unban', requestId?: string): Promise<{ success: boolean; errorMessage?: string }> {
    return this.request('POST', `/players/${encodeURIComponent(steamId)}/${action}`, '', requestId,
      'text/plain; charset=utf-8', 503);
  }
  bans(requestId?: string): Promise<string[]> {
    return this.request('GET', '/bans', undefined, requestId);
  }
  events(since: number): Promise<NativeEvents> {
    return this.request('GET', `/events?since=${encodeURIComponent(String(since))}`);
  }
  message(text: string, requestId?: string): Promise<{ success: boolean }> {
    return this.request('POST', '/message', text, requestId);
  }
  messageTo(steamId: string, text: string, requestId?: string): Promise<{ success: boolean }> {
    return this.request('POST', `/players/${encodeURIComponent(steamId)}/message`, text, requestId);
  }
  console(command: string, requestId?: string): Promise<{ success: boolean; rawResult: string; errorMessage: string | null }> {
    // A native 503 carries a valid CommandOutput failure, not a transport ack.
    return this.request('POST', '/console', command, requestId, 'text/plain; charset=utf-8', 503, true);
  }
  shutdown(requestId?: string): Promise<{ success: boolean }> {
    return this.request('POST', '/shutdown', '', requestId);
  }

  private async request<T>(method: 'GET' | 'POST', route: string, text?: string, requestId?: string, contentType = 'text/plain; charset=utf-8', acceptedFailureStatus?: number, strictConsoleStatus = false): Promise<T> {
    const signal = AbortSignal.timeout(this.timeoutMs);
    const traced = requestId !== undefined;
    const trace = (status: number | null, itemCount?: number): void => {
      if (!traced) return;
      logger.info(JSON.stringify({ event: 'native-http', utc: new Date().toISOString(),
        monotonicNs: process.hrtime.bigint().toString(), requestId: requestId!.slice(0, 128),
        method, path: route.split('?')[0], status, ...(itemCount === undefined ? {} : { itemCount }) }));
    };
    let status: number | null = null;
    try {
      const response = await this.fetchImpl(`${this.baseUrl}${route}`, {
        method,
        headers: {
          Authorization: `Bearer ${this.token}`,
          ...(text === undefined ? {} : { 'Content-Type': contentType }),
        },
        body: text,
        signal,
      });
      status = response.status;
      if (!response.ok && status !== acceptedFailureStatus) throw new NativeHttpError(status, route.split('?')[0]);
      const payload = (await response.json()) as T;
      if (strictConsoleStatus) {
        const success = (payload as { success?: unknown } | null)?.success;
        if (typeof success !== 'boolean' || (status !== 200 && status !== 503) ||
            (status === 200) !== success) {
          throw new Error('Native console status does not match its acknowledgment');
        }
      }
      const page = payload as { items?: unknown };
      trace(status, route === '/locations' && Array.isArray(payload) ? payload.length :
        route.endsWith('/inventory') && Array.isArray(payload) ? payload.length :
        route.startsWith('/items?') && Array.isArray(page?.items) ? page.items.length : undefined);
      return payload;
    } catch (error) {
      trace(status);
      throw error;
    }
  }
}
import { logger } from '../logger.js';
