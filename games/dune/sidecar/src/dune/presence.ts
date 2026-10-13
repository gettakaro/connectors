import { logger } from '../logger.js';
import type { GameEventType } from '../takaro/protocol.js';
import { mapPlayer } from './identity.js';
import { dimensionForMap } from './mapDimensions.js';
import { withDeathMessage } from './mapping.js';
import { isDead, isLeaving, isOnline } from './pg.js';
import type { DunePlayerRow, Position, TakaroPlayer } from './types.js';

export interface PresenceEmit {
  (type: GameEventType, data: Record<string, unknown>): void;
}

export interface PresenceOptions {
  /** Online roster read (`pg.onlinePlayers()`), or the plugin's list when it is present and healthy. */
  roster: () => Promise<DunePlayerRow[]>;
  emit: PresenceEmit;
  /** Players Takaro was last told are online; seeds the differ so a restart neither replays nor loses leaves. */
  initialOnline?: TakaroPlayer[];
  /** Persist the online set after every change. */
  save?: (players: TakaroPlayer[]) => void;
  intervalMs?: number;
  /**
   * A map transfer (Hagga Basin → Deep Desert, or any partition hand-off) takes the player out of `online_status`
   * for a few seconds. Reporting that as a disconnect/connect pair is the classic churn bug (VEIN F19), so a
   * disappearance is only believed after it has held for this long.
   */
  transferGraceMs?: number;
  /** Live position for a player, used to give a death event a `position`. */
  locationOf?: (row: DunePlayerRow) => Position | null | undefined;
  onError?: (err: Error) => void;
  now?: () => number;
  /**
   * Where a `life_state`-edge death goes. Defaults to `emit`, which is the plugin-absent behaviour.
   * With the plugin present this is the `DeathCoalescer`, which HOLDS the edge briefly so the plugin's
   * attributed death can replace it — the connector then reports exactly ONE `player-death` per death
   * instead of one from each source.
   */
  emitDeath?: (gameId: string, data: Record<string, unknown>) => void;
}

interface Tracked {
  player: TakaroPlayer;
  row: DunePlayerRow;
}

/**
 * Polls the roster and turns it into Takaro events.
 *
 * Out of process there is no join/leave callback — the only truth is `player_state.online_status` and `life_state`,
 * so presence is a differ over two snapshots. Everything it emits is therefore at most one poll interval late, which
 * is stated in the README rather than papered over.
 */
export class PresencePoller {
  private timer: NodeJS.Timeout | null = null;
  private running = false;
  private started = false;
  /** Players we have told Takaro are online, by gameId. */
  private readonly online = new Map<string, Tracked>();
  /** Players missing from the roster but still inside their transfer grace, by gameId → deadline. */
  private readonly leaving = new Map<string, { player: TakaroPlayer; row: DunePlayerRow; deadline: number }>();
  /** Last `life_state` seen per gameId, so only the *edge* into a dead state produces player-death. */
  private readonly lifeStates = new Map<string, string>();
  private lastError: string | null = null;

  constructor(private readonly options: PresenceOptions) {
    for (const player of options.initialOnline ?? []) {
      this.online.set(player.gameId, { player, row: { flsId: player.gameId, characterName: player.name } });
    }
  }

  onlinePlayers(): TakaroPlayer[] {
    return [...this.online.values()].map((t) => t.player);
  }

  /** Players inside the transfer-grace window: not reported as gone yet, not counted as online either. */
  pendingLeaves(): string[] {
    return [...this.leaving.keys()];
  }

  error(): string | null {
    return this.lastError;
  }

  start(): void {
    if (this.timer) return;
    this.timer = setInterval(() => void this.tick(), this.options.intervalMs ?? 5000);
    this.timer.unref?.();
    void this.tick();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  private async tick(): Promise<void> {
    if (this.running) return;
    this.running = true;
    try {
      await this.pollOnce();
      this.lastError = null;
    } catch (err) {
      this.lastError = err instanceof Error ? err.message : String(err);
      this.options.onError?.(err instanceof Error ? err : new Error(String(err)));
    } finally {
      this.running = false;
    }
  }

  /** One diff. Exposed so tests drive it deterministically instead of waiting on a timer. */
  async pollOnce(): Promise<void> {
    const now = this.options.now?.() ?? Date.now();
    // `online_status` is the three-state `PlayerConnectionStatus`. A `LoggingOut` row is dropped here rather than
    // treated as online: the player is on their way out, so they leave the online set exactly once, at this poll,
    // and the `Offline` row that follows a second later finds nothing left to remove — no duplicate disconnect.
    // A row whose status column does not exist on this build (empty/null) is trusted as online, because the only
    // reason it reached us is the SQL `online_status = 'Online'` filter or a plugin roster.
    const rows = (await this.options.roster()).filter(
      (r) => !isLeaving(r) && (isOnline(r) || r.onlineStatus == null || r.onlineStatus === ''),
    );
    const seen = new Map<string, Tracked>();
    for (const row of rows) {
      let player: TakaroPlayer;
      try {
        player = mapPlayer(row);
      } catch (err) {
        logger.debug(`Skipping unidentifiable roster row: ${(err as Error).message}`);
        continue;
      }
      seen.set(player.gameId, { player, row });
    }

    const firstTick = !this.started;
    this.started = true;

    // --- arrivals -----------------------------------------------------------
    for (const [gameId, tracked] of seen) {
      const pending = this.leaving.get(gameId);
      if (pending) {
        // They came back inside the grace window: this was a map transfer, not a disconnect. Nothing is emitted, on
        // purpose — Takaro was never told they left.
        this.leaving.delete(gameId);
        this.online.set(gameId, tracked);
        logger.debug(`${tracked.player.name} reappeared within the transfer grace; no connect/disconnect emitted`);
        continue;
      }
      if (this.online.has(gameId)) {
        this.online.set(gameId, tracked); // refresh the row (position, life state, map)
        continue;
      }
      this.online.set(gameId, tracked);
      // On the very first tick a player who is already in the persisted online set is NOT re-announced: they were
      // announced before the restart and replaying it would give Takaro a second connect with no leave in between.
      this.options.emit('player-connected', { player: tracked.player });
      logger.info(`player-connected ${tracked.player.name} (${gameId})${firstTick ? ' [first poll]' : ''}`);
    }

    // --- departures ---------------------------------------------------------
    for (const [gameId, tracked] of [...this.online]) {
      if (seen.has(gameId)) continue;
      this.online.delete(gameId);
      const grace = graceFor(tracked.row, this.options.transferGraceMs ?? 45_000, now);
      if (grace > 0) {
        this.leaving.set(gameId, { player: tracked.player, row: tracked.row, deadline: now + grace });
        logger.debug(`${tracked.player.name} left the online roster; holding ${grace}ms for a map transfer`);
      } else {
        this.emitDisconnect(tracked.player);
      }
    }
    for (const [gameId, pending] of [...this.leaving]) {
      if (pending.deadline > now) continue;
      this.leaving.delete(gameId);
      this.emitDisconnect(pending.player);
    }

    // --- deaths -------------------------------------------------------------
    for (const [gameId, tracked] of seen) {
      const state = (tracked.row.lifeState ?? '').trim();
      if (!state) continue;
      const previous = this.lifeStates.get(gameId);
      this.lifeStates.set(gameId, state);
      // The EDGE is the event. A player who stays dead across ten polls died once; and on the first poll after a
      // restart there is no previous state, so nothing is emitted — a corpse we find is not a death we witnessed.
      if (previous === undefined || previous === state || !isDead(tracked.row)) continue;
      const position = this.options.locationOf?.(tracked.row) ?? tracked.row.deathLocation ?? tracked.row.position ?? null;
      const data: Record<string, unknown> = { player: tracked.player };
      if (position) data.position = { ...position, dimension: position.dimension ?? dimensionForMap(tracked.row.worldMap, tracked.row.map) };
      const death = withDeathMessage(data, { lifeState: state });
      if (this.options.emitDeath) this.options.emitDeath(gameId, death);
      else this.options.emit('player-death', death);
      logger.info(`player-death ${tracked.player.name} (${gameId}) life_state ${previous} → ${state}`);
    }
    // Forget the life state of players who are gone, so a rejoin followed by a death is a fresh edge.
    for (const gameId of [...this.lifeStates.keys()]) {
      if (!seen.has(gameId) && !this.leaving.has(gameId)) this.lifeStates.delete(gameId);
    }

    this.options.save?.(this.onlinePlayers());
  }

  private emitDisconnect(player: TakaroPlayer): void {
    this.options.emit('player-disconnected', { player });
    logger.info(`player-disconnected ${player.name} (${player.gameId})`);
  }

  /** How long this row's disappearance is held before it counts as a leave. Exposed for the presence tests. */
  graceMsFor(row: DunePlayerRow, now = Date.now()): number {
    return graceFor(row, this.options.transferGraceMs ?? 45_000, now);
  }

  /** Marks a player online without emitting anything (used when another source already announced them). */
  note(player: TakaroPlayer, row: DunePlayerRow): void {
    this.online.set(player.gameId, { player, row });
    this.leaving.delete(player.gameId);
  }

  /**
   * Drops a player from the online set without emitting anything — the counterpart of `note` for a leave that
   * another source (the plugin's `player-disconnected` edge) has already reported to Takaro. Without it the next
   * poll would find them missing and emit a SECOND disconnect.
   */
  forget(gameId: string): void {
    this.online.delete(gameId);
    this.leaving.delete(gameId);
    this.lifeStates.delete(gameId);
  }
}

/**
 * The window a disappearance is held before it counts as a leave.
 *
 * `DUNE_TRANSFER_GRACE_SECONDS` is a guess; the game keeps the real number in
 * `player_state.reconnect_grace_period_end` (a UTC TIMESTAMP it sets on disconnect and resets on a map hand-off, per
 * `player_state_update` / `reset_all_players_from_server_ids_grace_period_and_logoff_timer`). When the row carries a
 * grace end in the future we wait until then instead, so the connector uses the server's own answer rather than ours.
 * The configured value stays the floor, so a build without the column — or an already-expired one — behaves exactly
 * as before.
 */
export function graceFor(row: DunePlayerRow, configuredMs: number, now: number): number {
  const raw = (row.reconnectGraceEnd ?? '').trim();
  if (!raw) return configuredMs;
  const end = Date.parse(raw.endsWith('Z') || /[+-]\d\d:?\d\d$/.test(raw) ? raw : `${raw}Z`);
  if (!Number.isFinite(end)) return configuredMs;
  return Math.max(configuredMs, end - now);
}
