import type { GameEventType, GameServerAction } from './protocol.js';

export type ActionCoverageStatus = 'live-supported' | 'schema-fallback' | 'unsupported';
export type EventCoverageStatus = 'live-supported' | 'unsupported';

export interface ActionCoverage {
  status: ActionCoverageStatus;
  responseShape: string;
  liveVerification: string;
  reason: string;
}

export interface EventCoverage {
  status: EventCoverageStatus;
  payloadShape: string;
  liveVerification: string;
  reason: string;
}

export const ACTION_COVERAGE: Record<GameServerAction, ActionCoverage> = {
  getPlayer: {
    status: 'live-supported',
    responseShape: 'public Takaro player DTO or null',
    liveVerification: 'MCP gameserverGetPlayers, then gameserverGetPlayer with a returned gameId',
    reason: 'Conan listplayers exposes enough identity data for online players.',
  },
  getPlayers: {
    status: 'live-supported',
    responseShape: 'public Takaro player DTO array',
    liveVerification: 'MCP gameserverGetPlayers',
    reason: 'Conan listplayers is available through RCON and has live output parsing.',
  },
  getPlayerLocation: {
    status: 'live-supported',
    responseShape: '{ x: number, y: number, z: number, dimension?: string }',
    liveVerification: 'MCP player location actions read actor_position from configured Conan game_0.db',
    reason: 'Conan persists character coordinates in the save database actor_position table.',
  },
  testReachability: {
    status: 'live-supported',
    responseShape: '{ connectable: boolean, reason: string | null }',
    liveVerification: 'MCP gameserverTestReachability or RCON help',
    reason: 'A simple RCON help command verifies server reachability without mutating game state.',
  },
  executeConsoleCommand: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'MCP gameserverExecuteCommand with help',
    reason: 'The bridge forwards explicit admin console commands to Conan RCON.',
  },
  listBans: {
    status: 'live-supported',
    responseShape: 'Takaro ban DTO array',
    liveVerification: 'MCP gameserverListBans',
    reason: 'Conan listbans is available through RCON and empty-server output was live validated.',
  },
  listItems: {
    status: 'live-supported',
    responseShape: 'discovered Takaro item DTO array',
    liveVerification: 'MCP list item actions read the configured item catalog and distinct template_id values from Conan game_0.db item_inventory',
    reason: 'Conan persists seen item template IDs in the save database item_inventory table; an optional item catalog adds readable names and aliases.',
  },
  listEntities: {
    status: 'live-supported',
    responseShape: 'discovered Takaro entity DTO array',
    liveVerification: 'MCP list entity actions read distinct actor classes from configured Conan game_0.db actor_position',
    reason: 'Conan persists actor classes in the save database actor_position table.',
  },
  listLocations: {
    status: 'live-supported',
    responseShape: 'saved player location DTO array',
    liveVerification: 'MCP list location actions read character positions from configured Conan game_0.db',
    reason: 'Conan persists player locations in the save database actor_position table.',
  },
  getPlayerInventory: {
    status: 'live-supported',
    responseShape: 'Takaro item DTO array with Conan template IDs',
    liveVerification: 'MCP get inventory actions read item_inventory rows from configured Conan game_0.db',
    reason: 'Conan persists character inventory rows in the save database item_inventory table.',
  },
  getMapInfo: {
    status: 'schema-fallback',
    responseShape: '{ enabled: false, mapBlockSize: 0, maxZoom: 0, mapSizeX: 0, mapSizeY: 0, mapSizeZ: 0 }',
    liveVerification: 'MCP gameserverGetMapInfo returns disabled map info; Conan map metadata is not exposed',
    reason: 'Map metadata is not available with the current Conan/Pippi runtime, but Takaro validates this action against MapInfoDTO.',
  },
  getMapTile: {
    status: 'unsupported',
    responseShape: '{ success: false, error: string }',
    liveVerification: 'MCP gameserverGetMapTile returns structured unsupported error',
    reason: 'Map tile rendering is not available with the current Conan/Pippi runtime.',
  },
  giveItem: {
    status: 'live-supported',
    responseShape: '{ success: true, rawResult: string }',
    liveVerification: 'MCP gameserverGiveItem routes to Conan con <online player> SpawnItem <item> <amount>',
    reason: 'Conan RCON exposes con <id> <client command>; online players accept SpawnItem through that relay.',
  },
  sendMessage: {
    status: 'live-supported',
    responseShape: 'chat bridge renderer result',
    liveVerification: 'MCP gameserverSendMessage reaches Enhanced Pippi server/directmessage through the helper',
    reason: 'The optional helper polls /mod/poll and renders normal in-game chat through Enhanced Pippi.',
  },
  teleportPlayer: {
    status: 'live-supported',
    responseShape: '{ success: true, rawResult: string }',
    liveVerification: 'MCP gameserverTeleportPlayer routes to Conan con <online player> TeleportPlayer <x> <y> <z>',
    reason: 'Conan RCON exposes con <id> <client command>; online players accept TeleportPlayer through that relay.',
  },
  kickPlayer: {
    status: 'live-supported',
    responseShape: '{ success: true, rawResult: string }',
    liveVerification: 'MCP gameserverKickPlayer routes to Conan kickplayer after explicit live-test approval',
    reason: 'Conan RCON exposes kickplayer.',
  },
  banPlayer: {
    status: 'live-supported',
    responseShape: '{ success: true, rawResult: string }',
    liveVerification: 'RCON help banplayer; avoid live mutation without approval',
    reason: 'Conan RCON exposes banplayer.',
  },
  unbanPlayer: {
    status: 'live-supported',
    responseShape: '{ success: true, rawResult: string }',
    liveVerification: 'RCON help unbanplayer; use only against a known test ban',
    reason: 'Conan RCON exposes unbanplayer.',
  },
  shutdown: {
    status: 'live-supported',
    responseShape: '{ success: true, rawResult: string }',
    liveVerification: 'RCON help shutdown; avoid live shutdown without approval',
    reason: 'Conan RCON exposes shutdown.',
  },
};

export const EVENT_COVERAGE: Record<GameEventType, EventCoverage> = {
  'player-connected': {
    status: 'live-supported',
    payloadShape: '{ player: public Takaro player DTO }',
    liveVerification: 'Bridge emits startup online players and listplayers deltas after Takaro identification',
    reason: 'Player polling derives connect events from RCON listplayers.',
  },
  'player-disconnected': {
    status: 'live-supported',
    payloadShape: '{ player: public Takaro player DTO }',
    liveVerification: 'Bridge emits listplayers disconnect deltas after Takaro identification',
    reason: 'Player polling derives disconnect events from RCON listplayers.',
  },
  'chat-message': {
    status: 'live-supported',
    payloadShape: 'Takaro chat-message payload with player identity when resolvable',
    liveVerification: 'Live Pippi ChatWindow log advances Takaro chat-message analytics with no validation errors',
    reason: 'Enhanced Pippi ChatWindow logs plus listplayers enrichment provide inbound chat identity.',
  },
  'player-death': {
    status: 'live-supported',
    payloadShape: '{ player: public Takaro player DTO when resolvable, timestamp: string, msg?: string, attacker?: public Takaro player DTO }',
    liveVerification: 'Configured Conan logs contain KillCharacterWithRagdoll_Implementation player death lines parsed by LogTailer',
    reason: 'Conan writes KillCharacterWithRagdoll_Implementation lines for player deaths; listplayers enrichment resolves online player identity when possible.',
  },
  'entity-killed': {
    status: 'live-supported',
    payloadShape: '{ player: public Takaro player DTO when resolvable, entity: string, weapon: string, timestamp: string, msg?: string }',
    liveVerification: 'Configured Conan logs contain player-attributed KillCharacterWithRagdoll_Implementation non-player death lines parsed by LogTailer',
    reason: 'Conan writes KillCharacterWithRagdoll_Implementation lines for non-player deaths; player-attributed lines map to Takaro entity-killed events.',
  },
  log: {
    status: 'live-supported',
    payloadShape: '{ message: string, level?: string, timestamp?: string }',
    liveVerification: 'Configured Conan log files emit log events through LogTailer',
    reason: 'The sidecar tails configured Conan logs and forwards raw lines.',
  },
};

export function getActionCoverage(action: GameServerAction): ActionCoverage {
  return ACTION_COVERAGE[action];
}

export function getEventCoverage(eventType: GameEventType): EventCoverage {
  return EVENT_COVERAGE[eventType];
}

export function schemaFallbackForAction(action: GameServerAction): unknown | undefined {
  const coverage = ACTION_COVERAGE[action];
  if (coverage.status !== 'schema-fallback') return undefined;
  if (action === 'getMapInfo') {
    return {
      enabled: false,
      mapBlockSize: 0,
      maxZoom: 0,
      mapSizeX: 0,
      mapSizeY: 0,
      mapSizeZ: 0,
    };
  }
  return action === 'getPlayerLocation' ? { x: 0, y: 0, z: 0 } : [];
}

export function unsupportedActionError(action: GameServerAction): { success: false; error: string } | undefined {
  const coverage = ACTION_COVERAGE[action];
  if (coverage.status !== 'unsupported') return undefined;
  return {
    success: false,
    error: `Action ${action} is not supported by the Conan Exiles RCON sidecar: ${coverage.reason}`,
  };
}
