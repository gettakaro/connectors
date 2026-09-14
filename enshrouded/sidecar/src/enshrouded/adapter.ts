import { asRecord } from '../takaro/protocol.js';
import {
  mapBan,
  mapEntity,
  mapInventoryItem,
  mapItemDefinition,
  mapLocation,
  mapPlayer,
  mapPosition,
  num,
  str,
} from './mapping.js';
import { EnshroudedPluginClient, PluginHttpError, PluginUnimplementedError } from './pluginClient.js';
import type { PluginPlayer } from './types.js';

export class ActionError extends Error {}

type Args = Record<string, unknown>;

/**
 * Maps the 17 Takaro generic-connector actions onto the Enshrouded plugin HTTP API.
 * Returns the Takaro response payload, or throws (ActionError / plugin errors) so the caller answers with an error frame.
 */
export class EnshroudedAdapter {
  constructor(private readonly plugin: EnshroudedPluginClient) {}

  async handleAction(action: string, args: Args): Promise<unknown> {
    try {
      return await this.dispatch(action, args);
    } catch (err) {
      if (err instanceof PluginUnimplementedError) {
        throw new ActionError(`Enshrouded connector cannot perform '${action}': ${err.message}`);
      }
      throw err;
    }
  }

  private async dispatch(action: string, args: Args): Promise<unknown> {
    switch (action) {
      case 'testReachability':
        return this.testReachability();
      case 'getPlayers': {
        const players = await this.plugin.getPlayers();
        return (Array.isArray(players) ? players : []).filter((p) => p.online !== false).map(mapPlayer);
      }
      case 'getPlayer': {
        const id = playerId(args);
        const found = await this.findPlayer(id);
        if (found) return mapPlayer(found);
        try {
          return mapPlayer(await this.plugin.getPlayer(id.replace(/^steam:/i, '')));
        } catch (err) {
          if (err instanceof PluginHttpError && err.status === 404) return null;
          throw err;
        }
      }
      case 'getPlayerLocation': {
        const pluginId = await this.resolvePluginId(playerId(args));
        return mapPosition(await this.plugin.getPlayerLocation(pluginId));
      }
      case 'getPlayerInventory': {
        const pluginId = await this.resolvePluginId(playerId(args));
        const items = await this.plugin.getPlayerInventory(pluginId);
        return (Array.isArray(items) ? items : []).map(mapInventoryItem);
      }
      case 'giveItem': {
        const pluginId = await this.resolvePluginId(playerId(args));
        const code = str(args.item) ?? str(args.itemCode) ?? str(args.code) ?? str(asRecord(args.item).code) ?? fail("giveItem requires 'item'");
        const amount = num(args.amount) ?? num(args.quantity) ?? 1;
        if (amount <= 0) fail('giveItem amount must be positive');
        const quality = args.quality === undefined || args.quality === null || args.quality === '' ? undefined : String(args.quality);
        await this.plugin.give(pluginId, code, amount, quality);
        return {};
      }
      case 'listItems':
        return listOf(await this.plugin.getItems()).map(mapItemDefinition);
      case 'listEntities':
        return listOf(await this.plugin.getEntities()).map(mapEntity);
      case 'listLocations':
        return listOf(await this.plugin.getLocations()).map(mapLocation);
      case 'executeConsoleCommand': {
        const command = str(args.command) ?? fail("executeConsoleCommand requires 'command'");
        const result = await this.plugin.command(command);
        const success = result.success !== false;
        const output = typeof result.output === 'string' ? result.output : '';
        return { success, rawResult: output, errorMessage: success ? null : output || 'Command failed' };
      }
      case 'sendMessage': {
        const message = str(args.message) ?? fail("sendMessage requires 'message'");
        const opts = asRecord(args.opts);
        const sender = str(opts.senderNameOverride);
        const text = sender ? `${sender}: ${message}` : message;
        const recipient = asRecord(opts.recipient);
        const recipientId = str(recipient.gameId) ?? str(recipient.steamId) ?? str(args.recipientGameId);
        await this.plugin.sendMessage(text, recipientId ? await this.resolvePluginId(recipientId) : undefined);
        return {};
      }
      case 'teleportPlayer': {
        const pluginId = await this.resolvePluginId(playerId(args));
        const x = num(args.x);
        const y = num(args.y);
        const z = num(args.z);
        if (x === null || y === null || z === null) fail('teleportPlayer requires numeric x, y, z');
        await this.plugin.teleport(pluginId, x!, y!, z!);
        return {};
      }
      case 'kickPlayer': {
        const pluginId = await this.resolvePluginId(playerId(args));
        await this.plugin.kick(pluginId, str(args.reason) ?? undefined);
        return {};
      }
      case 'banPlayer': {
        const pluginId = await this.resolvePluginId(playerId(args));
        await this.plugin.ban(pluginId, str(args.reason) ?? undefined, str(args.expiresAt) ?? undefined);
        return {};
      }
      case 'unbanPlayer': {
        const pluginId = await this.resolvePluginId(playerId(args));
        await this.plugin.unban(pluginId);
        return {};
      }
      case 'listBans':
        return listOf(await this.plugin.getBans()).map(mapBan);
      case 'shutdown':
        await this.plugin.shutdown();
        return {};
      default:
        throw new ActionError(`Unknown Takaro action '${action}'`);
    }
  }

  private async testReachability(): Promise<{ connectable: boolean; reason: string | null }> {
    try {
      const health = await this.plugin.health();
      const status = String(health.status ?? '').toLowerCase();
      // "unimplemented" is a known, static gap (the action returns a clear error); only degraded capabilities are news.
      const caps = Object.entries(health.capabilities ?? {}).filter(([, state]) => state !== 'ok' && state !== 'unimplemented');
      const capText = caps.length ? `capabilities not ok: ${caps.map(([n, s]) => `${n}=${s}`).join(', ')}` : '';
      if (status === 'ok') return { connectable: true, reason: capText || null };
      if (status === 'degraded') return { connectable: true, reason: `Enshrouded plugin degraded${capText ? `; ${capText}` : ''}` };
      return { connectable: false, reason: `Enshrouded plugin status '${health.status}'${capText ? `; ${capText}` : ''}` };
    } catch (err) {
      return { connectable: false, reason: err instanceof Error ? err.message : String(err) };
    }
  }

  /** Finds a plugin player by Takaro gameId (SteamID), platformId, plugin gameId, or name. */
  private async findPlayer(id: string): Promise<PluginPlayer | undefined> {
    const needle = id.replace(/^steam:/i, '').toLowerCase();
    const players = await this.plugin.getPlayers();
    if (!Array.isArray(players)) return undefined;
    return (
      players.find((p) => p.steamId?.toLowerCase() === needle) ??
      players.find((p) => p.gameId?.toLowerCase() === needle) ??
      players.find((p) => p.name?.toLowerCase() === id.toLowerCase())
    );
  }

  /** Plugin endpoints take the plugin's gameId; offline players (ban/unban) fall back to the SteamID as given. */
  private async resolvePluginId(id: string): Promise<string> {
    const found = await this.findPlayer(id);
    return found?.gameId ?? id.replace(/^steam:/i, '');
  }
}

/** Accepts flat { gameId } / { steamId } / { platformId } or nested { player: {...} } / { playerRef: {...} }. */
export function playerId(args: Args): string {
  for (const source of [args, asRecord(args.player), asRecord(args.playerRef)]) {
    const id = str(source.gameId) ?? str(source.steamId) ?? str(source.platformId);
    if (id) return id;
  }
  return fail('Expected player identifier (gameId, or player.gameId)');
}

function listOf(value: unknown): unknown[] {
  if (Array.isArray(value)) return value;
  const rec = asRecord(value);
  for (const key of ['items', 'entities', 'locations', 'bans', 'data']) {
    if (Array.isArray(rec[key])) return rec[key] as unknown[];
  }
  return [];
}

function fail(message: string): never {
  throw new ActionError(message);
}
