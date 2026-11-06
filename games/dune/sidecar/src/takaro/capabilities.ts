import type { MapDimension } from '../dune/mapDimensions.js';
import { gameProfileCapability } from './gameProfile.js';
import { GAME_EVENT_TYPES, type GameServerAction } from './protocol.js';

export interface Capability {
  name: string;
  parameters?: unknown;
}

/**
 * Protocol v1 capability per action. Every action the adapter answers is listed; `testReachability` is not a
 * capability. Dune has no `getMapInfo`/`getMapTile`: the map is a declared Dimension the operator uploads an image
 * for.
 */
export const ACTION_CAPABILITY: Record<Exclude<GameServerAction, 'testReachability'>, string> = {
  getPlayers: 'players.list',
  getPlayer: 'players.get',
  getPlayerLocation: 'players.location',
  getPlayerInventory: 'players.inventory',
  getPlayerProgression: 'players.progression',
  teleportPlayer: 'players.teleport',
  listItems: 'items.list',
  giveItem: 'items.give',
  listEntities: 'entities.list',
  listLocations: 'locations.list',
  sendMessage: 'chat.send',
  executeConsoleCommand: 'console.execute',
  kickPlayer: 'moderation.kick',
  banPlayer: 'moderation.ban',
  unbanPlayer: 'moderation.unban',
  listBans: 'moderation.listBans',
  shutdown: 'server.shutdown',
};

/** The character's level and specializations live in the server's own database, so a client cannot spoof them. */
const CAPABILITY_PARAMETERS: Record<string, unknown> = {
  'players.progression': { source: 'server' },
};

/**
 * The Capability Manifest sent with a version 1 identify (and as a `capabilities` replacement when the game build
 * changes). `gameVersion` is the running game build, null while unknown.
 */
export function buildManifest(dimensions: readonly MapDimension[], gameVersion: string | null = null): Capability[] {
  const manifest: Capability[] = Object.values(ACTION_CAPABILITY).map((name) => ({
    name,
    ...(CAPABILITY_PARAMETERS[name] ? { parameters: CAPABILITY_PARAMETERS[name] } : {}),
  }));
  for (const type of GAME_EVENT_TYPES) manifest.push({ name: `events.${type}` });
  if (dimensions.length) manifest.push({ name: 'map', parameters: { dimensions: dimensions.map((d) => ({ ...d })) } });
  // Death does not end a Dune character, so `ends` stays out.
  manifest.push({ name: 'players.characters', parameters: { held: 'server' } });
  manifest.push(gameProfileCapability(gameVersion));
  return manifest;
}
