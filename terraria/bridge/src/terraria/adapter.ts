import { logger } from '../logger.js';
import { schemaFallbackForAction, unsupportedActionError } from '../takaro/coverage.js';
import type { GameServerAction } from '../takaro/protocol.js';
import type { CommandResult, TShockBan, TShockPlayer, TShockStatus } from '../tshock/client.js';
import { listTerrariaItems, resolveTerrariaItemCode, type TerrariaItemCatalogEntry } from './itemCatalog.js';

export type TakaroItem = Omit<TerrariaItemCatalogEntry, 'aliases'>;

export interface TShockApi {
  testToken(): Promise<CommandResult>;
  status(): Promise<TShockStatus>;
  players(): Promise<TShockPlayer[]>;
  broadcast(message: string): Promise<CommandResult>;
  rawCommand(command: string): Promise<CommandResult>;
  createBan(input: { name?: string; ip?: string; reason?: string }): Promise<CommandResult>;
  destroyBan(input: { user: string; type?: 'user' | 'ip' }): Promise<CommandResult>;
  listBans(): Promise<TShockBan[]>;
  shutdown(save?: boolean): Promise<CommandResult>;
}

export interface AdapterOptions {
  commandAllowlistExact: string[];
  commandAllowlistPrefixes: string[];
  enableShutdown: boolean;
  serverChatName?: string;
}

export class TerrariaAdapter {
  private players = new Map<string, TShockPlayer>();

  constructor(
    private readonly tshock: TShockApi,
    private readonly options: AdapterOptions,
  ) {}

  async handleAction(action: GameServerAction | undefined, rawArgs: unknown): Promise<unknown> {
    if (!action) return { success: false, error: 'Missing action' };
    const args = parseArgs(rawArgs);

    const schemaFallback = schemaFallbackForAction(action);
    if (schemaFallback !== undefined) return schemaFallback;

    const unsupported = unsupportedActionError(action);
    if (unsupported) return unsupported;

    switch (action) {
      case 'testReachability':
        return this.testReachability();
      case 'getPlayers':
        return this.listPlayersForTakaro();
      case 'getPlayer':
        return this.getPlayer(args);
      case 'getPlayerLocation':
        return this.getPlayerLocation(args);
      case 'getPlayerInventory':
        return this.getPlayerInventory(args);
      case 'executeConsoleCommand':
        return this.executeConsoleCommand(args);
      case 'sendMessage':
        return this.sendMessage(args);
      case 'kickPlayer':
        return this.tshock.rawCommand(`/kick ${quote(identifierName(args))}${reasonSuffix(args)}`);
      case 'banPlayer':
        return this.tshock.createBan({ name: identifierName(args), reason: optionalString(args, ['reason', 'message']) || undefined });
      case 'unbanPlayer':
        return this.tshock.destroyBan({ user: identifierName(args), type: 'user' });
      case 'listBans':
        return this.tshock.listBans();
      case 'listItems':
        return listTerrariaItems();
      case 'giveItem':
        return this.giveItem(args);
      case 'teleportPlayer':
        return this.tshock.rawCommand(`/takarotp ${quote(identifierName(args))} ${requireNumber(args, ['x'])} ${requireNumber(args, ['y'])}`);
      case 'shutdown':
        return this.shutdown();
      default:
        return { success: false, error: `Unknown action ${action}` };
    }
  }

  /**
   * Poller-facing read. This one deliberately propagates TShock failures so the poller can
   * tell "nobody is online" from "the game server is unreachable" and report it to /health.
   */
  async getPlayers(): Promise<TShockPlayer[]> {
    return this.refreshPlayers();
  }

  /**
   * Takaro-facing read for the getPlayers action. Takaro validates this action's response
   * against an array schema, so a thrown TShock error must not escape to the request
   * handler: that handler answers with an error envelope ({ message }), and Takaro rejects
   * the object with "Expected array for action getPlayers but got object". An unreachable
   * game server means no observable players, which is the empty array. Reachability is
   * reported through /health and the poller's own logging, not by breaking this schema.
   */
  private async listPlayersForTakaro(): Promise<TShockPlayer[]> {
    try {
      return await this.refreshPlayers();
    } catch (err) {
      logger.warn(`getPlayers falling back to empty list: ${errorMessage(err)}`);
      return [];
    }
  }

  private async testReachability(): Promise<{ connectable: boolean; reason: string | null }> {
    try {
      const token = await this.tshock.testToken();
      const status = await this.tshock.status();
      return {
        connectable: token.success && typeof status === 'object',
        reason: token.success ? null : token.rawResult,
      };
    } catch (err) {
      return { connectable: false, reason: errorMessage(err) };
    }
  }

  private async refreshPlayers(): Promise<TShockPlayer[]> {
    const players = await this.tshock.players();
    this.players.clear();
    for (const player of players) {
      this.players.set(player.gameId, player);
      this.players.set(player.name.toLowerCase(), player);
      if (player.platformId) this.players.set(player.platformId, player);
    }
    return players;
  }

  private async getPlayer(args: Record<string, unknown>): Promise<TShockPlayer | null | { success: false; error: string }> {
    if (this.players.size === 0) await this.refreshPlayers();
    const identifier = optionalNestedString(args, ['gameId', 'platformId', 'name', 'playerId']);
    if (!identifier) return { success: false, error: 'Missing player identifier' };
    return this.players.get(identifier) || this.players.get(identifier.toLowerCase()) || null;
  }

  private async getPlayerLocation(args: Record<string, unknown>): Promise<{ x: number; y: number; z: number }> {
    const result = await this.tshock.rawCommand(`/takaropos ${quote(identifierName(args))}`);
    const marker = result.rawResult.match(/TAKARO_POSITION\s+({[^}]+})/);
    if (!result.success || !marker) return { x: 0, y: 0, z: 0 };

    try {
      const parsed = JSON.parse(marker[1]) as { x?: unknown; y?: unknown; z?: unknown };
      return {
        x: finiteNumber(parsed.x) ?? 0,
        y: finiteNumber(parsed.y) ?? 0,
        z: finiteNumber(parsed.z) ?? 0,
      };
    } catch {
      return { x: 0, y: 0, z: 0 };
    }
  }

  private async getPlayerInventory(args: Record<string, unknown>): Promise<TakaroItem[]> {
    let result: CommandResult;
    try {
      result = await this.tshock.rawCommand(`/takaroinv ${quote(identifierName(args))}`);
    } catch {
      return [];
    }

    if (!result.success) return [];
    const parsed = parseMarker(result.rawResult, 'TAKARO_INVENTORY');
    if (!parsed || !Array.isArray(parsed.items)) return [];
    return parsed.items
      .map((entry) => toTakaroItem(entry))
      .filter((item): item is TakaroItem => item !== null);
  }

  /**
   * Delivers a shop item through the plugin's /takarogive rather than TShock's /give.
   *
   * TShock's /give refuses outright when the player has no completely empty inventory slot,
   * answering "Player does not have free slots!" — which previously left a Takaro shop order
   * marked COMPLETED with the player charged and nothing delivered. /takarogive places items
   * itself: it stacks onto partial stacks, and refuses unless the whole amount fits. Nothing
   * is ever dropped on the ground, and a partial delivery is never attempted, since half an
   * order is still a lost order.
   *
   * A refusal is reported as { error } so Takaro throws rather than completing quietly. Takaro
   * flips shop orders to COMPLETED before delivery and swallows delivery errors, so this does
   * not itself fail the order — but it fires shop-order-delivery-failed carrying the reason
   * and the exact items, which is the record a disputed order is resolved from.
   */
  private async giveItem(args: Record<string, unknown>): Promise<CommandResult | { error: string }> {
    const itemCode = resolveTerrariaItemCode(requireString(args, ['itemCode', 'item', 'code', 'name']));
    const player = identifierName(args);
    const amount = parseAmount(args);

    let result: CommandResult;
    try {
      result = await this.tshock.rawCommand(`/takarogive ${quote(player)} ${quote(itemCode)} ${amount}`);
    } catch (err) {
      return { error: `Failed to give item to ${player}: ${errorMessage(err)}` };
    }

    const parsed = parseMarker(result.rawResult, 'TAKARO_GIVE');

    // No marker means the plugin never ran the give (old plugin build, unknown player, or a
    // rejected command). Fall back to the raw output so the reason stays visible.
    if (!parsed) {
      if (!result.success) return { error: result.rawResult };
      return result;
    }

    if (parsed.success !== true) {
      const reason = typeof parsed.reason === 'string' && parsed.reason.trim()
        ? parsed.reason
        : result.rawResult;
      return { error: `Failed to give item to ${player}: ${reason}` };
    }

    const dropped = parsed.method === 'dropped';
    if (dropped) {
      logger.info(`giveItem: ${player}'s inventory was full, ${amount}x ${itemCode} dropped at their feet`);
    }

    // The success shape stays a CommandResult so existing Takaro handling is unchanged; the
    // delivery method rides along in rawResult so a dropped item is visible in logs.
    return {
      success: true,
      rawResult: dropped
        ? `Gave ${amount}x ${itemCode} to ${player} (inventory full, dropped at their feet)`
        : `Gave ${amount}x ${itemCode} to ${player}`,
    };
  }

  private async executeConsoleCommand(args: Record<string, unknown>): Promise<CommandResult> {
    const command = requireString(args, ['command', 'rawCommand']);
    if (!isAllowlisted(command, this.options)) {
      return { success: false, rawResult: `Command is not allowlisted: ${command}` };
    }
    return this.tshock.rawCommand(command);
  }

  private async sendMessage(args: Record<string, unknown>): Promise<CommandResult> {
    const message = requireString(args, ['message', 'text']);
    const senderName = optionalNestedString(args, ['senderNameOverride', 'senderName', 'from']) || this.options.serverChatName;
    return this.tshock.broadcast(senderName ? `${senderName}: ${message}` : message);
  }

  private async shutdown(): Promise<CommandResult> {
    if (!this.options.enableShutdown) {
      return { success: false, rawResult: 'Shutdown is disabled; set enableShutdown=true to allow this action' };
    }
    return this.tshock.shutdown(true);
  }
}

export function parseArgs(rawArgs: unknown): Record<string, unknown> {
  if (typeof rawArgs === 'string') {
    if (!rawArgs.trim()) return {};
    return JSON.parse(rawArgs) as Record<string, unknown>;
  }
  if (rawArgs && typeof rawArgs === 'object' && !Array.isArray(rawArgs)) return rawArgs as Record<string, unknown>;
  return {};
}

/**
 * Reads a single-line TAKARO_* JSON marker out of TShock command output.
 *
 * The payload can contain nested objects, so this matches to the end of the marker line
 * rather than the single-level {[^}]+} pattern getPlayerLocation can rely on. Returns null
 * when the marker is absent or malformed; callers decide what that means for their action.
 */
function parseMarker(rawResult: string, marker: string): Record<string, unknown> | null {
  const match = rawResult.match(new RegExp(`${marker}\\s+(\\{.*)`));
  if (!match) return null;

  try {
    return recordValue(JSON.parse(match[1]));
  } catch {
    return null;
  }
}

function isAllowlisted(command: string, options: AdapterOptions): boolean {
  const normalized = command.trim();
  return options.commandAllowlistExact.includes(normalized)
    || options.commandAllowlistPrefixes.some((prefix) => matchesPrefix(normalized, prefix));
}

function matchesPrefix(command: string, prefix: string): boolean {
  const normalizedPrefix = prefix.trim();
  if (!normalizedPrefix) return false;
  if (command === normalizedPrefix || command.startsWith(`${normalizedPrefix} `)) return true;
  if (!normalizedPrefix.startsWith('/')) {
    const slashPrefix = `/${normalizedPrefix}`;
    return command === slashPrefix || command.startsWith(`${slashPrefix} `);
  }
  return false;
}

function identifierName(args: Record<string, unknown>): string {
  const nestedValue = nestedRecords(args)
    .slice(1)
    .map((record) => optionalString(record, ['name', 'playerName', 'gameId', 'playerId', 'platformId']))
    .find((value): value is string => Boolean(value));
  const value = nestedValue || optionalString(args, ['playerName', 'gameId', 'playerId', 'platformId', 'name']);
  if (!value) throw new Error('Missing player identifier');
  return value.startsWith('terraria:') ? value.slice('terraria:'.length) : value;
}

function optionalNestedString(args: Record<string, unknown>, keys: string[]): string | null {
  for (const record of nestedRecords(args)) {
    const value = optionalString(record, keys);
    if (value) return value;
  }
  return null;
}

function nestedRecords(args: Record<string, unknown>): Record<string, unknown>[] {
  const records = [args];
  for (const key of ['player', 'recipient', 'opts']) {
    const record = recordValue(args[key]);
    if (record) {
      records.push(record);
      const nestedPlayer = recordValue(record.player);
      if (nestedPlayer) records.push(nestedPlayer);
      const recipient = recordValue(record.recipient);
      if (recipient) records.push(recipient);
    }
  }
  return records;
}

function recordValue(value: unknown): Record<string, unknown> | null {
  return value && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : null;
}

function optionalString(args: Record<string, unknown>, keys: string[]): string | null {
  for (const key of keys) {
    const value = args[key];
    if (typeof value === 'string' && value.trim()) return value.trim();
    if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  }
  return null;
}

function requireString(args: Record<string, unknown>, keys: string[]): string {
  const value = optionalString(args, keys);
  if (!value) throw new Error(`Missing required argument: ${keys.join(' or ')}`);
  return value;
}

function requireNumber(args: Record<string, unknown>, keys: string[]): number {
  for (const key of keys) {
    const value = args[key];
    if (typeof value === 'number' && Number.isFinite(value)) return value;
    if (typeof value === 'string' && value.trim() && Number.isFinite(Number(value))) return Number(value);
  }
  throw new Error(`Missing required number argument: ${keys.join(' or ')}`);
}

function parseAmount(args: Record<string, unknown>): number {
  const amount = optionalString(args, ['amount', 'quantity']);
  if (!amount) return 1;
  const parsed = Number.parseInt(amount, 10);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 1;
}

function toTakaroItem(entry: unknown): TakaroItem | null {
  const record = recordValue(entry);
  if (!record) return null;

  const code = itemString(record.code);
  const name = itemString(record.name);
  const amount = finiteNumber(record.amount);
  if (!code || !name || amount === undefined) return null;

  return { code, name, amount, quality: itemString(record.quality) ?? '' };
}

function itemString(value: unknown): string | null {
  if (typeof value === 'string') return value;
  if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  return null;
}

function finiteNumber(value: unknown): number | undefined {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && value.trim() && Number.isFinite(Number(value))) return Number(value);
  return undefined;
}

function reasonSuffix(args: Record<string, unknown>): string {
  const reason = optionalString(args, ['reason', 'message']);
  return reason ? ` ${quote(reason)}` : '';
}

function quote(value: string): string {
  return `"${value.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}
