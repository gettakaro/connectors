export type GameServerAction =
  | 'getPlayer'
  | 'getPlayers'
  | 'getPlayerLocation'
  | 'testReachability'
  | 'executeConsoleCommand'
  | 'listBans'
  | 'listItems'
  | 'listEntities'
  | 'listLocations'
  | 'getPlayerInventory'
  | 'getMapInfo'
  | 'getMapTile'
  | 'getMapImage'
  | 'giveItem'
  | 'sendMessage'
  | 'teleportPlayer'
  | 'kickPlayer'
  | 'banPlayer'
  | 'unbanPlayer'
  | 'shutdown';

export const ALL_GAME_SERVER_ACTIONS = [
  'getPlayer',
  'getPlayers',
  'getPlayerLocation',
  'testReachability',
  'executeConsoleCommand',
  'listBans',
  'listItems',
  'listEntities',
  'listLocations',
  'getPlayerInventory',
  'getMapInfo',
  'getMapTile',
  'getMapImage',
  'giveItem',
  'sendMessage',
  'teleportPlayer',
  'kickPlayer',
  'banPlayer',
  'unbanPlayer',
  'shutdown',
] as const satisfies readonly GameServerAction[];

export type GameEventType =
  | 'player-connected'
  | 'player-disconnected'
  | 'chat-message'
  | 'player-death'
  | 'entity-killed'
  | 'log';

export const ALL_GAME_EVENT_TYPES = [
  'player-connected',
  'player-disconnected',
  'chat-message',
  'player-death',
  'entity-killed',
  'log',
] as const satisfies readonly GameEventType[];

export interface WsMessage {
  type:
    | 'identify'
    | 'identifyResponse'
    | 'capabilities'
    | 'capabilitiesResponse'
    | 'eventAck'
    | 'eventError'
    | 'connected'
    | 'gameEvent'
    | 'request'
    | 'response'
    | 'error'
    | 'ping'
    | 'pong';
  payload?: RequestPayload | IdentifyResponsePayload | unknown;
  requestId?: string;
  error?: unknown;
  eventType?: GameEventType;
  data?: unknown;
}

export interface IdentifyPayload {
  identityToken: string;
  registrationToken: string;
  name: string;
  /** Offered protocol versions; absent means a version 0 identify. */
  protocolVersions?: number[];
  capabilities?: unknown[];
}

/** The version 1 response error codes. */
export type ProtocolErrorCode = 'unsupported' | 'invalid_args' | 'not_found' | 'game_error' | 'internal';

export interface IdentifyResponsePayload {
  gameServerId?: string;
  protocolVersion?: number;
  error?: unknown;
}

export interface RequestPayload {
  action: GameServerAction;
  args?: unknown;
}

export interface GameEvent {
  type: GameEventType;
  data: unknown;
}
