import type { TShockConfig } from '../config.js';

export interface TShockStatus {
  name?: string;
  port?: number;
  playercount?: number;
  players?: unknown;
}

export interface TShockPlayer {
  gameId: string;
  name: string;
  platformId: string;
  ip?: string;
  group?: string;
  active?: boolean;
  state?: number;
  team?: number;
}

export interface TShockBan {
  ticket_number?: number;
  identifier?: string;
  name?: string;
  ip?: string;
  reason?: string;
}

export interface CommandResult {
  success: boolean;
  rawResult: string;
}

export class TShockClient {
  private cachedToken: string | null;

  constructor(private readonly config: TShockConfig) {
    this.cachedToken = config.token || null;
  }

  async getToken(): Promise<string> {
    if (this.cachedToken) return this.cachedToken;
    if (!this.config.username || !this.config.password) throw new Error('No TShock credentials configured');
    const data = await this.get('/v2/token/create', {
      username: this.config.username,
      password: this.config.password,
    }, false);
    const token = stringValue(data.token) || stringValue(data.response);
    if (!token) throw new Error(`TShock token creation did not return a token: ${JSON.stringify(data)}`);
    this.cachedToken = token;
    return token;
  }

  async testToken(): Promise<CommandResult> {
    try {
      const data = await this.get('/tokentest', { token: await this.getToken() }, false);
      return { success: statusOk(data), rawResult: responseText(data) };
    } catch (err) {
      return { success: false, rawResult: errorMessage(err) };
    }
  }

  async status(): Promise<TShockStatus> {
    return await this.get('/v2/server/status', { players: 'true', rules: 'true' }, false) as TShockStatus;
  }

  async players(): Promise<TShockPlayer[]> {
    const data = await this.status();
    return normalizePlayers(data.players);
  }

  async broadcast(message: string): Promise<CommandResult> {
    const data = await this.get('/v2/server/broadcast', { token: await this.getToken(), msg: message }, true);
    return { success: statusOk(data), rawResult: responseText(data) };
  }

  async rawCommand(command: string): Promise<CommandResult> {
    const params = { token: await this.getToken(), cmd: command };
    const data = await this.getWithFallback(['/v2/server/rawcmd', '/v3/server/rawcmd'], params, true);
    const rawResult = responseText(data);
    return { success: statusOk(data) && !isInvalidCommand(rawResult), rawResult };
  }

  async createBan(input: TShockBan): Promise<CommandResult> {
    const data = await this.get('/bans/create', {
      token: await this.getToken(),
      identifier: input.identifier || input.name || input.ip || '',
      reason: input.reason || '',
    }, true);
    return { success: statusOk(data), rawResult: responseText(data) };
  }

  async destroyBan(input: { user: string; type?: 'user' | 'ip' }): Promise<CommandResult> {
    const ticketNumber = numberString(input.user) || await this.findBanTicketNumber(input.user);
    if (!ticketNumber) return { success: false, rawResult: `No TShock ban found for ${input.user}` };

    const data = await this.get('/v2/bans/destroy', {
      token: await this.getToken(),
      ticketNumber,
    }, true);
    return { success: statusOk(data), rawResult: responseText(data) };
  }

  async listBans(): Promise<TShockBan[]> {
    const data = await this.get('/v2/bans/list', { token: await this.getToken() }, false);
    const bans = Array.isArray(data.bans) ? data.bans : [];
    return bans.map((ban) => typeof ban === 'object' && ban !== null ? ban as TShockBan : { name: String(ban) });
  }

  async shutdown(save = true): Promise<CommandResult> {
    const data = await this.get('/v2/server/off', {
      token: await this.getToken(),
      confirm: 'true',
      nosave: save ? 'false' : 'true',
    }, true);
    return { success: statusOk(data), rawResult: responseText(data) };
  }

  private async findBanTicketNumber(identifier: string): Promise<string | null> {
    const normalized = identifier.toLowerCase();
    const bans = await this.listBans();
    const match = bans.find((ban) => [ban.identifier, ban.name, ban.ip]
      .filter((value): value is string => typeof value === 'string' && value.trim().length > 0)
      .map((value) => value.toLowerCase())
      .includes(normalized));
    return match?.ticket_number ? String(match.ticket_number) : null;
  }

  private async get(pathname: string, params: Record<string, string | number | boolean>, mutating: boolean): Promise<Record<string, unknown>> {
    const url = new URL(pathname, `${this.config.baseUrl}/`);
    for (const [key, value] of Object.entries(params)) {
      if (value !== '') url.searchParams.set(key, String(value));
    }

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.config.timeoutMs);
    try {
      const response = await fetch(url, { method: 'GET', signal: controller.signal });
      const data = await response.json() as Record<string, unknown>;
      if (!response.ok || (!statusOk(data) && mutating)) {
        throw new Error(`TShock ${pathname} failed: ${response.status} ${JSON.stringify(data)}`);
      }
      return data;
    } finally {
      clearTimeout(timer);
    }
  }

  private async getWithFallback(pathnames: string[], params: Record<string, string | number | boolean>, mutating: boolean): Promise<Record<string, unknown>> {
    let lastError: unknown;
    for (const pathname of pathnames) {
      try {
        return await this.get(pathname, params, mutating);
      } catch (err) {
        lastError = err;
        if (!(err instanceof Error) || !err.message.includes(' 404 ')) throw err;
      }
    }
    throw lastError;
  }
}

function normalizePlayers(players: unknown): TShockPlayer[] {
  if (Array.isArray(players)) return players.map(normalizePlayer).filter((player): player is TShockPlayer => player !== null);
  if (typeof players === 'string') {
    return players.split(',')
      .map((name) => name.trim())
      .filter(Boolean)
      .map((name) => ({
        gameId: name,
        name,
        platformId: `terraria:${name}`,
      }));
  }
  return [];
}

function normalizePlayer(raw: unknown): TShockPlayer | null {
  if (!raw || typeof raw !== 'object') return null;
  const record = raw as Record<string, unknown>;
  const name = stringValue(record.nickname) || stringValue(record.name) || stringValue(record.username);
  if (!name) return null;
  const gameId = stringValue(record.username) || stringValue(record.account) || name;
  return {
    gameId,
    name,
    platformId: `terraria:${gameId}`,
    ip: stringValue(record.ip) || undefined,
    group: stringValue(record.group) || undefined,
    active: booleanValue(record.active),
    state: numberValue(record.state),
    team: numberValue(record.team),
  };
}

function statusOk(data: Record<string, unknown>): boolean {
  return data.status === undefined || data.status === '200' || data.status === 200 || data.success === true;
}

function responseText(data: Record<string, unknown>): string {
  return textValue(data.response) || textValue(data.rawResult) || JSON.stringify(data);
}

function textValue(value: unknown): string | null {
  if (Array.isArray(value)) return value.map((entry) => String(entry)).join('\n');
  return stringValue(value);
}

function stringValue(value: unknown): string | null {
  return typeof value === 'string' && value.trim() ? value.trim() : null;
}

function numberString(value: string): string | null {
  return /^\d+$/.test(value.trim()) ? value.trim() : null;
}

function isInvalidCommand(rawResult: string): boolean {
  return rawResult.toLowerCase().includes('invalid command entered');
}

function numberValue(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function booleanValue(value: unknown): boolean | undefined {
  return typeof value === 'boolean' ? value : undefined;
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}
