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

function playerDto(value: unknown): { gameId: string; name?: string; ip?: string } | undefined {
  const source = record(value);
  const gameId = stringValue(source.gameId) || stringValue(source.name) || stringValue(source.platformId);
  if (!gameId) return undefined;
  return compact({
    gameId: gameId.startsWith('terraria:') ? gameId.slice('terraria:'.length) : gameId,
    name: stringValue(source.name),
    ip: stringValue(source.ip),
  }) as { gameId: string; name?: string; ip?: string };
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
