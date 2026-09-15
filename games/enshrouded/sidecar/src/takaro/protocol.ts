export const GAME_SERVER_ACTIONS = [
  'getPlayer',
  'getPlayers',
  'getPlayerLocation',
  'getPlayerInventory',
  'giveItem',
  'listItems',
  'listEntities',
  'listLocations',
  'executeConsoleCommand',
  'sendMessage',
  'teleportPlayer',
  'testReachability',
  'kickPlayer',
  'banPlayer',
  'unbanPlayer',
  'listBans',
  'shutdown',
] as const;

export type GameServerAction = (typeof GAME_SERVER_ACTIONS)[number];

export const GAME_EVENT_TYPES = [
  'log',
  'player-connected',
  'player-disconnected',
  'chat-message',
  'player-death',
  'entity-killed',
] as const;

export type GameEventType = (typeof GAME_EVENT_TYPES)[number];

export function isGameEventType(value: unknown): value is GameEventType {
  return typeof value === 'string' && (GAME_EVENT_TYPES as readonly string[]).includes(value);
}

export interface WsMessage {
  type: 'identify' | 'identifyResponse' | 'connected' | 'gameEvent' | 'request' | 'response' | 'error' | 'ping' | 'pong';
  requestId?: string;
  payload?: unknown;
  error?: string;
}

export interface IdentifyConfig {
  identityToken: string;
  registrationToken: string;
  serverName?: string;
}

export interface TakaroRequest {
  requestId: string;
  action: string;
  args: Record<string, unknown>;
}

export function createIdentify(config: IdentifyConfig): WsMessage {
  const payload: Record<string, unknown> = { identityToken: config.identityToken };
  if (config.registrationToken) payload.registrationToken = config.registrationToken;
  if (config.serverName) payload.name = config.serverName;
  return { type: 'identify', payload };
}

export function createResponse(requestId: string, payload: unknown): WsMessage {
  return { type: 'response', requestId, payload: payload === undefined || payload === null ? {} : payload };
}

/** Error response: same frame type as a success, with a top-level error string (proven on the PZ connector). */
export function createErrorResponse(requestId: string, error: string): WsMessage {
  return { type: 'response', requestId, error };
}

export function createGameEvent(type: GameEventType, data: unknown): WsMessage {
  return { type: 'gameEvent', payload: { type, data } };
}

export function parseTakaroRequest(message: WsMessage): TakaroRequest {
  if (message.type !== 'request') throw new Error('Takaro message is not a request');
  if (!message.requestId) throw new Error('Takaro request missing requestId');
  const payload = asRecord(message.payload);
  const action = typeof payload.action === 'string' ? payload.action : '';
  if (!action) throw new Error('Takaro request missing action');
  return { requestId: message.requestId, action, args: normalizeArgs(payload.args) };
}

/** Takaro sends args as [], {}, a JSON string ("{}", "[]", "{...}"), "" or null. */
export function normalizeArgs(value: unknown): Record<string, unknown> {
  if (value == null || Array.isArray(value)) return {};
  if (typeof value === 'string') {
    const trimmed = value.trim();
    if (!trimmed) return {};
    try {
      return asRecord(JSON.parse(trimmed));
    } catch {
      return {};
    }
  }
  return asRecord(value);
}

export function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value) ? (value as Record<string, unknown>) : {};
}
