import { logger } from '../logger.js';
import { asRecord } from '../takaro/protocol.js';
import { BanManager, expiryOf } from './bans.js';
import { Catalogue, NON_CARRYABLE_CODE_RE } from './catalogue.js';
import { runConsoleCommand, type CommandContext, type CommandResult } from './commands.js';
import type { GlobalMessageMode } from './config.js';
import * as gmCommands from './gm.js';
import { GmClient, GmCommandError, defaultExecRunner, type ExecRunner } from './gm.js';
import { gmPlayerId, mapPlayer, num, str, stripPlatform, type PlayerIdKind } from './identity.js';
import type { KnownPlayerStore } from './knownStore.js';
import { aggregateInventory, mapInventoryRow } from './mapping.js';
import { dimensionForMap } from './mapDimensions.js';
import { DunePg, isOnline } from './pg.js';
import { DunePluginClient } from './pluginClient.js';
import type { PluginJoin } from './pluginJoin.js';
import { DuneRmq } from './rmq.js';
import type { DunePlayerRow, Position, TakaroBan, TakaroEntity, TakaroItem, TakaroPlayer } from './types.js';

export class ActionError extends Error {}

/** An action this connector does not implement (protocol v1 error code `unsupported`). */
export class UnsupportedActionError extends ActionError {}

/**
 * Upper bound on how long `shutdown` will sit on the announced countdown before running the stop hook. The action
 * must finish inside `DUNE_EXEC_TIMEOUT_MS` (150 s on the rig), so a misconfigured 10-minute notice cannot wedge it.
 */
export const MAX_SHUTDOWN_NOTICE_SECONDS = 120;

type Args = Record<string, unknown>;

export interface AdapterOptions {
  pg: DunePg;
  gm: GmClient;
  catalogue: Catalogue;
  bans: BanManager;
  rmq?: DuneRmq | null;
  plugin?: DunePluginClient | null;
  /** Maps the plugin's own player refs to FLS ids (and back) through Postgres. See `pluginJoin.ts`. */
  pluginJoin?: PluginJoin | null;
  knownStore?: KnownPlayerStore;
  maxKnownPlayers?: number;
  senderName?: string;
  serverName?: string;
  gmPlayerIdKind?: PlayerIdKind;
  globalMessageMode?: GlobalMessageMode;
  /** How long a mutation's read-back may poll before the action answers `verified:false`. */
  verifyWindowMs?: number;
  /** Teleports are the slowest GM command measured on the live rig (19–20 s), so they get their own window. */
  teleportVerifyWindowMs?: number;
  /** Takaro's own request budget (`ws.requestTimeoutMs`, default 10 s); every read-back is capped under it. */
  takaroRequestTimeoutMs?: number;
  takaroRequestMarginMs?: number;
  verifyIntervalMs?: number;
  /** Argv run to stop the battlegroup after the shutdown notice. Empty = `shutdown` refuses with a clear message. */
  shutdownCmd?: string;
  shutdownNoticeSeconds?: number;
  /** Injectable sleep so the shutdown notice wait is testable without real time. */
  sleep?: (ms: number) => Promise<void>;
  execRunner?: ExecRunner;
  execTimeoutMs?: number;
  now?: () => number;
}

/**
 * The 17 Takaro generic-connector actions, mapped onto the Dune battlegroup's two real seams: Postgres for reads and
 * GM commands over RabbitMQ for writes.
 *
 * Two rules run through all of it:
 *  - Every optional argument is parsed for ABSENT, explicit JSON `null`, and wrong-type alike. Takaro modules send
 *    `{"dimension": null}` where the dashboard omits the key, and a connector that checks "is the key present" throws
 *    while telling the player it succeeded (wire gotcha, Zomboid #137).
 *  - A mutation answers only after reading the effect back. `success` from a publish means "RabbitMQ took the bytes",
 *    which is exactly the claim that made kick/give/teleport look like they worked for hours on VEIN while doing
 *    nothing. Where a read-back is impossible the answer says `verified:false` and why.
 */
export class DuneAdapter {
  private readonly known = new Map<string, TakaroPlayer>();
  private readonly options: AdapterOptions;

  private lastLocation: Record<string, unknown> | null = null;
  private readonly locationSourceCounts: Record<string, number> = {};

  constructor(options: AdapterOptions) {
    this.options = options;
    for (const player of options.knownStore?.load() ?? []) this.index(player);
  }

  private now(): number {
    return this.options.now?.() ?? Date.now();
  }

  senderName(override?: string | null): string {
    return override || this.options.senderName || this.options.serverName || 'Server';
  }

  /** What this deployment can actually do right now; mirrored onto `/health` and used by `testReachability`. */
  capabilities(): Record<string, unknown> {
    const plugin = this.options.plugin?.enabled() ?? false;
    return {
      gmPublisher: this.options.gm.publisherKind(),
      gmAuthToken: this.options.gm.hasToken(),
      chatConsumer: this.options.rmq?.chatConsumerBound() ?? false,
      rmqConnected: this.options.rmq?.connected() ?? false,
      pgProbed: this.options.pg.isProbed(),
      plugin,
      // Without the plugin these four are the documented gaps of the out-of-process design.
      livePlayerLocation: plugin ? 'plugin-then-chat-origin-then-last-saved' : 'chat-origin-or-last-saved',
      lastPlayerLocation: this.lastLocation,
      playerLocationSources: { ...this.locationSourceCounts },
      entityKilled: plugin ? 'plugin' : 'absent',
      deathAttribution: plugin ? 'plugin-then-life-state' : 'life-state-only',
      pluginJoin: this.options.pluginJoin?.status() ?? null,
      bans: 'connector-enforced',
      catalogue: this.options.catalogue.status(),
      shutdownHook: Boolean(this.options.shutdownCmd),
    };
  }

  async handleAction(action: string, args: Args): Promise<unknown> {
    try {
      return await this.dispatch(action, args);
    } catch (err) {
      if (err instanceof GmCommandError) throw new ActionError(`Dune connector cannot perform '${action}': ${err.message}`);
      throw err;
    }
  }

  private async dispatch(action: string, args: Args): Promise<unknown> {
    switch (action) {
      case 'testReachability':
        return this.testReachability();
      case 'getPlayers':
        return this.getPlayers();
      case 'getPlayer':
        return this.getPlayer(playerId(args));
      case 'getPlayerLocation':
        return this.getPlayerLocation(playerId(args));
      case 'getPlayerInventory':
        return this.getPlayerInventory(playerId(args));
      case 'giveItem':
        return this.giveItem(args);
      case 'listItems':
        return this.options.catalogue.listItems(str(args.search) ?? undefined);
      case 'listEntities':
        return this.listEntities();
      case 'listLocations':
        return this.options.pg.locations();
      case 'executeConsoleCommand':
        return this.executeConsoleCommand(args);
      case 'sendMessage':
        return this.sendMessage(args);
      case 'teleportPlayer':
        return this.teleportPlayer(args);
      case 'kickPlayer':
        return this.kickPlayer(args);
      case 'banPlayer':
        return this.banPlayer(args);
      case 'unbanPlayer':
        return this.unbanPlayer(args);
      case 'listBans':
        return this.listBans();
      case 'shutdown':
        return this.shutdown();
      default:
        throw new UnsupportedActionError(`Unknown Takaro action '${action}'`);
    }
  }

  // --- reads ---------------------------------------------------------------

  async testReachability(): Promise<{ connectable: boolean; reason: string | null }> {
    try {
      const pg = await this.options.pg.reachability();
      const problems: string[] = [];
      if (pg.reason) problems.push(pg.reason);
      if (this.options.rmq && !this.options.rmq.connected()) problems.push(`game RabbitMQ not connected (${this.options.rmq.error() ?? 'unknown'})`);
      if (!this.options.gm.hasToken()) problems.push('DUNE_GM_AUTH_TOKEN is unset, so no server command can be executed');
      // Postgres readiness is the load-bearing signal: without it nothing works. The other two degrade features.
      return { connectable: pg.connectable, reason: problems.length ? problems.join('; ') : null };
    } catch (err) {
      return { connectable: false, reason: err instanceof Error ? err.message : String(err) };
    }
  }

  async getPlayers(): Promise<TakaroPlayer[]> {
    const rows = await this.options.pg.onlinePlayers();
    return rows.filter(isOnline).map((row) => this.remember(mapPlayer(row)));
  }

  /**
   * Takaro calls this for players who are NOT online (running a `commandTrigger`, rendering a ban row, …) and rejects
   * every "no player" answer with a user-visible 400: `{}` fails IGamePlayer validation on `gameId`, and `null` and
   * an error frame both fail with "No payload provided but expected DTO: IGamePlayer". So this ALWAYS returns a real
   * record — the live row, the last-known one, or a minimal one synthesised from the identifier we were asked about.
   */
  async getPlayer(id: string): Promise<TakaroPlayer> {
    const row = await this.findRow(id).catch(() => undefined);
    if (row) {
      const player = this.remember(mapPlayer(row));
      return isOnline(row) ? player : { ...player, online: false };
    }
    const last = this.lastKnown(id);
    if (last) return { ...last, online: false };
    logger.warn(`getPlayer: no record for '${id}'; answering a minimal offline record built from the identifier`);
    return { gameId: stripPlatform(id), name: stripPlatform(id), online: false };
  }

  /**
   * Location, best source first:
   *  1. the native plugin's live pawn position — the only one that tracks a moving player;
   *  2. `m_OriginLocation` from that player's most recent chat message — live, but only as recent as their last line;
   *  3. `(actors.transform).location` — the last position the server PERSISTED, which the location audit showed can
   *     be a long way from where the player actually is.
   * Nothing is invented: when none of the three has an answer the action fails rather than reporting the origin.
   */
  async getPlayerLocation(id: string): Promise<Position> {
    const row = await this.findRow(id).catch(() => undefined);
    // Which map the player is on; protocol v1 draws a position only on the Dimension it names.
    const dimension = dimensionForMap(row?.worldMap, row?.map);
    const plugin = this.options.plugin;
    if (plugin?.enabled()) {
      // The plugin has never seen an FLS id, so the ref has to be resolved through the Postgres join
      // first. Passing the FLS id straight through would be a guaranteed 404 that looked like "the
      // plugin has no position", which is a different and much more misleading failure.
      const flsId = row?.flsId ?? stripPlatform(id);
      const joined = await this.options.pluginJoin?.refFor(flsId).catch(() => null);
      const ref = joined?.ref ?? null;
      if (ref) {
        try {
          const live = await plugin.getPlayerLocation(ref);
          // MEASURED on the live rig: while the pawn is being replaced (respawn, partition hand-off) the plugin
          // loses the pawn transform and answers `(0,0,0)` with `source:"playerState"`. That is the MAP ORIGIN, not
          // a position — and Takaro stored it as the player's location. The origin from the weak source is therefore
          // treated as "no live position" so the chain falls through to the chat-origin hint and the last saved
          // pawn transform, both of which are labelled for what they are.
          const originFromWeakSource = live && live.x === 0 && live.y === 0 && live.z === 0 && live.source !== 'pawn';
          if (originFromWeakSource) this.noteLocationSource(id, `plugin-origin-rejected:${live.source ?? 'unknown'}`);
          if (live && Number.isFinite(live.x) && !originFromWeakSource) {
            // `source` comes from the plugin and is always present: `pawn` is the live transform this
            // plugin exists for, `playerState` is a weaker fallback.
            this.noteLocationSource(id, `plugin:${live.source ?? 'unknown'}`, live.ageMs);
            return { x: live.x, y: live.y, z: live.z, dimension };
          }
        } catch (err) {
          logger.debug(`Plugin location unavailable for ${id} (ref ${ref}): ${(err as Error).message}`);
        }
      } else {
        logger.debug(`No plugin ref for ${flsId}: the plugin has not seen this player (PostLogin) or the join failed`);
      }
    }
    for (const key of [row?.flsId, row?.funcomId, stripPlatform(id)]) {
      const hint = key ? this.options.rmq?.originHint(key) : null;
      if (hint) {
        this.noteLocationSource(id, 'chat-origin');
        return { ...hint, dimension };
      }
    }
    if (row?.position) {
      // Labelled, not laundered: this is the last position the server PERSISTED, which the location
      // audit showed can be a long way from where the player actually is.
      this.noteLocationSource(id, 'postgres-last-saved');
      return { ...row.position, dimension };
    }
    this.noteLocationSource(id, 'none');
    throw new ActionError(
      `No location for '${id}': the plugin has no live pawn transform for them, the player has not chatted recently, and the database has no saved pawn position`,
    );
  }

  /**
   * Which of the three chain steps answered, for `/health` and the logs. Takaro's `getPlayerLocation`
   * DTO is a bare position, so the provenance cannot ride along in the response — and a silent chain
   * is how "the plugin is working" gets believed while every answer is actually a stale DB row.
   */
  private noteLocationSource(id: string, source: string, ageMs?: number): void {
    this.lastLocation = { id, source, ageMs: ageMs ?? null, at: new Date(this.now()).toISOString() };
    this.locationSourceCounts[source] = (this.locationSourceCounts[source] ?? 0) + 1;
    logger.debug(`getPlayerLocation(${id}) answered from ${source}${ageMs !== undefined ? ` (${ageMs} ms old)` : ''}`);
  }

  locationSources(): Record<string, unknown> {
    return { last: this.lastLocation, counts: { ...this.locationSourceCounts } };
  }

  async getPlayerInventory(id: string): Promise<TakaroItem[]> {
    const row = await this.findRow(id);
    if (!row) throw new ActionError(`Unknown player '${id}'`);
    const rows = await this.options.pg.inventory(row.flsId);
    // Belt and braces with `DUNE_INVENTORY_TYPES` (config.ts `REPORTED_INVENTORY_TYPES`): an emote is not a
    // possession, and Tester saw ten `Emote_*` rows in Takaro's inventory screen. The type filter is the primary
    // defence; this one holds even if a build moves emotes to another container or an operator widens the type list.
    const carried = rows.filter((r) => !NON_CARRYABLE_CODE_RE.test(String(r.templateId ?? '')));
    if (carried.length !== rows.length) {
      logger.debug(`getPlayerInventory: withheld ${rows.length - carried.length} non-carryable row(s) (emotes) for ${row.flsId}`);
    }
    return aggregateInventory(carried.map((r) => mapInventoryRow(r, (code) => this.options.catalogue.displayName(code))));
  }

  async listEntities(): Promise<TakaroEntity[]> {
    const fromFile = this.options.catalogue.listEntities();
    const plugin = this.options.plugin;
    if (!plugin?.enabled()) return fromFile;
    try {
      const rows = await plugin.getEntities();
      if (Array.isArray(rows) && rows.length) {
        const { mapEntity } = await import('./mapping.js');
        const merged = new Map(fromFile.map((e) => [e.code, e] as const));
        for (const raw of rows) {
          try {
            // The plugin flags a row whose only available name is the blueprint/class name. Those are dev names and
            // the catalogue gate forbids them, so they are dropped rather than shown to an operator as a creature.
            if (isClassNamed(raw)) continue;
            const entity = mapEntity(raw);
            // The file wins on `name`: it is the one carrying curated display names.
            if (!merged.has(entity.code)) merged.set(entity.code, entity);
          } catch {
            /* unidentifiable plugin row */
          }
        }
        return [...merged.values()];
      }
    } catch (err) {
      logger.debug(`Plugin /entities unavailable: ${(err as Error).message}`);
    }
    return fromFile;
  }

  // --- mutations -----------------------------------------------------------

  /**
   * `AddItemToInventory`, then read the inventory back.
   *
   * The read-back has a real caveat: `dune.items` is written by the server's periodic `SavePlayer`, not synchronously
   * on the grant, so a successful grant can be invisible in the DB for a while. `verified:false` here therefore means
   * "not observed within DUNE_VERIFY_WINDOW_MS" — not "did not happen" — and the payload says so instead of claiming
   * either outcome.
   */
  async giveItem(args: Args): Promise<Record<string, unknown>> {
    const id = playerId(args);
    const row = await this.requireRow(id);
    const code =
      str(args.item) ?? str(args.itemCode) ?? str(args.code) ?? str(asRecord(args.item).code) ?? fail("giveItem requires 'item'");
    const item = this.options.catalogue.resolveCode(code) ?? code;
    const amount = num(args.amount) ?? num(args.quantity) ?? 1;
    if (amount <= 0) fail('giveItem amount must be positive');
    // `quality` arrives as a string, a number or an explicit JSON null. Dune's grant command has no quality field at
    // all, so it is accepted and ignored rather than rejected.
    const before = await this.countItemEverywhere(row.flsId, item);
    await this.options.gm.send(
      gmCommands.addItemToInventory({ playerId: this.gmId(row, id), itemName: item, quantity: Math.trunc(amount) }),
      this.now(),
    );
    let after = before;
    const verified = await this.pollUntil(async () => {
      after = await this.countItemEverywhere(row.flsId, item);
      return after.total > before.total;
    });
    return {
      verified,
      item,
      amount: Math.trunc(amount),
      ...(verified ? { inventoryTypes: after.types, count: after.total } : {}),
      ...(verified
        ? {}
        : {
            reason:
              'the grant was published and accepted by the broker, but the item did not appear in the database within the verify window; Dune persists inventories periodically, so re-check with getPlayerInventory',
          }),
    };
  }

  /** `TeleportToExact`. `dimension` is always null/absent for a single-map battlegroup and is accepted and ignored. */
  async teleportPlayer(args: Args): Promise<Record<string, unknown>> {
    const id = playerId(args);
    const row = await this.requireRow(id);
    const x = num(args.x);
    const y = num(args.y);
    const z = num(args.z);
    if (x === null || y === null || z === null) fail('teleportPlayer requires numeric x, y and z');
    await this.options.gm.send(
      gmCommands.teleport({ playerId: this.gmId(row, id), x: x!, y: y!, z: z!, yaw: num(args.yaw) }, true),
      this.now(),
    );
    // Only the plugin can see a live position, so without it the move is unverifiable — stated, not assumed.
    if (!this.options.plugin?.enabled()) {
      return {
        verified: false,
        reason: 'teleport published; no live position source is available (native plugin absent) so the move could not be read back',
      };
    }
    // The read-back addresses the plugin by ITS ref, which only exists once the plugin has seen the
    // player through PostLogin. No ref means no live position, and that is reported as unverifiable
    // rather than as a failed move — the two are very different things for an operator.
    const joined = await this.options.pluginJoin?.refFor(row.flsId).catch(() => null);
    if (!joined) {
      return {
        verified: false,
        reason:
          'teleport published; the plugin has no ref for this player (it has not seen their PostLogin, or the '
          + 'Postgres join did not resolve), so the move could not be read back',
      };
    }
    // MEASURED on the live rig: `AddItemToInventory` runs ~1 s after the publish, but `TeleportToExact` took 19–20 s
    // to reach `Now running ServerCommand` twice in a row. The default 8 s window therefore reported `verified:false`
    // for teleports that did happen, so this read-back gets its own, longer window.
    // …but Takaro abandons a connector request after `ws.requestTimeoutMs` — **10 s** by default
    // (`app-connector/src/config.ts`, env `T_WEBSOCKET_REQUEST_TIMEOUT_MS`), after which it answers the API caller
    // `Request timed out after 10000ms : teleportPlayer` and IGNORES our late reply. A 30 s read-back therefore never
    // reaches anybody: the operator sees a timeout, which reads like "the teleport failed", when it did not.
    // So the window is capped just under Takaro's budget and the answer says the verification is still PENDING —
    // never silence, and never a `verified:false` that pretends the move was checked and found wanting.
    const budget = this.teleportReadBackMs();
    let sawLivePosition = false;
    const verified = await this.pollUntil(async () => {
      const pos = await this.options.plugin!.getPlayerLocation(joined.ref).catch(() => null);
      // (0,0,0) from the weak source is the map origin, not a position — see getPlayerLocation.
      if (!pos || (pos.x === 0 && pos.y === 0 && pos.z === 0 && pos.source !== 'pawn')) return false;
      sawLivePosition = true;
      return distance(pos, { x: x!, y: y!, z: z! }) < 500;
    }, budget.ms);
    if (verified) {
      this.noteLocationSource(id, 'plugin:pawn');
      return { verified: true };
    }
    // The read-back was cut short by Takaro's own request budget, not by evidence that the move failed. Say so.
    if (budget.cappedBy) {
      return {
        verified: false,
        pending: true,
        reason:
          `teleport published; the read-back is still PENDING — a live Dune \`TeleportToExact\` takes ~20 s to run, but `
          + `Takaro abandons a connector request after ${budget.takaroTimeoutMs} ms, so verification was cut off at `
          + `${budget.ms} ms. The move is very likely in progress: confirm with the GM milestone `
          + `\`Now running ServerCommand 'TeleportToExact'\` in the map-server log, or read the position again in a few `
          + `seconds. Raise T_WEBSOCKET_REQUEST_TIMEOUT_MS on the Takaro side to verify inline.`,
      };
    }
    return {
      verified: false,
      reason: sawLivePosition
        ? 'teleport published but the live position did not reach the target within the verify window'
        : 'teleport published; the plugin never answered a live pawn position for this player (it reported the map '
          + 'origin from its weaker playerState source), so the move could not be read back — check the GM milestone '
          + "`Now running ServerCommand 'TeleportToExact'` in the map-server log",
    };
  }

  /**
   * The teleport read-back window, capped so the reply still fits inside Takaro's request budget.
   *
   * `cappedBy` records that the configured window was the longer one — the caller uses it to answer "pending" instead
   * of "not verified", because those are very different statements to an operator.
   */
  teleportReadBackMs(): { ms: number; cappedBy: boolean; takaroTimeoutMs: number } {
    const configured = this.options.teleportVerifyWindowMs ?? 30_000;
    const takaroTimeoutMs = this.options.takaroRequestTimeoutMs ?? 10_000;
    if (takaroTimeoutMs <= 0) return { ms: configured, cappedBy: false, takaroTimeoutMs };
    const margin = this.options.takaroRequestMarginMs ?? 1_500;
    // Never below one verify interval: a cap that leaves no time at all would answer before the first poll.
    const cap = Math.max(this.options.verifyIntervalMs ?? 500, takaroTimeoutMs - margin);
    return { ms: Math.min(configured, cap), cappedBy: configured > cap, takaroTimeoutMs };
  }

  async kickPlayer(args: Args): Promise<Record<string, unknown>> {
    const id = playerId(args);
    const row = await this.requireRow(id);
    return this.kickRow(row, id, str(args.reason) ?? undefined);
  }

  private async kickRow(row: DunePlayerRow, id: string, reason?: string): Promise<Record<string, unknown>> {
    if (reason) logger.info(`Kicking ${row.characterName ?? row.flsId}: ${reason}`);
    await this.options.gm.send(gmCommands.kickPlayer(this.gmId(row, id)), this.now());
    const verified = await this.pollUntil(async () => {
      const fresh = await this.options.pg.findPlayer(row.flsId).catch(() => undefined);
      return !fresh || !isOnline(fresh);
    });
    return verified
      ? { verified: true }
      : { verified: false, reason: 'kick published but the player was still online at the end of the verify window' };
  }

  /**
   * A connector-owned ban: the record is stored, the player is kicked immediately, and the presence loop keeps
   * kicking them for as long as the ban stands. The game itself has no ban list, which the README states plainly.
   */
  async banPlayer(args: Args): Promise<Record<string, unknown>> {
    const id = playerId(args);
    const row = await this.findRow(id).catch(() => undefined);
    const player = row ? this.remember(mapPlayer(row)) : this.lastKnown(id);
    const gameId = row?.flsId ?? player?.gameId ?? stripPlatform(id);
    // `reason` and `expiresAt` both arrive as explicit JSON null from modules.
    const reason = str(args.reason);
    const expiresAt = expiryOf(args.expiresAt);
    this.options.bans.add(gameId, reason, expiresAt, player ?? { gameId, name: gameId });
    let kicked = false;
    if (row && isOnline(row)) {
      try {
        const result = await this.kickRow(row, id, reason ?? 'Banned');
        kicked = result.verified === true;
      } catch (err) {
        logger.warn(`Ban for ${gameId} stored, but the immediate kick failed: ${(err as Error).message}`);
      }
    }
    return { verified: true, enforcement: 'kick-on-sight', kicked, expiresAt };
  }

  async unbanPlayer(args: Args): Promise<Record<string, unknown>> {
    const id = playerId(args);
    const row = await this.findRow(id).catch(() => undefined);
    const gameId = row?.flsId ?? this.lastKnown(id)?.gameId ?? stripPlatform(id);
    const removed = this.options.bans.remove(gameId);
    return { verified: true, removed };
  }

  async listBans(): Promise<TakaroBan[]> {
    return this.options.bans.list();
  }

  /**
   * `sendMessage`. A recipient makes it a whisper (`chat.whispers` + a temporary bind of that player's own queue);
   * without one it is a server-wide message, delivered as chat, as an on-screen `ServiceBroadcast`, or both,
   * depending on `DUNE_GLOBAL_MESSAGE_MODE`.
   */
  async sendMessage(args: Args): Promise<Record<string, unknown>> {
    const message = str(args.message) ?? fail("sendMessage requires 'message'");
    const opts = asRecord(args.opts);
    const recipient = asRecord(opts.recipient);
    const recipientId = str(recipient.gameId) ?? str(recipient.steamId) ?? str(recipient.platformId) ?? str(args.recipientGameId);
    const sender = this.senderName(str(opts.senderNameOverride));

    if (recipientId) {
      const row = await this.requireRow(recipientId);
      if (!this.options.rmq) fail('sendMessage needs the game RabbitMQ connection (DUNE_RMQ_URL)');
      const result = await this.options.rmq.whisper(whisperKey(row), message, row.characterName ?? undefined);
      return { delivered: result.ok, channel: 'whisper', routingKey: result.routingKey, sender };
    }

    const mode = this.options.globalMessageMode ?? 'chat';
    const out: Record<string, unknown> = { channel: 'global', mode, sender };
    let delivered = false;
    if (mode === 'chat' || mode === 'both') {
      if (!this.options.rmq) fail('sendMessage needs the game RabbitMQ connection (DUNE_RMQ_URL)');
      const result = await this.options.rmq.broadcastChat(message);
      out.chat = result.ok;
      // The routing keys are the whole story for `chat.map`: it is a DIRECT exchange, so a publish on a key nobody
      // bound is discarded in silence. Reporting them makes `delivered:true` checkable instead of a claim.
      if (result.routingKeys) out.routingKeys = result.routingKeys;
      delivered ||= result.ok;
      if (!result.ok) {
        // `chat.map` is declared by the MAP SERVER, not by text-router: on a battlegroup whose map process has not
        // joined the broker it simply does not exist, and a publish into it is discarded in silence. Rather than
        // reporting a success nobody saw, fan the line out as a whisper to each online player, and — in `both`
        // mode — let the `ServiceBroadcast` below carry it too.
        out.chatRefused = result.reason ?? 'broadcast exchange unavailable';
        const fanout = await this.whisperFanout(message);
        out.fanout = fanout;
        delivered ||= fanout.delivered > 0;
      }
    }
    if (mode === 'broadcast' || mode === 'both') {
      await this.options.gm.send(gmCommands.serviceBroadcast({ title: sender, body: message }), this.now());
      out.broadcast = true;
      delivered = true;
    }
    out.delivered = delivered;
    return out;
  }

  /**
   * Last resort for a global chat line: one whisper per online player. It is O(players) publishes and it is visible
   * only to whoever is online at that instant, so it is a fallback and never the default — but it is a real
   * delivery, which a discarded `chat.map` publish is not. Failures are counted, not thrown: one unreachable player
   * must not sink the announcement for everyone else.
   */
  private async whisperFanout(message: string): Promise<{ delivered: number; attempted: number; failed: number }> {
    const rmq = this.options.rmq;
    if (!rmq) return { delivered: 0, attempted: 0, failed: 0 };
    let rows: DunePlayerRow[] = [];
    try {
      rows = await this.options.pg.onlinePlayers();
    } catch (err) {
      logger.warn(`Global-message fan-out could not read the roster: ${(err as Error).message}`);
      return { delivered: 0, attempted: 0, failed: 0 };
    }
    let delivered = 0;
    let failed = 0;
    for (const row of rows) {
      if (!row.flsId) continue;
      try {
        const result = await rmq.whisper(whisperKey(row), message, row.characterName ?? undefined);
        if (result.ok) delivered += 1;
        else failed += 1;
      } catch (err) {
        failed += 1;
        logger.debug(`Global-message fan-out to ${row.flsId} failed: ${(err as Error).message}`);
      }
    }
    return { delivered, attempted: rows.length, failed };
  }

  /**
   * Shutdown: the in-game notice first (so players are not dropped without warning), then the configured stop hook.
   * Without a hook this REFUSES instead of returning a quiet success — the GM `ServerShutdown` broadcast is a
   * countdown notice, not a stop, and nothing in the battlegroup halts the map process by itself.
   *
   * The notice has to be TRUE (L6d): the first version announced "going down in 60 s" and then stopped the container
   * in the same tick, so the countdown on the player's screen was decoration.
   *
   * It also cannot be answered synchronously. Takaro's request budget is ~10 s (measured: a slow action comes back as
   * `400 BadRequestError "Request timed out after 10000ms : shutdown"`), while stopping the map server takes MINUTES
   * — the UE world save alone ran past 120 s on the dev rig, even with nobody in world. So `shutdown` ALWAYS:
   *
   *   1. broadcasts the `ServerShutdown` countdown — `shutdownNoticeSeconds` when players are in world, 1 s when the
   *      server is empty, because then there is nobody to warn;
   *   2. SCHEDULES the stop hook for when that countdown expires;
   *   3. answers immediately with `verified: false, scheduled: true, stopsAt`.
   *
   * `verified` is deliberately false: nothing has stopped yet, and this connector does not claim effects it has not
   * read back. The hook's own success or failure is logged when it completes.
   */
  async shutdown(): Promise<Record<string, unknown>> {
    const configured = this.options.shutdownNoticeSeconds ?? 60;
    const online = await this.getPlayers().catch(() => []);
    const lead = online.length > 0 ? Math.min(Math.max(1, configured), MAX_SHUTDOWN_NOTICE_SECONDS) : 1;
    await this.options.gm.send(gmCommands.serverShutdownBroadcast({ leadSeconds: lead, shutdownType: 'Maintenance' }), this.now());
    const cmd = (this.options.shutdownCmd ?? '').trim();
    if (!cmd) {
      throw new ActionError(
        `Shutdown notice broadcast (${lead}s), but this connector has no way to stop the battlegroup: set DUNE_SHUTDOWN_CMD ` +
          `to the command that stops the map container(s), e.g. 'docker stop dune-survival-1'. ` +
          `Dune's ServerShutdown server command only shows a countdown; it does not stop the process.`,
      );
    }
    const argv = parseArgv(cmd);
    const runStop = async (): Promise<string> => {
      const runner = this.options.execRunner ?? defaultExecRunner;
      const result = await runner(argv, '', this.options.execTimeoutMs ?? 20000);
      const output = `${result.stdout}${result.stderr}`.trim();
      if (result.code !== 0) throw new ActionError(`Shutdown hook '${argv[0]}' exited ${result.code}: ${output}`);
      return output;
    };

    const sleep = this.options.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
    logger.info(
      `Shutdown notice broadcast (${lead}s, ${online.length} player(s) in world); the stop hook runs when it expires`,
    );
    void sleep(lead * 1000)
      .then(runStop)
      .then((output) => logger.info(`Shutdown hook '${argv[0]}' completed after the ${lead}s notice: ${output}`))
      .catch((err: unknown) => logger.error(`Scheduled shutdown hook failed: ${(err as Error).message}`));
    return {
      verified: false,
      scheduled: true,
      noticeSeconds: lead,
      onlinePlayers: online.length,
      stopsAt: new Date(this.now() + lead * 1000).toISOString(),
      hook: argv[0],
      reason:
        'the shutdown countdown is being honoured and stopping the map server takes minutes, so this answers ' +
        'before the server has actually stopped',
    };
  }

  async executeConsoleCommand(args: Args): Promise<CommandResult> {
    // Even "no command at all" answers a CommandOutput payload: Takaro turns an error frame for this action into a
    // user-visible 400 "the gameserver responded with bad data".
    const command = str(args.command) ?? '';
    return runConsoleCommand(command, this.commandContext());
  }

  private commandContext(): CommandContext {
    return {
      players: () => this.getPlayers(),
      say: async (msg) => JSON.stringify(await this.sendMessage({ message: msg })),
      whisper: async (id, msg) => JSON.stringify(await this.sendMessage({ message: msg, opts: { recipient: { gameId: id } } })),
      broadcast: async (title, body) => {
        await this.options.gm.send(gmCommands.serviceBroadcast({ title, body }), this.now());
        return `broadcast '${title}' published`;
      },
      give: async (id, item, quantity) => JSON.stringify(await this.giveItem({ gameId: id, item, amount: quantity })),
      teleport: async (id, x, y, z) => JSON.stringify(await this.teleportPlayer({ gameId: id, x, y, z })),
      kick: async (id, reason) => JSON.stringify(await this.kickPlayer({ gameId: id, reason })),
      ban: async (id, reason, expiresAt) => JSON.stringify(await this.banPlayer({ gameId: id, reason, expiresAt })),
      unban: async (id) => JSON.stringify(await this.unbanPlayer({ gameId: id })),
      listBans: () => this.listBans(),
      spawnVehicle: async (id, className, templateName, x, y, z) => {
        const row = await this.requireRow(id);
        await this.options.gm.send(
          gmCommands.spawnVehicleAt({ playerId: this.gmId(row, id), className, templateName, x, y, z }),
          this.now(),
        );
        return `SpawnVehicleAt ${className}/${templateName} published for ${row.characterName ?? row.flsId}`;
      },
      shutdown: async () => JSON.stringify(await this.shutdown()),
      gm: async (command, payload) => {
        await this.options.gm.send(gmCommands.passthrough(command, payload), this.now());
        return `${command} published`;
      },
    };
  }

  // --- helpers -------------------------------------------------------------

  /** Which id the GM `PlayerId` field carries; configurable because community tools disagree (settled on the rig). */
  private gmId(row: DunePlayerRow, fallback: string): string {
    return gmPlayerId(row, this.options.gmPlayerIdKind ?? 'fls') ?? stripPlatform(fallback);
  }

  async findRow(id: string): Promise<DunePlayerRow | undefined> {
    const row = (await this.options.pg.findPlayer(id)) ?? (await this.options.pg.findPlayer(stripPlatform(id)));
    if (row) this.remember(mapPlayer(row));
    return row;
  }

  private async requireRow(id: string): Promise<DunePlayerRow> {
    const row = await this.findRow(id);
    if (!row) throw new ActionError(`Unknown player '${id}': no row in player_state matches that id or character name`);
    return row;
  }

  private async countItem(flsId: string, code: string): Promise<number> {
    return (await this.countItemEverywhere(flsId, code)).total;
  }

  /**
   * How many of `code` the player holds across EVERY inventory their pawn owns, and in which `inventory_type`s.
   *
   * `getPlayerInventory` deliberately shows only the display containers, but the game puts a granted item wherever it
   * belongs — a contract/quest item lands in `inventory_type = 29`, which is not one of them. Verifying a grant
   * against the display set alone reported `verified:false` for grants that had plainly succeeded, so the read-back
   * looks everywhere and says where the item actually landed.
   */
  private async countItemEverywhere(flsId: string, code: string): Promise<{ total: number; types: number[] }> {
    const rows = await this.options.pg.inventory(flsId, { allTypes: true }).catch(() => []);
    const mine = rows.filter((r) => r.templateId.toLowerCase() === code.toLowerCase());
    const types = [...new Set(mine.map((r) => r.inventoryType).filter((t): t is number => t !== null && t !== undefined))].sort(
      (a, b) => a - b,
    );
    return { total: mine.reduce((sum, r) => sum + (r.stackSize || 0), 0), types };
  }

  /** Polls a predicate until it holds or the verify window runs out. Returns whether it was OBSERVED, never assumed. */
  private async pollUntil(check: () => Promise<boolean>, windowMs?: number): Promise<boolean> {
    const window = windowMs ?? this.options.verifyWindowMs ?? 8000;
    const step = Math.max(50, this.options.verifyIntervalMs ?? 500);
    const deadline = this.now() + window;
    for (;;) {
      if (await check().catch(() => false)) return true;
      if (this.now() >= deadline) return false;
      await sleep(Math.min(step, Math.max(0, deadline - this.now())));
    }
  }

  remember(player: TakaroPlayer): TakaroPlayer {
    if (this.index(player)) this.saveKnown();
    return player;
  }

  private index(player: TakaroPlayer): boolean {
    if (!player?.gameId) return false;
    const copy = { ...player };
    delete copy.online;
    const before = this.known.get(player.gameId.toLowerCase());
    for (const key of [player.gameId, player.steamId, player.platformId, player.name]) {
      if (typeof key === 'string' && key) this.known.set(key.toLowerCase(), copy);
    }
    return JSON.stringify(before) !== JSON.stringify(copy);
  }

  knownPlayers(): TakaroPlayer[] {
    const byGameId = new Map<string, TakaroPlayer>();
    for (const p of this.known.values()) byGameId.set(p.gameId.toLowerCase(), p);
    return [...byGameId.values()];
  }

  lastKnown(id: string): TakaroPlayer | undefined {
    return this.known.get(id.toLowerCase()) ?? this.known.get(stripPlatform(id).toLowerCase());
  }

  private saveKnown(): void {
    if (!this.options.knownStore) return;
    const max = this.options.maxKnownPlayers ?? 500;
    const players = this.knownPlayers();
    try {
      this.options.knownStore.save(players.length > max ? players.slice(players.length - max) : players);
    } catch (err) {
      logger.warn(`Could not persist known players: ${(err as Error).message}`);
    }
  }
}

/** Accepts flat `{gameId}` / `{steamId}` / `{platformId}` or nested `{player:{…}}` / `{playerRef:{…}}`. */
export function playerId(args: Args): string {
  for (const source of [args, asRecord(args.player), asRecord(args.playerRef)]) {
    const id = str(source.gameId) ?? str(source.steamId) ?? str(source.platformId);
    if (id) return id;
  }
  return fail('Expected a player identifier (gameId, or player.gameId)');
}

/**
 * The `chat.whispers` routing key for a player.
 *
 * MEASURED on the live rig: the client binds its own queue to the direct exchange `chat.whispers` under its
 * **funcom id** (`Tester#41350`), not under its FLS id. The FLS id is kept as the fallback for a row that has no
 * funcom id yet, because a wrong key is silently discarded by a direct exchange and looks exactly like a mute.
 */
function whisperKey(row: DunePlayerRow): string {
  return str(row.funcomId) ?? row.flsId;
}

/** A plugin entity row whose name is a blueprint/class id (`BP_IdleCivilian_C`, `DT_…`) rather than a display name. */
function isClassNamed(raw: unknown): boolean {
  const row = asRecord(raw);
  if (row.nameIsClassName === true) return true;
  const name = str(row.name) ?? str(row.code) ?? '';
  return /^(BP_|DT_|SK_|ABP_)/.test(name) || /_C$/.test(name);
}

function distance(a: Position, b: Position): number {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

/** Splits a configured command into argv, honouring quotes. No shell is involved at any point. */
export function parseArgv(command: string): string[] {
  const trimmed = command.trim();
  if (trimmed.startsWith('[')) {
    const parsed: unknown = JSON.parse(trimmed);
    if (!Array.isArray(parsed) || !parsed.every((v) => typeof v === 'string') || !parsed.length) {
      throw new ActionError('DUNE_SHUTDOWN_CMD as JSON must be a non-empty array of strings');
    }
    return parsed as string[];
  }
  const out: string[] = [];
  const re = /"([^"]*)"|'([^']*)'|(\S+)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(trimmed)) !== null) out.push(m[1] ?? m[2] ?? m[3]);
  if (!out.length) throw new ActionError('DUNE_SHUTDOWN_CMD is empty');
  return out;
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function fail(message: string): never {
  throw new ActionError(message);
}
