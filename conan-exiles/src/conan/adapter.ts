import type { GameServerAction } from '../takaro/protocol.js';
import { schemaFallbackForAction, unsupportedActionError } from '../takaro/coverage.js';
import { parseListBans, parseListPlayers, type ConanPlayer } from './parsers.js';
import { seedConanItemCatalog, type ConanItemCatalog } from './itemCatalog.js';
import { type ConanSaveDbReader, type TakaroItem } from './saveDb.js';
import { logger } from '../logger.js';

export type RconExecutor = (command: string) => Promise<string>;

interface GrantedItem extends TakaroItem {
  expectedAmount?: number;
}

export interface ConanChatBridge {
  isConnected(): boolean;
  sendMessage(message: string, recipientIdentifier: string | null, senderNameOverride: string | null): Promise<unknown>;
}

export class ConanAdapter {
  private players = new Map<string, ConanPlayer>();
  private readonly knownPlayers = new Map<string, ConanPlayer>();
  private readonly grantedItemsByAlias = new Map<string, Map<string, GrantedItem>>();

  constructor(
    private readonly execute: RconExecutor,
    private readonly chatBridge?: ConanChatBridge,
    private readonly saveDb?: ConanSaveDbReader,
    private readonly itemCatalog: ConanItemCatalog = saveDb?.itemCatalog ?? seedConanItemCatalog(),
  ) {}

  async handleAction(action: GameServerAction, rawArgs: unknown): Promise<unknown> {
    const args = parseArgs(rawArgs);

    const schemaFallback = schemaFallbackForAction(action);
    if (schemaFallback !== undefined) return schemaFallback;

    const unsupported = unsupportedActionError(action);
    if (unsupported) return unsupported;

    switch (action) {
      case 'testReachability': {
        try {
          await this.execute('help');
          return { connectable: true, reason: null };
        } catch (err) {
          return { connectable: false, reason: errorMessage(err) };
        }
      }
      case 'getPlayers':
        return this.safeArray(async () => (await this.refreshPlayers()).map((player) => publicPlayer(player)));
      case 'getPlayer':
        return this.getPlayer(args);
      case 'getPlayerLocation':
        return this.getPlayerLocation(args);
      case 'getPlayerInventory':
        return this.getPlayerInventory(args);
      case 'listItems':
        return this.safeDbArray(() => this.saveDb?.listItems() ?? []);
      case 'listEntities':
        return this.safeDbArray(() => this.saveDb?.listEntities() ?? []);
      case 'listLocations':
        return this.safeDbArray(() => this.saveDb?.listPlayerLocations() ?? []);
      case 'executeConsoleCommand':
        return this.executeConsoleCommand(args);
      case 'sendMessage':
        return this.sendMessage(args);
      case 'giveItem':
        return this.giveItem(args);
      case 'teleportPlayer':
        return this.teleportPlayer(args);
      case 'kickPlayer':
        return this.success(await this.execute(`kickplayer ${identifierArgs(args)}${reasonSuffix(args)}`));
      case 'banPlayer':
        return this.success(await this.execute(`banplayer ${identifierArgs(args)}${reasonSuffix(args)}`));
      case 'unbanPlayer':
        return this.success(await this.execute(`unbanplayer ${identifierArgs(args)}`));
      case 'listBans':
        return this.safeArray(async () => parseListBans(await this.execute('listbans')));
      case 'shutdown':
        return this.success(await this.execute('shutdown'));
      default:
        return {
          success: false,
          error: `Unknown action ${action}`,
        };
    }
  }

  async getPlayers(): Promise<ConanPlayer[]> {
    return this.safeArray(() => this.refreshPlayers());
  }

  async getKnownPlayersForEvents(): Promise<ConanPlayer[]> {
    const livePlayers = await this.getPlayers();
    return livePlayers.length ? livePlayers : [...this.knownPlayers.values()];
  }

  private async refreshPlayers(): Promise<ConanPlayer[]> {
    const players = parseListPlayers(await this.execute('listplayers'));
    this.players.clear();
    for (const player of players) {
      this.players.set(player.gameId, player);
      if (player.steamId) this.players.set(player.steamId, player);
      if (player.platformId) this.players.set(player.platformId, player);
      this.players.set(player.name.toLowerCase(), player);
    }
    if (players.length) {
      this.knownPlayers.clear();
      for (const player of players) this.knownPlayers.set(player.gameId, player);
    }
    return players;
  }

  private async getPlayer(args: Record<string, unknown>): Promise<unknown> {
    if (this.players.size === 0) await this.refreshPlayers();
    const identifier = optionalString(args, ['gameId', 'steamId', 'platformId', 'name', 'playerId']);
    if (!identifier) return { success: false, error: 'Missing player identifier' };
    const player = this.players.get(identifier) || this.players.get(identifier.toLowerCase());
    return player ? publicPlayer(player) : null;
  }

  private async getPlayerLocation(args: Record<string, unknown>): Promise<unknown> {
    const candidates = await this.playerReadCandidates(args);
    if (!candidates.length) return { x: 0, y: 0, z: 0 };
    return this.locationForCandidates(candidates) ?? { x: 0, y: 0, z: 0 };
  }

  private async getPlayerInventory(args: Record<string, unknown>): Promise<unknown> {
    const candidates = await this.playerReadCandidates(args);
    if (!candidates.length) return [];
    const dbItems = this.inventoryForCandidates(candidates);
    return this.mergeGrantedItems(dbItems, candidates);
  }

  private async sendMessage(args: Record<string, unknown>): Promise<unknown> {
    if (!this.chatBridge?.isConnected()) {
      return {
        success: false,
        error: 'Conan chat bridge is not connected; vanilla RCON broadcast is not used for Takaro chat messages',
      };
    }

    const message = requireString(args, ['message', 'text']);
    const recipient = optionalRecipientIdentifier(args);
    const senderNameOverride = optionalSenderNameOverride(args);
    return this.chatBridge.sendMessage(message, recipient, senderNameOverride);
  }

  private async giveItem(args: Record<string, unknown>): Promise<unknown> {
    logger.info(`giveItem args keys=${Object.keys(args).sort().join(',') || '<none>'}`);
    const player = await this.requireOnlinePlayer(args);
    const requestedItem = requireItemIdentifier(args);
    const spawnItemCode = this.itemCatalog.resolveTemplateId(requestedItem);
    requireCleanConsoleToken(spawnItemCode);
    const itemCode = this.itemCatalog.publicCodeForTemplate(spawnItemCode);
    const amount = positiveInteger(args, ['amount', 'quantity'], 1);
    const requestAliases = playerIdentifierCandidates(args);
    const beforeAmount = this.inventoryAmountForAliases([...requestAliases, ...playerAliases(player)], itemCode);
    const command = `con ${conTarget(player)} SpawnItem ${spawnItemCode} ${amount}`;
    logger.info(
      `giveItem resolved requestedItem=${JSON.stringify(requestedItem)} templateId=${spawnItemCode} publicCode=${itemCode} amount=${amount} target=${conTarget(player)}`,
    );
    const rawResult = await this.execute(command);
    logger.info(`giveItem RCON command=${JSON.stringify(command)} result=${JSON.stringify(rawResult)}`);
    const result = this.success(rawResult);
    this.recordGrantedItem(player, requestAliases, itemCode, amount, beforeAmount);
    return result;
  }

  private async teleportPlayer(args: Record<string, unknown>): Promise<unknown> {
    const player = await this.requireOnlinePlayer(args);
    const x = requireNumber(args, ['x']);
    const y = requireNumber(args, ['y']);
    const z = requireNumber(args, ['z']);
    return this.success(await this.execute(`con ${conTarget(player)} TeleportPlayer ${x} ${y} ${z}`));
  }

  private async executeConsoleCommand(args: Record<string, unknown>): Promise<unknown> {
    try {
      return this.success(await this.execute(requireString(args, ['command', 'rawCommand'])));
    } catch (err) {
      return {
        success: false,
        rawResult: errorMessage(err),
      };
    }
  }

  private success(rawResult: string): { success: true; rawResult: string } {
    return { success: true, rawResult };
  }

  private async safeArray<T>(loader: () => Promise<T[]>): Promise<T[]> {
    try {
      return await loader();
    } catch {
      return [];
    }
  }

  private safeDbArray<T>(loader: () => T[]): T[] {
    try {
      return loader();
    } catch {
      return [];
    }
  }

  private safeDbValue<T>(loader: () => T | null): T | null {
    try {
      return loader();
    } catch {
      return null;
    }
  }

  private inventoryForCandidates(candidates: string[]): TakaroItem[] {
    for (const candidate of candidates) {
      const items = this.safeDbArray(() => this.saveDb?.getPlayerInventory(candidate) ?? []);
      if (items.length) return items;
    }
    return [];
  }

  private locationForCandidates(candidates: string[]): ReturnType<ConanSaveDbReader['getPlayerLocation']> {
    for (const candidate of candidates) {
      const location = this.safeDbValue(() => this.saveDb?.getPlayerLocation(candidate) ?? null);
      if (location) return location;
    }
    return null;
  }

  private async playerReadCandidates(args: Record<string, unknown>): Promise<string[]> {
    const candidates = playerIdentifierCandidates(args);
    if (candidates.length !== 1 || !maybeOpaqueTakaroPlayerId(candidates[0]!)) return candidates;
    const players = await this.refreshPlayers();
    if (players.length !== 1) return candidates;
    return [...new Set([...candidates, ...playerAliases(players[0]!)])];
  }

  private inventoryAmountForAliases(aliases: string[], code: string): number {
    const existing = this.inventoryForCandidates(aliases).find((item) => item.code === code);
    return existing?.amount ?? 0;
  }

  private async requireOnlinePlayer(args: Record<string, unknown>): Promise<ConanPlayer> {
    const players = await this.refreshPlayers();
    const candidates = playerIdentifierCandidates(args);
    const player = players.find((candidate) => playerMatches(candidate, candidates));
    if (player) return player;
    if (players.length === 1 && candidates.length === 1 && maybeOpaqueTakaroPlayerId(candidates[0]!)) {
      return players[0]!;
    }
    throw new Error(`No online Conan player matched ${candidates.length ? candidates.join(', ') : 'the request'}`);
  }

  private recordGrantedItem(
    player: ConanPlayer,
    requestAliases: string[],
    code: string,
    amount: number,
    beforeAmount: number,
  ): void {
    const aliases = [...new Set([...requestAliases, ...playerAliases(player)].map(normalizeMatchValue))];
    for (const alias of aliases) {
      let grants = this.grantedItemsByAlias.get(alias);
      if (!grants) {
        grants = new Map<string, GrantedItem>();
        this.grantedItemsByAlias.set(alias, grants);
      }
      const existing = grants.get(code);
      if (existing) {
        existing.amount = (existing.amount ?? 0) + amount;
        existing.expectedAmount = Math.max(existing.expectedAmount ?? beforeAmount, beforeAmount) + amount;
      } else {
        grants.set(code, {
          code,
          name: this.itemCatalog.nameForTemplate(this.itemCatalog.resolveTemplateId(code)),
          amount,
          expectedAmount: beforeAmount + amount,
          quality: '1',
        });
      }
    }
  }

  private mergeGrantedItems(dbItems: TakaroItem[], aliases: string[]): TakaroItem[] {
    const merged = dbItems.map((item) => {
      const copy = { ...item };
      if (item.position) copy.position = { ...item.position };
      return copy;
    });
    const grants = aliases.map(normalizeMatchValue).map((alias) => this.grantedItemsByAlias.get(alias)).find(Boolean);
    if (!grants) return merged;
    for (const grant of grants.values()) {
      const existing = merged.find((item) => item.code === grant.code);
      if (existing) {
        const currentAmount = existing.amount ?? 0;
        const expectedAmount = grant.expectedAmount ?? currentAmount + (grant.amount ?? 0);
        if (currentAmount >= expectedAmount) {
          grants.delete(grant.code);
          continue;
        }
        existing.amount = expectedAmount;
        existing.name = grant.name;
      } else {
        const { expectedAmount: _expectedAmount, ...item } = grant;
        merged.push({ ...item });
      }
    }
    return merged;
  }
}

function publicPlayer(player: ConanPlayer): Omit<ConanPlayer, 'characterName' | 'rconId'> {
  const { characterName: _characterName, rconId: _rconId, ...rest } = player;
  return rest;
}

export function parseArgs(rawArgs: unknown): Record<string, unknown> {
  if (typeof rawArgs === 'string') {
    if (!rawArgs.trim()) return {};
    return JSON.parse(rawArgs) as Record<string, unknown>;
  }
  if (rawArgs && typeof rawArgs === 'object' && !Array.isArray(rawArgs)) return rawArgs as Record<string, unknown>;
  return {};
}

function optionalString(args: Record<string, unknown>, keys: string[]): string | null {
  for (const key of keys) {
    const value = args[key];
    if (typeof value === 'string' && value.trim()) return value.trim();
    if (typeof value === 'number') return String(value);
  }
  return null;
}

function requireString(args: Record<string, unknown>, keys: string[]): string {
  const value = optionalString(args, keys);
  if (!value) throw new Error(`Missing required argument: ${keys.join(' or ')}`);
  return value;
}

function requireItemIdentifier(args: Record<string, unknown>): string {
  const direct = optionalString(args, ['itemCode', 'code', 'name']);
  if (direct) return direct;

  const item = args.item;
  if (typeof item === 'string' && item.trim()) return item.trim();
  if (typeof item === 'number') return String(item);

  const itemRecord = recordValue(item);
  if (itemRecord) {
    const nested = optionalString(itemRecord, ['itemCode', 'code', 'templateId', 'name']);
    if (nested) return nested;
  }

  throw new Error('Missing required argument: itemCode or item or code or name');
}

function requireNumber(args: Record<string, unknown>, keys: string[]): number {
  for (const key of keys) {
    const value = args[key];
    if (typeof value === 'number' && Number.isFinite(value)) return value;
    if (typeof value === 'string' && value.trim() && Number.isFinite(Number(value))) return Number(value);
  }
  throw new Error(`Missing required numeric argument: ${keys.join(' or ')}`);
}

function positiveInteger(args: Record<string, unknown>, keys: string[], fallback: number): number {
  for (const key of keys) {
    const value = args[key];
    const parsed = typeof value === 'number' ? value : typeof value === 'string' ? Number(value) : Number.NaN;
    if (Number.isFinite(parsed) && parsed > 0) return Math.floor(parsed);
  }
  return fallback;
}

function requireCleanToken(args: Record<string, unknown>, keys: string[]): string {
  const value = requireString(args, keys);
  requireCleanConsoleToken(value);
  return value;
}

function requireCleanConsoleToken(value: string): void {
  if (!/^[A-Za-z0-9_:/.-]+$/.test(value)) throw new Error(`Invalid command token: ${value}`);
}

function identifierArgs(args: Record<string, unknown>): string {
  const records = identifierRecords(args);

  const platformId = firstString(records, ['platformId']);
  if (platformId) return `platformid ${conanPlatformIdentifier(platformId)}`;

  const steamId = firstString(records, ['steamId']);
  if (steamId) return `platformid ${steamId}`;

  const gameId = firstString(records, ['gameId', 'userId']);
  if (gameId) return isSteamId(gameId) ? `platformid ${gameId}` : `userid ${gameId}`;

  const name = firstString(records, ['name', 'playerName']);
  if (name) return `name ${quote(name)}`;

  throw new Error('Missing player identifier');
}

function reasonSuffix(args: Record<string, unknown>): string {
  const reason = optionalString(args, ['reason', 'message']);
  return reason ? ` ${quote(reason)}` : '';
}

function quote(value: string): string {
  return `"${value.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
}

function quoteConsoleToken(value: string): string {
  return /^[A-Za-z0-9_#:/.-]+$/.test(value) ? value : quote(value);
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

function conanPlatformIdentifier(platformId: string): string {
  const parts = platformId.split(':');
  return parts.length > 1 ? parts.slice(1).join(':') : platformId;
}

function isSteamId(value: string): boolean {
  return /^7656\d{13}$/.test(value);
}

function identifierRecords(args: Record<string, unknown>): Record<string, unknown>[] {
  const records = [args];
  const player = recordValue(args.player);
  if (player) {
    records.push(player);
    const nestedPlayer = recordValue(player.player);
    if (nestedPlayer) records.push(nestedPlayer);
  }
  return records;
}

function playerIdentifierCandidates(args: Record<string, unknown>): string[] {
  const candidates = [
    optionalString(args, ['gameId', 'steamId', 'platformId', 'userId', 'playerId']),
  ].filter((value): value is string => Boolean(value));

  const player = recordValue(args.player);
  if (player) {
    candidates.push(
      ...['gameId', 'steamId', 'platformId', 'userId', 'playerId', 'name', 'playerName']
        .map((key) => optionalString(player, [key]))
        .filter((value): value is string => Boolean(value)),
    );
    const nestedPlayer = recordValue(player.player);
    if (nestedPlayer) {
      candidates.push(
        ...['gameId', 'steamId', 'platformId', 'userId', 'playerId', 'name', 'playerName']
          .map((key) => optionalString(nestedPlayer, [key]))
          .filter((value): value is string => Boolean(value)),
      );
    }
  }

  return [...new Set(candidates.map((value) => value.trim()).filter(Boolean))];
}

function playerMatches(player: ConanPlayer, candidates: string[]): boolean {
  const aliases = playerAliases(player).map(normalizeMatchValue);
  return candidates.map(normalizeMatchValue).some((candidate) => aliases.includes(candidate));
}

function playerAliases(player: ConanPlayer): string[] {
  return [
    player.gameId,
    player.steamId,
    player.platformId,
    player.platformId ? conanPlatformIdentifier(player.platformId) : null,
    player.name,
    player.characterName,
    player.rconId,
  ].filter((value): value is string => Boolean(value));
}

function normalizeMatchValue(value: string): string {
  return conanPlatformIdentifier(value).toLowerCase();
}

function maybeOpaqueTakaroPlayerId(value: string): boolean {
  return /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(value);
}

function conTarget(player: ConanPlayer): string {
  return quoteConsoleToken(player.rconId || player.characterName || player.name || player.gameId);
}

function recordValue(value: unknown): Record<string, unknown> | null {
  return value && typeof value === 'object' && !Array.isArray(value) ? (value as Record<string, unknown>) : null;
}

function firstString(records: Record<string, unknown>[], keys: string[]): string | null {
  for (const record of records) {
    const value = optionalString(record, keys);
    if (value) return value;
  }
  return null;
}

function optionalRecipientIdentifier(args: Record<string, unknown>): string | null {
  const records = identifierRecords(args);
  const opts = recordValue(args.opts);
  if (opts) {
    records.push(opts);
    const recipient = recordValue(opts.recipient);
    if (recipient) records.push(recipient);
  }
  const recipient = recordValue(args.recipient);
  if (recipient) records.push(recipient);

  const platformId = firstString(records, ['platformId']);
  if (platformId) return conanPlatformIdentifier(platformId);

  const steamId = firstString(records, ['steamId']);
  if (steamId) return steamId;

  const gameId = firstString(records, ['gameId', 'userId', 'playerId']);
  if (gameId) return gameId;

  return firstString(records, ['name', 'playerName']);
}

function optionalSenderNameOverride(args: Record<string, unknown>): string | null {
  const opts = recordValue(args.opts);
  return opts ? optionalString(opts, ['senderNameOverride']) : null;
}
