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
    responseShape: 'Takaro player DTO or null',
    liveVerification: 'fake TShock seeded player plus MCP gameserverGetPlayer when a real client is connected',
    reason: 'TShock /v2/server/status and /v2/players/list expose online player names and metadata.',
  },
  getPlayers: {
    status: 'live-supported',
    responseShape: 'Takaro player DTO array',
    liveVerification: 'fake TShock seeded player and real TShock empty-player smoke',
    reason: 'TShock REST exposes online players.',
  },
  getPlayerLocation: {
    status: 'live-supported',
    responseShape: '{ x: number, y: number, z: number }',
    liveVerification: 'server-side TakaroTerrariaEvents /takaropos plugin command with connected player',
    reason: 'TShock REST does not expose coordinates directly, so the optional server-side plugin reads TSPlayer.TPlayer.position.',
  },
  testReachability: {
    status: 'live-supported',
    responseShape: '{ connectable: boolean, reason: string | null }',
    liveVerification: 'TShock tokentest and /v2/server/status',
    reason: 'A token test plus status call verifies the REST bridge without mutating game state.',
  },
  executeConsoleCommand: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'fake Takaro all-action harness and safe real rawcmd such as help',
    reason: 'TShock exposes raw command execution through REST; this sidecar gates it with an allowlist.',
  },
  listBans: {
    status: 'live-supported',
    responseShape: 'Takaro ban DTO array',
    liveVerification: 'fake TShock seeded bans and real TShock empty list smoke',
    reason: 'TShock exposes /v2/bans/list.',
  },
  listItems: {
    status: 'live-supported',
    responseShape: 'Takaro item DTO array',
    liveVerification: 'built-in catalog extracted from the local TShock/Terraria server assemblies',
    reason: 'The sidecar ships a server-version item catalog and TShock /give accepts the numeric item code.',
  },
  listEntities: {
    status: 'schema-fallback',
    responseShape: '[]',
    liveVerification: 'fake harness returns []',
    reason: 'The REST API does not expose an entity catalog endpoint.',
  },
  listLocations: {
    status: 'schema-fallback',
    responseShape: '[]',
    liveVerification: 'fake harness returns []',
    reason: 'The REST API does not expose saved Takaro-style locations.',
  },
  getPlayerInventory: {
    status: 'schema-fallback',
    responseShape: '[]',
    liveVerification: 'fake harness returns []',
    reason: 'TShock REST does not expose a player inventory DTO without a custom plugin.',
  },
  getMapInfo: {
    status: 'schema-fallback',
    responseShape: '{ enabled: false, mapBlockSize: 0, maxZoom: 0, mapSizeX: 0, mapSizeY: 0, mapSizeZ: 0 }',
    liveVerification: 'MCP gameserverGetMapInfo returns a disabled map DTO',
    reason: 'TShock REST does not expose map metadata, but Takaro validates this action against a map DTO.',
  },
  getMapTile: {
    status: 'unsupported',
    responseShape: '{ success: false, error: string }',
    liveVerification: 'fake harness returns explicit unsupported response',
    reason: 'TShock REST does not render map tiles.',
  },
  giveItem: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'fake TShock rawcmd; real smoke should use a disposable connected test player',
    reason: 'TShock console commands support item giving to a known player.',
  },
  sendMessage: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'fake and real /v2/server/broadcast smoke',
    reason: 'TShock exposes a broadcast REST endpoint.',
  },
  teleportPlayer: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'server-side TakaroTerrariaEvents /takarotp plugin command; real smoke should use a disposable connected test player',
    reason: 'TShock REST raw commands cannot coordinate-teleport another player directly, so the optional server-side plugin owns this action.',
  },
  kickPlayer: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'fake TShock rawcmd; live mutation only against a disposable profile',
    reason: 'TShock console commands support kicking players.',
  },
  banPlayer: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'fake TShock /bans/create plus cleanup',
    reason: 'TShock exposes ban creation through REST.',
  },
  unbanPlayer: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'fake TShock /v2/bans/destroy plus cleanup',
    reason: 'TShock exposes ban deletion through REST.',
  },
  shutdown: {
    status: 'live-supported',
    responseShape: '{ success: boolean, rawResult: string }',
    liveVerification: 'guarded final smoke only when enableShutdown=true',
    reason: 'TShock exposes /v2/server/off, but this sidecar refuses shutdown unless explicitly enabled.',
  },
};

export const EVENT_COVERAGE: Record<GameEventType, EventCoverage> = {
  'player-connected': {
    status: 'live-supported',
    payloadShape: '{ player: Takaro player DTO }',
    liveVerification: 'poll-derived connect event from fake TShock snapshots',
    reason: 'The sidecar compares TShock player snapshots.',
  },
  'player-disconnected': {
    status: 'live-supported',
    payloadShape: '{ player: Takaro player DTO }',
    liveVerification: 'poll-derived disconnect event from fake TShock snapshots',
    reason: 'The sidecar compares TShock player snapshots.',
  },
  'chat-message': {
    status: 'live-supported',
    payloadShape: '{ player: Takaro player DTO, message: string }',
    liveVerification: 'log parser fixture and tailer test',
    reason: 'TShock text logs include chat lines in common deployments.',
  },
  'player-death': {
    status: 'live-supported',
    payloadShape: '{ player: Takaro player DTO, msg?: string, attacker?: Takaro player DTO, position?: IPosition }',
    liveVerification: 'TShock plugin emits TAKARO_EVENT player-death marker parsed by log tailer',
    reason: 'The optional Takaro Terraria Events TShock plugin hooks GetDataHandlers.KillMe and emits structured log markers.',
  },
  'entity-killed': {
    status: 'live-supported',
    payloadShape: '{ player?: Takaro player DTO, entity: string, weapon: string }',
    liveVerification: 'TShock plugin emits TAKARO_EVENT entity-killed marker parsed by log tailer',
    reason: 'The optional Takaro Terraria Events TShock plugin hooks ServerApi.Hooks.NpcKilled and emits structured log markers.',
  },
  log: {
    status: 'live-supported',
    payloadShape: '{ message: string, level?: string, timestamp?: string }',
    liveVerification: 'log tailer fixture',
    reason: 'Configured TShock log files can be tailed by the sidecar.',
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
  return [];
}

export function unsupportedActionError(action: GameServerAction): { success: false; error: string } | undefined {
  const coverage = ACTION_COVERAGE[action];
  if (coverage.status !== 'unsupported') return undefined;
  return {
    success: false,
    error: `Action ${action} is not supported by the Terraria TShock sidecar: ${coverage.reason}`,
  };
}
