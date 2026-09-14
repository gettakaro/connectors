import { EnshroudedAdapter, playerId } from './enshrouded/adapter.js';
import type { CursorStore } from './enshrouded/cursorStore.js';
import { EventPoller } from './enshrouded/eventPoller.js';
import { LogTailer } from './enshrouded/logTail.js';
import { mapPlayer } from './enshrouded/mapping.js';
import type { OnlineStore } from './enshrouded/onlineStore.js';
import { EnshroudedPluginClient } from './enshrouded/pluginClient.js';
import type { PluginHealth, Position, TakaroPlayer } from './enshrouded/types.js';
import { logger } from './logger.js';
import { createErrorResponse, createResponse, parseTakaroRequest, type GameEventType, type WsMessage } from './takaro/protocol.js';

/**
 * Plugin /health capability names (docs/API.md) that, when present and not "ok", mean the plugin cannot be trusted
 * for join/leave events: `logEvents` (log sink hook) feeds `players` (connect/disconnect correlation).
 */
export const CONNECTION_CAPABILITIES = ['logEvents', 'players'];
const TAILED_TYPES: ReadonlySet<string> = new Set(['player-connected', 'player-disconnected']);

/**
 * Takaro (hosted, observed 2026-09-13) calls getPlayerLocation while processing a player-connected /
 * player-disconnected gameEvent and only stores the event record if that request succeeds: a failed location reply
 * silently drops the event (same failure seen on the Zomboid connector when the player was already gone). A player who
 * just left, or a plugin build without location support (501), therefore loses every connect/disconnect record.
 * Inside this window after forwarding such an event we answer a failed location lookup with the last position the
 * plugin reported for that player, or the origin when none is known, so Takaro can store the event.
 */
export const EVENT_LOCATION_FALLBACK_MS = 60_000;
const ORIGIN: Position = { x: 0, y: 0, z: 0 };

export interface TakaroSink {
  send(message: WsMessage): boolean;
  sendGameEvent(type: GameEventType, data: unknown): boolean;
}

export interface BridgeOptions {
  plugin: EnshroudedPluginClient;
  takaro: TakaroSink;
  cursorStore: CursorStore;
  /** Persisted set of players Takaro was told are online; enables disconnect reconciliation after restarts. */
  onlineStore?: OnlineStore;
  logFile: string;
  logTailMode: 'auto' | 'always' | 'never';
  /** Forward `log` events: all lines, filtered (drop periodic stats/spam, default), or none. */
  logEvents?: 'all' | 'filtered' | 'none';
  pollIntervalMs?: number;
  healthCheckIntervalMs?: number;
  /** Exit the process after the plugin has been unreachable this long (0 = never). */
  exitAfterUnreachableMs?: number;
}

/**
 * Periodic/spammy server lines. Takaro rate-limits `log` per server (sustained 50 per 30 s), and the server prints a
 * multi-line stats block every 30 s plus bursts like "Could not prune enough replication states"; forwarding them
 * burns the budget and hides useful lines.
 */
const NOISY_LOG = [
  /^-{5,}/,
  /^Machines:$/,
  /^\s+m#\d+/,
  /^\[ecss\] Stats:/,
  /^\[Water\] /,
  /^Could not prune enough replication states/,
  /^\s*$/,
];

export function shouldForwardLog(mode: BridgeOptions['logEvents'], data: unknown): boolean {
  if (mode === 'none') return false;
  if (mode === 'all') return true;
  const msg = typeof (data as { msg?: unknown })?.msg === 'string' ? (data as { msg: string }).msg : '';
  return !NOISY_LOG.some((re) => re.test(msg));
}

export function shouldTailLog(mode: BridgeOptions['logTailMode'], health: PluginHealth | null): boolean {
  if (mode === 'always') return true;
  if (mode === 'never') return false;
  if (!health) return true;
  const status = String(health.status ?? '').toLowerCase();
  if (status !== 'ok' && status !== 'degraded') return true;
  const caps = health.capabilities ?? {};
  return CONNECTION_CAPABILITIES.some((name) => name in caps && caps[name] !== 'ok');
}

export class Bridge {
  readonly adapter: EnshroudedAdapter;
  readonly poller: EventPoller;
  readonly tailer: LogTailer;
  private healthTimer: NodeJS.Timeout | null = null;
  private lastHealth: PluginHealth | null = null;
  private tailActive = false;
  private unreachableSince: number | null = null;
  /** gameId/steamId -> expiry of the location fallback window opened by a forwarded connect/disconnect event. */
  private readonly eventWindows = new Map<string, number>();
  private readonly lastPositions = new Map<string, Position>();
  private readonly online = new Map<string, TakaroPlayer>();
  /** Reconcile online players at the next healthy plugin check (sidecar start, plugin restart). */
  private reconcilePending = true;

  constructor(private readonly options: BridgeOptions) {
    this.adapter = new EnshroudedAdapter(options.plugin);
    for (const p of options.onlineStore?.load() ?? []) this.online.set(p.gameId, p);
    this.poller = new EventPoller({
      getEvents: (since) => options.plugin.getEvents(since),
      emit: (type, data) => {
        if (type === 'log' && !shouldForwardLog(options.logEvents ?? 'filtered', data)) return true;
        const sent = options.takaro.sendGameEvent(type, data);
        if (sent) this.noteConnectionEvent(type, data);
        if (type !== 'log') logger.info(`Forwarded plugin ${type} (sent=${sent}): ${JSON.stringify(data)}`);
        return sent;
      },
      store: options.cursorStore,
      onRestart: () => {
        logger.info('Plugin reports a new server process (bootId/seq reset); reconciling online players');
        void this.reconcileOnline();
      },
      suppress: () => (this.tailActive ? TAILED_TYPES : new Set()),
      onError: (err) => logger.warn(`Event poll failed: ${err.message}`),
      intervalMs: options.pollIntervalMs,
    });
    this.tailer = new LogTailer({
      file: options.logFile,
      intervalMs: options.pollIntervalMs,
      onEvent: (event) => {
        logger.info(`Log tail ${event.type}: ${event.data.player.name} (${event.data.player.gameId})`);
        if (options.takaro.sendGameEvent(event.type, event.data)) this.noteConnectionEvent(event.type, event.data);
      },
      onError: (err) => logger.debug(`Log tail: ${err.message}`),
    });
  }

  async handleRequest(message: WsMessage): Promise<WsMessage | null> {
    if (!message.requestId) {
      logger.warn(`Ignoring Takaro request without requestId: ${JSON.stringify(message)}`);
      return null;
    }
    let reply: WsMessage;
    try {
      const request = parseTakaroRequest(message);
      logger.debug(`Takaro request ${request.action} ${JSON.stringify(request.args)}`);
      let payload: unknown;
      try {
        payload = await this.adapter.handleAction(request.action, request.args);
        if (request.action === 'getPlayerLocation') this.rememberPosition(request.args, payload);
      } catch (err) {
        const fallback = request.action === 'getPlayerLocation' ? this.eventLocationFallback(request.args) : null;
        if (!fallback) throw err;
        logger.warn(
          `getPlayerLocation failed during a connect/disconnect event window (${(err as Error).message}); ` +
            `answering ${JSON.stringify(fallback)} so Takaro stores the event`,
        );
        payload = fallback;
      }
      reply = createResponse(request.requestId, payload);
    } catch (err) {
      const text = err instanceof Error ? err.message : String(err);
      logger.warn(`Takaro request ${message.requestId} failed: ${text}`);
      reply = createErrorResponse(message.requestId, text);
    }
    this.options.takaro.send(reply);
    return reply;
  }

  /** Opens the location fallback window for the player of a forwarded player-connected/-disconnected event. */
  noteConnectionEvent(type: string, data: unknown, now = Date.now()): void {
    if (type !== 'player-connected' && type !== 'player-disconnected') return;
    const player = (data as { player?: Record<string, unknown> } | null)?.player ?? {};
    if (typeof player.gameId === 'string' && player.gameId) {
      if (type === 'player-connected') this.online.set(player.gameId, player as unknown as TakaroPlayer);
      else this.online.delete(player.gameId);
      this.saveOnline();
    }
    for (const key of [player.gameId, player.steamId]) {
      if (typeof key === 'string' && key) this.eventWindows.set(key, now + EVENT_LOCATION_FALLBACK_MS);
    }
  }

  eventLocationFallback(args: Record<string, unknown>, now = Date.now()): Position | null {
    let id: string;
    try {
      id = playerId(args);
    } catch {
      return null;
    }
    const expires = this.eventWindows.get(id);
    if (expires === undefined) return null;
    if (expires < now) {
      this.eventWindows.delete(id);
      return null;
    }
    return this.lastPositions.get(id) ?? ORIGIN;
  }

  private rememberPosition(args: Record<string, unknown>, payload: unknown): void {
    const pos = payload as Position | null;
    if (!pos || typeof pos.x !== 'number' || typeof pos.y !== 'number' || typeof pos.z !== 'number') return;
    try {
      this.lastPositions.set(playerId(args), { x: pos.x, y: pos.y, z: pos.z });
    } catch {
      /* no identifier: nothing to remember */
    }
  }

  /** Players Takaro was told are online and not yet told have left. */
  onlinePlayers(): TakaroPlayer[] {
    return [...this.online.values()];
  }

  /**
   * Sends player-disconnected for every player Takaro was told is online but the plugin no longer lists (e.g. the
   * game server restarted while they were connected, so no leave line was ever logged). Returns the players sent.
   */
  async reconcileOnline(): Promise<TakaroPlayer[]> {
    if (this.online.size === 0) return [];
    let current: Set<string>;
    try {
      current = new Set((await this.options.plugin.getPlayers()).map((p) => mapPlayer(p).gameId));
    } catch (err) {
      logger.warn(`Online reconciliation postponed, plugin getPlayers failed: ${(err as Error).message}`);
      this.reconcilePending = true;
      return [];
    }
    const gone: TakaroPlayer[] = [];
    for (const player of [...this.online.values()]) {
      if (current.has(player.gameId)) continue;
      const sent = this.options.takaro.sendGameEvent('player-disconnected', { player });
      logger.info(`Reconcile: ${player.name} (${player.gameId}) is no longer on the server; sent player-disconnected (sent=${sent})`);
      if (!sent) {
        this.reconcilePending = true;
        continue;
      }
      this.noteConnectionEvent('player-disconnected', { player });
      gone.push(player);
    }
    return gone;
  }

  private saveOnline(): void {
    try {
      this.options.onlineStore?.save([...this.online.values()]);
    } catch (err) {
      logger.warn(`Could not persist online players: ${(err as Error).message}`);
    }
  }

  isLogTailActive(): boolean {
    return this.tailActive;
  }

  pluginHealth(): PluginHealth | null {
    return this.lastHealth;
  }

  async refreshHealth(): Promise<void> {
    try {
      this.lastHealth = await this.options.plugin.health();
      this.unreachableSince = null;
      if (this.reconcilePending) {
        this.reconcilePending = false;
        await this.reconcileOnline();
      }
    } catch (err) {
      this.unreachableSince ??= Date.now();
      const limit = this.options.exitAfterUnreachableMs ?? 0;
      if (limit > 0 && Date.now() - this.unreachableSince >= limit) {
        // network_mode "service:enshrouded": when the game container restarts, this container keeps the old (dead)
        // network namespace. Exiting lets the restart policy re-attach us to the new one.
        logger.error(`Plugin unreachable for ${Math.round(limit / 1000)}s; exiting so the container restarts into the game's network namespace`);
        process.exit(1);
      }
      if (this.lastHealth !== null) logger.warn(`Plugin health failed: ${(err as Error).message}`);
      this.lastHealth = null;
    }
    const want = shouldTailLog(this.options.logTailMode, this.lastHealth);
    if (want && !this.tailActive) {
      logger.info(`Enabling log-tail fallback for connect/disconnect (${this.options.logFile})`);
      this.tailer.start();
    } else if (!want && this.tailActive) {
      logger.info('Plugin connection events healthy; disabling log-tail fallback');
      this.tailer.stop();
    }
    this.tailActive = want;
  }

  async startEvents(): Promise<void> {
    await this.refreshHealth();
    this.poller.start();
    if (!this.healthTimer) {
      this.healthTimer = setInterval(() => void this.refreshHealth(), this.options.healthCheckIntervalMs ?? 15000);
    }
  }

  stopEvents(): void {
    this.poller.stop();
    this.tailer.stop();
    this.tailActive = false;
    if (this.healthTimer) clearInterval(this.healthTimer);
    this.healthTimer = null;
  }
}
