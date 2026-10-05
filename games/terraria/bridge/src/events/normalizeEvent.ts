import { terrariaIdentity } from '../terraria/identity.js';
import type { GameEvent, GameEventType } from '../takaro/protocol.js';

export function normalizeGameEvent(type: GameEventType, rawData: unknown): GameEvent {
  const data = record(rawData);
  switch (type) {
    case 'log':
      return { type, data: compact({ msg: stringValue(data.message) || stringValue(data.msg) || JSON.stringify(rawData), timestamp: stringValue(data.timestamp) }) };
    case 'chat-message':
      return {
        type,
        data: compact({
          player: playerDto(data.player),
          channel: stringValue(data.channel) || 'global',
          msg: stringValue(data.msg) || stringValue(data.message) || '',
        }),
      };
    case 'player-connected':
    case 'player-disconnected':
      return { type, data: compact({ player: playerDto(data.player) }) };
    case 'player-death':
      return {
        type,
        data: compact({
          player: playerDto(data.player),
          attacker: playerDto(data.attacker),
          position: positionDto(data.position),
          timestamp: stringValue(data.timestamp),
          msg: stringValue(data.msg) || stringValue(data.reason),
        }),
      };
    case 'entity-killed': {
      const entity = record(data.entity);
      return {
        type,
        data: compact({
          player: playerDto(data.player) || playerDto(data.killer),
          entity: stringValue(data.entity) || stringValue(entity.name) || stringValue(entity.gameId),
          weapon: stringValue(data.weapon) || 'unknown',
          timestamp: stringValue(data.timestamp),
        }),
      };
    }
    default:
      return { type, data: rawData };
  }
}

function playerDto(value: unknown): { gameId: string; name?: string; platformId?: string; ip?: string } | undefined {
  const source = record(value);
  const raw = stringValue(source.gameId) || stringValue(source.name) || stringValue(source.platformId);
  if (!raw) return undefined;
  // Every event carries the same gameId/platformId pair as getPlayers. Without a platformId a
  // player-connected for someone Takaro has not seen yet cannot create the player at all, and the
  // events plugin's own platformId (the client UUID) would disagree with the REST player list.
  const key = raw.startsWith('terraria:') ? raw.slice('terraria:'.length) : raw;
  // An NPC killer is reported as `npc:<slot>`: it is no account, so it gets no platformId.
  if (key.startsWith('npc:')) {
    return compact({ gameId: key, name: stringValue(source.name) }) as { gameId: string; name?: string };
  }
  const { gameId, platformId } = terrariaIdentity(key);
  return compact({
    gameId,
    name: stringValue(source.name),
    platformId,
    ip: stringValue(source.ip),
  }) as { gameId: string; name?: string; platformId?: string; ip?: string };
}

function positionDto(value: unknown): { x: number; y: number; z: number; dimension?: string } | undefined {
  const source = record(value);
  const x = numberValue(source.x);
  const y = numberValue(source.y);
  const z = numberValue(source.z) ?? 0;
  if (x === undefined || y === undefined) return undefined;
  return compact({ x, y, z, dimension: stringValue(source.dimension) }) as { x: number; y: number; z: number; dimension?: string };
}

function record(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : {};
}

function stringValue(value: unknown): string | undefined {
  return typeof value === 'string' && value.trim() ? value.trim() : undefined;
}

function numberValue(value: unknown): number | undefined {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && Number.isFinite(Number(value))) return Number(value);
  return undefined;
}

function compact<T extends Record<string, unknown>>(value: T): Partial<T> {
  return Object.fromEntries(Object.entries(value).filter(([, entry]) => entry !== undefined && entry !== null && entry !== '')) as Partial<T>;
}
