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
  'giveItem',
  'sendMessage',
  'teleportPlayer',
  'kickPlayer',
  'banPlayer',
  'unbanPlayer',
  'shutdown',
] as const satisfies readonly GameServerAction[];

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
    | 'pong';
  payload?: unknown;
  requestId?: string;
}

export interface IdentifyPayload {
  identityToken: string;
  registrationToken: string;
  name: string;
}

export interface RequestPayload {
  action: GameServerAction;
  args?: unknown;
}

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
