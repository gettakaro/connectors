import { NativeClient, type NativeEntity, type NativeItem, type NativePlayer } from './native/client.js';
import { BanManager } from './native/banManager.js';
import { MemoryBanStore } from './native/banStore.js';
import { asRecord } from './takaro/protocol.js';

export interface TakaroPlayer { gameId: string; name: string; steamId: string; platformId: string }

const steam64 = (value: unknown): string | null => {
  if (typeof value !== 'string') return null;
  const id = value.replace(/^steam:/i, '');
  return /^\d{17}$/.test(id) ? id : null;
};

export function mapPlayer(source: NativePlayer): TakaroPlayer {
  const id = steam64(source.steamId) ?? steam64(source.steam64) ?? steam64(source.gameId);
  if (!id) throw new Error('Native player has no valid Steam64 identity');
  const name = typeof source.name === 'string' && source.name.trim() ? source.name.trim() : id;
  return { gameId: id, name, steamId: id, platformId: `steam:${id}` };
}

function playerId(args: Record<string, unknown>): string {
  for (const object of [args, asRecord(args.player), asRecord(args.playerRef)]) {
    const id = steam64(object.gameId) ?? steam64(object.steamId) ??
      steam64(object.platformId) ?? steam64(object.playerId);
    if (id) return id;
  }
  throw new Error('getPlayer requires a Steam64 player identifier');
}

function messageRecipient(args: Record<string, unknown>): string | null {
  const opts = asRecord(args.opts);
  const rawRecipient = opts.recipient;
  if (rawRecipient !== undefined && rawRecipient !== null &&
      (typeof rawRecipient !== 'object' || Array.isArray(rawRecipient))) {
    throw new Error('sendMessage recipient must be a player reference');
  }
  const recipient = asRecord(rawRecipient);
  const values = [args.recipientGameId, recipient.gameId, recipient.steamId, recipient.platformId]
    .filter((value) => value !== undefined && value !== null);
  if (rawRecipient && values.length === 0) {
    throw new Error('sendMessage recipient requires a Steam64 gameId');
  }
  let selected: string | null = null;
  for (const value of values) {
    const id = steam64(value);
    if (!id) throw new Error('sendMessage recipient requires a valid Steam64');
    if (selected && selected !== id) throw new Error('sendMessage recipient identities conflict');
    selected = id;
  }
  return selected;
}

function mapItem(source: NativeItem, inventory: boolean): NativeItem {
  if (!source || typeof source.code !== 'string' || !source.code.trim() ||
      typeof source.name !== 'string' || !source.name.trim()) {
    throw new Error('Native item has no verified code and name');
  }
  const item: NativeItem = { code: source.code.trim(), name: source.name.trim() };
  if (inventory) {
    if (typeof source.amount !== 'number' || !Number.isSafeInteger(source.amount) || source.amount < 1) {
      throw new Error('Native inventory item has no verified positive quantity');
    }
    item.amount = source.amount;
  }
  if (typeof source.description === 'string' && source.description.trim()) item.description = source.description.trim();
  if (typeof source.quality === 'string' && source.quality.trim()) item.quality = source.quality.trim();
  return item;
}

function mapItems(rows: unknown, inventory: boolean): NativeItem[] {
  if (!Array.isArray(rows)) throw new Error('Native items endpoint did not return an array');
  return rows.map((row) => mapItem(row as NativeItem, inventory));
}

function finiteNumber(value: unknown, label: string, limit: number): number {
  if (typeof value !== 'number' || !Number.isFinite(value) || Math.abs(value) > limit) {
    throw new Error(`${label} requires a finite number within ${limit}`);
  }
  return value;
}

function itemQuality(value: unknown): number {
  if (typeof value === 'string') {
    const trimmed = value.trim();
    if (!/^(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$/.test(trimmed)) {
      throw new Error('giveItem quality must be a finite nonnegative number');
    }
    value = Number(trimmed);
  }
  const quality = finiteNumber(value ?? 0, 'giveItem quality', 100000);
  if (quality < 0) throw new Error('giveItem quality must be nonnegative');
  return quality;
}

function consoleCommand(value: unknown): string {
  if (typeof value !== 'string' || !value.trim() || Buffer.byteLength(value, 'utf8') > 4096) {
    throw new Error('executeConsoleCommand requires nonempty text within 4096 UTF-8 bytes');
  }
  let codepoints = 0;
  for (const scalar of value) {
    const code = scalar.codePointAt(0)!;
    if (++codepoints > 1024 || code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
        (code >= 0xd800 && code <= 0xdfff) || code === 0x2028 || code === 0x2029) {
      throw new Error('executeConsoleCommand contains controls or exceeds 1024 code points');
    }
  }
  return value;
}

export class ArkAdapter {
  private readonly banManager: BanManager;
  constructor(private readonly native: NativeClient, banManager?: BanManager) {
    this.banManager = banManager ?? new BanManager(native, new MemoryBanStore());
  }

  async handle(action: string, args: Record<string, unknown>, requestId?: string): Promise<unknown> {
    switch (action) {
      case 'testReachability': {
        try {
          const health = await this.native.health(requestId);
          const ok = (health.status === 'ok' || health.status === 'ready') && !!health.bootId;
          const capabilities = asRecord(health.capabilities);
          const degraded = Object.entries(capabilities)
            .filter(([, state]) => state !== 'ok')
            .map(([name, state]) => `${name}=${String(state)}`);
          return {
            connectable: ok,
            reason: ok ? (degraded.length ? `Native capabilities: ${degraded.join(', ')}` : null)
              : `Native status ${health.status || 'unknown'}`,
          };
        } catch (error) {
          return { connectable: false, reason: error instanceof Error ? error.message : String(error) };
        }
      }
      case 'getPlayers': {
        const rows = await this.native.players(requestId);
        if (!Array.isArray(rows)) throw new Error('Native /players did not return an array');
        return rows.map(mapPlayer);
      }
      case 'getPlayer': {
        const row = await this.native.player(playerId(args), requestId);
        return row ? mapPlayer(row) : null;
      }
      case 'getPlayerLocation': {
        const position = await this.native.playerLocation(playerId(args), requestId);
        if (!position || ![position.x, position.y, position.z].every(
          (coordinate) => typeof coordinate === 'number' && Number.isFinite(coordinate),
        )) throw new Error('Native player location is unavailable or invalid');
        return { x: position.x, y: position.y, z: position.z };
      }
      case 'getPlayerInventory': {
        return mapItems(await this.native.playerInventory(playerId(args), requestId), true);
      }
      case 'giveItem': {
        const item = asRecord(args.item);
        const code = [args.item, args.itemCode, args.code, args.name, item.code]
          .find((value): value is string => typeof value === 'string' && !!value.trim());
        if (!code || !/^(?:Blueprint')?\/Game\/[A-Za-z0-9_/.]+_C'?$/.test(code) ||
            (code.startsWith('Blueprint\'') !== code.endsWith('\''))) {
          throw new Error('giveItem requires a native /Game item class path');
        }
        const amount = args.amount ?? args.quantity ?? 1;
        if (typeof amount !== 'number' || !Number.isSafeInteger(amount) || amount < 1 || amount > 10000) {
          throw new Error('giveItem amount must be an integer from 1 to 10000');
        }
        const quality = itemQuality(args.quality);
        const blueprint = args.blueprint ?? false;
        if (typeof blueprint !== 'boolean') throw new Error('giveItem blueprint must be boolean');
        const ack = await this.native.giveItem(playerId(args), { code, amount, quality, blueprint }, requestId);
        if (ack?.success !== true) throw new Error('Native item grant was not verified');
        return {};
      }
      case 'teleportPlayer': {
        if (args.target !== undefined && args.target !== null && args.target !== '') {
          throw new Error('teleportPlayer target lookup is unavailable on this ARK native build');
        }
        const dimension = args.dimension;
        if (dimension !== undefined && dimension !== null) {
          throw new Error('teleportPlayer dimension is unavailable on this ARK native build');
        }
        const position = {
          x: finiteNumber(args.x, 'teleportPlayer x', 100000000),
          y: finiteNumber(args.y, 'teleportPlayer y', 100000000),
          z: finiteNumber(args.z, 'teleportPlayer z', 100000000),
        };
        const ack = await this.native.teleport(playerId(args), position, requestId);
        if (ack?.success !== true) throw new Error('Native teleport was not verified');
        return {};
      }
      case 'listItems': {
        const search = args.search;
        if (search !== undefined && typeof search !== 'string') throw new Error('listItems search must be text');
        const rows: NativeItem[] = [];
        const seen = new Set<string>();
        let offset = 0;
        let total: number | undefined;
        let complete = false;
        for (let pageNumber = 0; pageNumber < 512 && !complete; ++pageNumber) {
          const page = await this.native.itemPage(offset, 128, requestId);
          if (!page || !Array.isArray(page.items) ||
              !Number.isSafeInteger(page.offset) || page.offset !== offset ||
              !Number.isSafeInteger(page.nextOffset) || page.nextOffset <= offset ||
              !Number.isSafeInteger(page.total) || page.total < 1 || page.total > 65536 ||
              page.nextOffset > page.total || typeof page.complete !== 'boolean' ||
              (total !== undefined && page.total !== total) ||
              (page.complete !== (page.nextOffset === page.total))) {
            throw new Error('Native item catalog page is incomplete or invalid');
          }
          total = page.total;
          for (const item of mapItems(page.items, false)) {
            if (seen.has(item.code)) throw new Error('Native item catalog contains duplicate class paths');
            seen.add(item.code);
            rows.push(item);
          }
          offset = page.nextOffset;
          complete = page.complete;
        }
        if (!complete) throw new Error('Native item catalog did not complete within bounded pages');
        const term = typeof search === 'string' ? search.trim().toLocaleLowerCase() : '';
        return term ? rows.filter((row) => row.code.toLocaleLowerCase().includes(term) ||
          row.name.toLocaleLowerCase().includes(term)) : rows;
      }
      case 'listEntities': {
        const search = args.search;
        if (search !== undefined && typeof search !== 'string') throw new Error('listEntities search must be text');
        const rows: NativeEntity[] = [];
        const seen = new Set<string>();
        let offset = 0;
        let total: number | undefined;
        let complete = false;
        for (let pageNumber = 0; pageNumber < 512 && !complete; ++pageNumber) {
          const page = await this.native.entityPage(offset, 128, requestId);
          if (!page || !Array.isArray(page.items) ||
              !Number.isSafeInteger(page.offset) || page.offset !== offset ||
              !Number.isSafeInteger(page.nextOffset) || page.nextOffset <= offset ||
              !Number.isSafeInteger(page.total) || page.total < 1 || page.total > 65536 ||
              page.nextOffset > page.total || page.items.length !== page.nextOffset - offset ||
              typeof page.complete !== 'boolean' ||
              (total !== undefined && page.total !== total) ||
              page.complete !== (page.nextOffset === page.total)) {
            throw new Error('Native entity catalog page is incomplete or invalid');
          }
          total = page.total;
          for (const source of page.items) {
            if (!source || typeof source.code !== 'string' || !source.code.trim() ||
                typeof source.name !== 'string' || !source.name.trim()) {
              throw new Error('Native entity has no verified code and name');
            }
            const entity = { code: source.code.trim(), name: source.name.trim() };
            if (seen.has(entity.code)) throw new Error('Native entity catalog contains duplicate codes');
            seen.add(entity.code);
            rows.push(entity);
          }
          offset = page.nextOffset;
          complete = page.complete;
        }
        if (!complete) throw new Error('Native entity catalog did not complete within bounded pages');
        const term = typeof search === 'string' ? search.trim().toLocaleLowerCase() : '';
        return term ? rows.filter((row) => row.code.toLocaleLowerCase().includes(term) ||
          row.name.toLocaleLowerCase().includes(term)) : rows;
      }
      case 'listLocations': {
        if (Object.values(args).some((value) => value !== undefined && value !== null)) {
          throw new Error('listLocations accepts no options on this native build');
        }
        const rows = await this.native.locations(requestId);
        if (!Array.isArray(rows) || rows.length < 1 || rows.length > 1024) {
          throw new Error('Native loaded spawn-point snapshot is unavailable or invalid');
        }
        const seen = new Set<string>();
        return rows.map((row) => {
          if (!row || typeof row.code !== 'string' || !row.code.trim() || row.code.length > 512 ||
              typeof row.name !== 'string' || !row.name.trim() || row.name.length > 512 ||
              !row.position ||
              ![row.position.x, row.position.y, row.position.z].every(
                (value) => typeof value === 'number' && Number.isFinite(value) && Math.abs(value) <= 1e9,
              ) ||
              ![row.sizeX, row.sizeY, row.sizeZ].every(
                (value) => typeof value === 'number' && Number.isFinite(value) && value > 0 && value <= 1e9,
              )) throw new Error('Native spawn-point bounds are unavailable or invalid');
          if (seen.has(row.code)) throw new Error('Native spawn-point snapshot has duplicate codes');
          seen.add(row.code);
          return { code: row.code, name: row.name,
            position: { x: row.position.x, y: row.position.y, z: row.position.z },
            sizeX: row.sizeX, sizeY: row.sizeY, sizeZ: row.sizeZ };
        });
      }
      case 'kickPlayer':
      case 'banPlayer':
      case 'unbanPlayer': {
        if (args.reason != null && typeof args.reason !== 'string') {
          throw new Error(`${action} reason must be text`);
        }
        const reason = typeof args.reason === 'string' ? args.reason : '';
        if (action === 'kickPlayer') {
          if (Buffer.byteLength(reason, 'utf8') > 1000 || [...reason].length > 480 ||
              /[\u0000-\u001f\u007f-\u009f\u2028\u2029\ud800-\udfff]/u.test(reason)) {
            throw new Error('kickPlayer reason must be a single line of at most 480 characters');
          }
        } else if (action === 'unbanPlayer' && reason.trim()) {
          throw new Error('unbanPlayer accepts no reason');
        }
        // Takaro owns the managed ban reason. ARK's native ban collection
        // stores Steam64 only, so the reason is not sent to the game endpoint.
        const nativeAction = action === 'kickPlayer' ? 'kick' : action === 'banPlayer' ? 'ban' : 'unban';
        const id = playerId(args);
        if (action === 'banPlayer') {
          await this.banManager.ban(id, args.reason, args.expiresAt, requestId);
          return {};
        }
        if (args.expiresAt != null) throw new Error(`${action} accepts no expiresAt`);
        if (action === 'unbanPlayer') {
          await this.banManager.unban(id, requestId);
          return {};
        }
        if (action === 'kickPlayer' && reason.trim()) {
          const notice = await this.native.messageTo(id, `Kick reason: ${reason}`, requestId);
          if (!notice || notice.success !== true) throw new Error('Native kick reason notice was not queued');
        }
        const ack = await this.native.moderate(id, nativeAction, requestId);
        if (!ack || ack.success !== true) {
          throw new Error(typeof ack?.errorMessage === 'string' && ack.errorMessage.trim() ? ack.errorMessage :
            `Native ${nativeAction} effect was not verified`);
        }
        return {};
      }
      case 'listBans': {
        if (Object.values(args).some((value) => value !== undefined && value !== null)) {
          throw new Error('listBans accepts no options on this native build');
        }
        const rows = await this.banManager.list(requestId);
        return rows.map(({ id, reason, expiresAt }) => {
          // Native membership is fresh and authoritative; metadata is persisted
          // separately so Takaro's sync cannot erase its managed expiry/reason.
          return { player: { gameId: id, name: id, steamId: id, platformId: `steam:${id}` },
            reason, expiresAt };
        });
      }
      case 'executeConsoleCommand': {
        const output = await this.native.console(consoleCommand(args.command), requestId);
        if (!output || typeof output.success !== 'boolean' || typeof output.rawResult !== 'string' ||
            Buffer.byteLength(output.rawResult, 'utf8') > 32768 ||
            (output.errorMessage !== null && typeof output.errorMessage !== 'string') ||
            (output.success && output.errorMessage !== null) ||
            (!output.success && !output.errorMessage?.trim()) ||
            (typeof output.errorMessage === 'string' && Buffer.byteLength(output.errorMessage, 'utf8') > 1024)) {
          throw new Error('Native console output is invalid');
        }
        return { success: output.success, rawResult: output.rawResult, errorMessage: output.errorMessage };
      }
      case 'shutdown': {
        if (Object.values(args).some((value) => value !== undefined && value !== null)) {
          throw new Error('shutdown accepts no options on this native build');
        }
        const ack = await this.native.shutdown(requestId);
        if (ack?.success !== true) throw new Error('Native shutdown was not acknowledged');
        return {};
      }
      case 'sendMessage': {
        const message = args.message;
        if (typeof message !== 'string' || !message.trim()) throw new Error('sendMessage requires nonempty message');
        const recipient = messageRecipient(args);
        const ack = recipient
          ? await this.native.messageTo(recipient, message, requestId)
          : await this.native.message(message, requestId);
        if (ack?.success !== true) throw new Error(`Native chat ${recipient ? 'recipient' : 'broadcast'} was not acknowledged`);
        return {};
      }
      default:
        throw new Error(`ARK native action '${action}' is unavailable on this build`);
    }
  }
}
