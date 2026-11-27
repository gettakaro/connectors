// Mirrors Takaro's connect protocol (see packages/app-connector). Lifted from
// the Enshrouded bridge — same shape works for any game.

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
  | 'giveItem'
  | 'sendMessage'
  | 'teleportPlayer'
  | 'kickPlayer'
  | 'banPlayer'
  | 'unbanPlayer'
  | 'shutdown';

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
  payload?: unknown;
  requestId?: string;
  /** Protocol v1: failed responses and identify/capabilities rejections. */
  error?: unknown;
}

export interface IdentifyPayload {
  identityToken: string;
  registrationToken: string;
  name: string;
  /** Offered protocol versions; absent means a version 0 identify. */
  protocolVersions?: number[];
  /** Protocol v1 Capability Manifest. */
  capabilities?: unknown[];
}

/** Protocol v1 response error codes. */
export type ProtocolErrorCode = 'unsupported' | 'invalid_args' | 'not_found' | 'game_error' | 'internal';

export interface RequestPayload {
  action: GameServerAction;
  args: string;
}

export type GameEventType =
  | 'player-connected'
  | 'player-disconnected'
  | 'chat-message'
  | 'player-death'
  | 'entity-killed'
  | 'log';
