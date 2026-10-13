import { ACTION_COVERAGE, EVENT_COVERAGE } from './coverage.js';
import { mapCapability, type MapImageMeta } from './mapImage.js';
import { ALL_GAME_EVENT_TYPES, ALL_GAME_SERVER_ACTIONS, type GameServerAction } from './protocol.js';

export interface Capability {
  name: string;
  parameters?: unknown;
}

/** Protocol v1 capability per action. Actions without one are not capabilities (testReachability) or v0 only. */
export const ACTION_CAPABILITY: Partial<Record<GameServerAction, string>> = {
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

/**
 * The Capability Manifest: exactly the live-supported actions and events of the coverage
 * registry, so a placeholder answer (schema-fallback) is never declared, plus the map once
 * the plugin has rendered one.
 */
export function buildManifest(map: MapImageMeta | null): Capability[] {
  const manifest: Capability[] = [];
  for (const action of ALL_GAME_SERVER_ACTIONS) {
    const name = ACTION_CAPABILITY[action];
    if (name && ACTION_COVERAGE[action].status === 'live-supported') manifest.push({ name });
  }
  for (const type of ALL_GAME_EVENT_TYPES) {
    if (EVENT_COVERAGE[type].status === 'live-supported') manifest.push({ name: `events.${type}` });
  }
  const mapEntry = mapCapability(map);
  if (mapEntry) manifest.push(mapEntry);
  return manifest;
}
