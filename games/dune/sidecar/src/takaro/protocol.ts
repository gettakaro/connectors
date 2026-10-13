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
  type:
    | 'identify'
    | 'identifyResponse'
    | 'connected'
    | 'gameEvent'
    | 'request'
    | 'response'
    | 'error'
    | 'ping'
    | 'pong'
    | 'capabilities'
    | 'capabilitiesResponse'
    | 'eventAck'
    | 'eventError';
  requestId?: string;
  payload?: unknown;
  /** A string on version 0; `{ code, message }` on version 1. */
  error?: string | ProtocolError;
}

/** The version 1 response error codes. */
export type ProtocolErrorCode = 'unsupported' | 'invalid_args' | 'not_found' | 'game_error' | 'internal';

export interface ProtocolError {
  code: ProtocolErrorCode;
  message: string;
}

export interface IdentifyConfig {
  identityToken: string;
  registrationToken: string;
  serverName?: string;
}

/** What a version 1 identify adds: the offered versions and the Capability Manifest. */
export interface IdentifyV1 {
  protocolVersions: number[];
  capabilities: unknown[];
}

export interface TakaroRequest {
  requestId: string;
  action: string;
  args: Record<string, unknown>;
}

export function createIdentify(config: IdentifyConfig, v1?: IdentifyV1): WsMessage {
  const payload: Record<string, unknown> = { identityToken: config.identityToken };
  if (config.registrationToken) payload.registrationToken = config.registrationToken;
  if (config.serverName) payload.name = config.serverName;
  if (v1) {
    payload.protocolVersions = v1.protocolVersions;
    payload.capabilities = v1.capabilities;
  }
  return { type: 'identify', payload };
}

/**
 * Success response. `null` is preserved: Takaro's `getPlayer` for an unknown/offline player expects a JSON `null`
 * (like the proven Zomboid/Minecraft connectors send), and an empty object fails IGamePlayer validation with
 * "property gameId has failed the following constraints: isString" (finding F7).
 */
export function createResponse(requestId: string, payload: unknown): WsMessage {
  return { type: 'response', requestId, payload: payload === undefined ? {} : payload };
}

/**
 * Error response: same frame type as a success. Version 0 takes a top-level error string (proven on the PZ
 * connector); version 1 takes `{ code, message }`. The client converts between the two for the negotiated version.
 */
export function createErrorResponse(requestId: string, error: string | ProtocolError): WsMessage {
  return { type: 'response', requestId, error };
}

/** The error frame for the negotiated version: the plain message on version 0, `{ code, message }` on version 1. */
export function errorForVersion(error: string | ProtocolError, version: number): string | ProtocolError {
  if (version === 1) return typeof error === 'string' ? { code: 'game_error', message: error } : error;
  return typeof error === 'string' ? error : error.message;
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
