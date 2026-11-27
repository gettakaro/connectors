// The protocol v1 Capability Manifest. Every action and event the mod implements, plus the map
// Dimension once the world survey has finished.

import type { MapDimension } from '../map/service.js';

export interface Capability {
  name: string;
  parameters?: unknown;
}

/** Actions the mod answers (TakaroCommandDispatcher) and events it emits, as v1 capabilities. */
export const ACTION_CAPABILITIES = [
  'players.list',
  'players.get',
  'players.location',
  'players.inventory',
  'players.teleport',
  'items.list',
  'items.give',
  'entities.list',
  'locations.list',
  'chat.send',
  'console.execute',
  'moderation.kick',
  'moderation.ban',
  'moderation.unban',
  'moderation.listBans',
  'server.shutdown',
] as const;

export const EVENT_CAPABILITIES = [
  'events.player-connected',
  'events.player-disconnected',
  'events.chat-message',
  'events.player-death',
  'events.entity-killed',
  'events.log',
] as const;

/** Characters live in the server's own database, and a DayZ death ends one for good. */
export const CHARACTERS_CAPABILITY: Capability = {
  name: 'players.characters',
  parameters: { held: 'server', ends: ['death'] },
};

export function buildManifest(dimension: MapDimension | null): Capability[] {
  const manifest: Capability[] = [...ACTION_CAPABILITIES, ...EVENT_CAPABILITIES].map((name) => ({ name }));
  manifest.push(CHARACTERS_CAPABILITY);
  if (dimension) manifest.push({ name: 'map', parameters: { dimensions: [dimension] } });
  return manifest;
}

/**
 * The mod reports the character as flat fields, because Enforce's JsonSerializer writes nested
 * classes poorly. Takaro takes one `character` object; a player without an id gets none.
 */
export function withCharacter(player: unknown): unknown {
  if (!player || typeof player !== 'object') return player;
  const { characterId, characterName, ...rest } = player as Record<string, unknown>;
  if (typeof characterId !== 'string' || characterId === '') return rest;
  const name = typeof characterName === 'string' && characterName !== '' ? characterName : String(rest.name ?? characterId);
  return { ...rest, character: { id: characterId, name } };
}

/**
 * Protocol v1 positions name their Dimension. Adds it to a mod answer: the player location,
 * and every position in listLocations. The mod answers an offline player's location with
 * (0, 0, 0), a spot in the sea corner no player can stand on; v1 says "unknown" with null.
 */
export function withDimension(action: string | undefined, result: unknown, dimension: string): unknown {
  if (action === 'getPlayerLocation') {
    if (!isPosition(result)) return result;
    if (result.x === 0 && result.y === 0 && result.z === 0) return null;
    return { ...result, dimension };
  }
  if (action === 'listLocations' && Array.isArray(result)) {
    return result.map((loc) => {
      const l = loc as Record<string, unknown>;
      return l && isPosition(l.position) ? { ...l, position: { ...l.position, dimension } } : loc;
    });
  }
  return result;
}

/** Adds the Dimension to an event's position (player-death, entity-killed). */
export function eventWithDimension<T extends { position?: unknown }>(event: T, dimension: string): T {
  return isPosition(event.position) ? { ...event, position: { ...event.position, dimension } } : event;
}

function isPosition(v: unknown): v is { x: number; y: number; z: number } {
  if (!v || typeof v !== 'object') return false;
  const p = v as Record<string, unknown>;
  return typeof p.x === 'number' && typeof p.y === 'number' && typeof p.z === 'number';
}
