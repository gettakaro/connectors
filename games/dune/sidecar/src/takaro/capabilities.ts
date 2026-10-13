import type { MapDimension } from '../dune/mapDimensions.js';
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

/** The Capability Manifest sent with a version 1 identify. */
export function buildManifest(dimensions: readonly MapDimension[]): Capability[] {
  const manifest: Capability[] = Object.values(ACTION_CAPABILITY).map((name) => ({ name }));
  for (const type of GAME_EVENT_TYPES) manifest.push({ name: `events.${type}` });
  if (dimensions.length) manifest.push({ name: 'map', parameters: { dimensions: dimensions.map((d) => ({ ...d })) } });
  return manifest;
}
