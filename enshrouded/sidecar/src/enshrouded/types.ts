export type CapabilityState = 'ok' | 'degraded' | 'unimplemented';

export interface PluginHealth {
  status: string;
  version?: string;
  gameBuild?: string;
  capabilities?: Record<string, CapabilityState | string>;
}

export interface Position {
  x: number;
  y: number;
  z: number;
  dimension?: string;
}

export interface PluginPlayer {
  gameId: string;
  name: string;
  steamId?: string;
  peerId?: string;
  group?: string;
  online?: boolean;
  position?: Position;
}

export interface PluginInventoryItem {
  code: string;
  name: string;
  amount: number;
  quality?: string | number;
}

export interface PluginEvent {
  seq: number;
  type: string;
  data: unknown;
  ts?: string | number;
}

export interface PluginEventsResponse {
  /** Per-process id of the game server (plugin >= 0.4.1); changes when the server restarts. */
  bootId?: string;
  seq: number;
  events: PluginEvent[];
}

export interface PluginCommandResult {
  success: boolean;
  output?: string;
}

export interface TakaroPlayer {
  gameId: string;
  name: string;
  steamId?: string;
  platformId?: string;
  ip?: string;
  ping?: number;
}

export interface TakaroItem {
  code: string;
  name: string;
  description?: string;
  amount?: number;
  quality?: string;
}

export type TakaroEntityType = 'hostile' | 'friendly' | 'neutral';

export interface TakaroEntity {
  code: string;
  name: string;
  description?: string;
  type: TakaroEntityType;
}

export interface TakaroLocation {
  code: string;
  name: string;
  position: Position;
  radius?: number;
  sizeX?: number;
  sizeY?: number;
  sizeZ?: number;
}

export interface TakaroBan {
  player: TakaroPlayer;
  reason: string;
  expiresAt: string | null;
}
