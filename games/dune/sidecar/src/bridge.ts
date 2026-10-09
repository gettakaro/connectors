import { DuneAdapter, playerId } from './dune/adapter.js';
import type { CursorStore } from './dune/cursorStore.js';
import { EventPoller } from './dune/eventPoller.js';
import type { OnlineStore } from './dune/onlineStore.js';
import type { Position, TakaroPlayer } from './dune/types.js';
import { logger } from './logger.js';
import { sanitizeGameEvent } from './takaro/eventWhitelist.js';
import { createErrorResponse, createResponse, parseTakaroRequest, type GameEventType, type WsMessage } from './takaro/protocol.js';

/**
 * Takaro (hosted) calls `getPlayerLocation` while processing a `player-connected` / `player-disconnected` gameEvent
 * and only stores the event record if that request succeeds. Inside this window after forwarding such an event a
 * failed location lookup is answered with the last position we know for that player, or the origin when none is known
 * — otherwise a perfectly good connect event is silently dropped by Takaro.
 */
export const EVENT_LOCATION_FALLBACK_MS = 60_000;

/**
 * How many undelivered gameEvents the sidecar keeps while Takaro is unreachable. At ~1 event/s of real traffic this
 * covers well over an hour of outage; beyond it the OLDEST events are dropped (and their cursor released) so the
 * sidecar never grows without bound.
 */
export const MAX_PENDING_EVENTS = 5000;
const ORIGIN: Position = { x: 0, y: 0, z: 0 };

/**
 * How old a queued `chat-message` may be when Takaro comes back. A chat line is only interesting while the
 * conversation is alive: replaying an hour of backlog floods the Discord bridge and every chat-hook module with
 * messages nobody can answer. Older lines are DROPPED and counted (`droppedStaleChat`) rather than delivered.
 */
export const DEFAULT_OUTAGE_CHAT_MAX_AGE_MS = 600_000;

/** A gameEvent that has not reached Takaro yet. `seq` is the plugin ring-buffer seq (absent for every other source). */
export interface PendingEvent {
  type: GameEventType;
  data: unknown;
  seq?: number;
  /** When the event was produced (ms epoch). Used for the chat max-age drop on reconnect. */
  at?: number;
  /** Sink send id, set once the event has been written to the socket but not yet proven delivered. */
  sendId?: number;
}

export interface TakaroSink {
  send(message: WsMessage): boolean;
  sendGameEvent(type: GameEventType, data: unknown): boolean;
  /** Id of the last gameEvent written; present only on sinks that can PROVE delivery (see onConfirmed). */
  lastSendId?(): number;
  /** Registers a callback fired when every gameEvent up to `sendId` is proven to have reached Takaro. */
  onConfirmed?(cb: (sendId: number) => void): void;
}

/** Anything that produces events: the presence poller, the chat consumer, the log tail, the plugin event poller. */
export interface EventSource {
  readonly name: string;
  start(): void | Promise<void>;
  stop(): void;
}

export interface BridgeOptions {
  adapter: DuneAdapter;
  takaro: TakaroSink;
  /** Persisted cursor for the optional plugin `/events` ring buffer. */
  cursorStore?: CursorStore;
  /** Persisted set of players Takaro was told are online; enables disconnect reconciliation after restarts. */
  onlineStore?: OnlineStore;
  /** Optional plugin event poller; when absent the bridge simply has no seq-carrying source. */
  pluginEvents?: EventPoller;
  /**
   * Event sources. They run for the WHOLE PROCESS LIFETIME — started by `startSources()` at boot and stopped only on
   * shutdown. They are deliberately NOT tied to the Takaro socket: the game-side duties they carry (presence, so ban
   * enforcement has an online set; chat consumption; the plugin ring drain) must keep working through a Takaro
   * outage, and only DELIVERY to Takaro waits for the socket. Tying them to `identified` is what let a banned player
   * rejoin during the 2026-09-21 registration-token outage.
   */
  sources?: EventSource[];
  /** The current online set, for presence reconciliation when Takaro comes back (`presence.onlinePlayers()`). */
  currentOnline?: () => TakaroPlayer[];
  /** Max age of a queued `chat-message` at reconnect; older lines are dropped. */
  outageChatMaxAgeMs?: number;
  now?: () => number;
  /** Probe for the connector's hard dependencies (Postgres). */
  dependenciesOk?: () => Promise<boolean>;
  /**
   * Exit the process after the hard dependencies have been unreachable this long (0 = never). Exiting lets the
   * container's `restart: unless-stopped` policy re-create the sidecar — which is how it re-attaches after the
   * battlegroup was re-created underneath it.
   */
  exitAfterDependencyLossMs?: number;
  healthCheckIntervalMs?: number;
  /** How often a congested socket is retried while events are queued (default 1000 ms). */
  drainRetryMs?: number;
  /** Called on every presence tick with the current online set (ban enforcement). */
  onOnlineTick?: (players: TakaroPlayer[]) => void | Promise<void>;
}

export class Bridge {
  readonly adapter: DuneAdapter;
  private healthTimer: NodeJS.Timeout | null = null;
  private drainTimer: NodeJS.Timeout | null = null;
  private sourcesActive = false;
  private watchAlways = false;
  private unreachableSince: number | null = null;
  private dependencyState: { ok: boolean; error: string | null } = { ok: true, error: null };
  /** gameId/steamId/platformId -> expiry of the location fallback window opened by a connect/disconnect event. */
  private readonly eventWindows = new Map<string, number>();
  private readonly lastPositions = new Map<string, Position>();
  private readonly online = new Map<string, TakaroPlayer>();
  /** gameEvents produced while Takaro was unreachable, oldest first; flushed in order after the next identify. */
  private readonly pendingEvents: PendingEvent[] = [];
  /**
   * gameEvents written to the socket but not yet PROVEN delivered, oldest first. The persisted cursor never advances
   * past this window, and on a disconnect the whole window goes back to the front of `pendingEvents`.
   *
   * Why: `ws.send()` succeeding proves only that the LOCAL kernel took the bytes. During an egress outage the socket
   * stays "open" for minutes while nothing reaches Takaro, and a cursor advanced on the send alone loses every event
   * in that gap for good.
   */
  private readonly unconfirmedEvents: PendingEvent[] = [];
  private readonly confirms: boolean;
  private droppedEvents = 0;
  /** Chat lines dropped at reconnect because they were older than `outageChatMaxAgeMs`. */
  private droppedStaleChat = 0;
  /** Queued connect/disconnect events collapsed into one reconciliation at reconnect. */
  private collapsedPresence = 0;
  /** Presence events emitted BY the reconciliation (so an operator can tell them from live ones). */
  private reconciled = { connected: 0, disconnected: 0 };
  private takaroUpSince: number | null = null;
  private takaroDownSince: number | null = null;
  /** Non-DTO keys the whitelist chokepoint removed before sending. Non-zero = a source is building the wrong payload. */
  private strippedFields = 0;
  /** `{"type":"error"}` frames Takaro sent us, with the last reason. A rejected event is LOST, so this must be visible. */
  private rejected = { count: 0, lastReason: null as string | null, lastAt: null as string | null };

  /**
   * Records a Takaro `error` frame. Takaro sends no correlation id with it, so the sidecar cannot know WHICH event was
   * thrown away and therefore cannot re-send it — which is precisely why this is surfaced on `/health` instead of
   * being logged once and forgotten: a non-zero count means events are being dropped by validation, and the fix is
   * always in the payload shape (see `takaro/eventWhitelist.ts`), never a retry.
   */
  noteTakaroError(reason: string): void {
    this.rejected.count += 1;
    this.rejected.lastReason = reason.length > 500 ? `${reason.slice(0, 497)}…` : reason;
    this.rejected.lastAt = new Date(this.now()).toISOString();
    logger.error(`Takaro REJECTED a frame (${this.rejected.count} so far); the event is lost, not retried: ${this.rejected.lastReason}`);
  }

  takaroRejections(): { count: number; lastReason: string | null; lastAt: string | null } {
    return { ...this.rejected };
  }

  constructor(private readonly options: BridgeOptions) {
    this.adapter = options.adapter;
    this.confirms = typeof options.takaro.lastSendId === 'function' && typeof options.takaro.onConfirmed === 'function';
    if (this.confirms) options.takaro.onConfirmed?.((sendId) => this.confirmUpTo(sendId));
    for (const p of options.onlineStore?.load() ?? []) this.online.set(p.gameId, p);
  }

  /** Players the sidecar has told Takaro are online and not yet told have left. */
  onlinePlayers(): TakaroPlayer[] {
    return [...this.online.values()];
  }

  pending(): PendingEvent[] {
    return this.pendingEvents.map((e) => ({ ...e }));
  }

  unconfirmed(): PendingEvent[] {
    return this.unconfirmedEvents.map((e) => ({ ...e }));
  }

  dropped(): number {
    return this.droppedEvents;
  }

  /** Everything an operator needs to judge an outage, on `/health`. */
  outage(): Record<string, unknown> {
    return {
      pendingEvents: this.pendingEvents.length,
      unconfirmedEvents: this.unconfirmedEvents.length,
      maxPendingEvents: MAX_PENDING_EVENTS,
      droppedEvents: this.droppedEvents,
      droppedStaleChat: this.droppedStaleChat,
      collapsedPresence: this.collapsedPresence,
      reconciled: { ...this.reconciled },
      chatMaxAgeMs: this.options.outageChatMaxAgeMs ?? DEFAULT_OUTAGE_CHAT_MAX_AGE_MS,
      strippedFields: this.strippedFields,
      takaroUpSince: this.takaroUpSince,
      takaroDownSince: this.takaroDownSince,
    };
  }

  dependencies(): { ok: boolean; error: string | null } {
    return { ...this.dependencyState };
  }

  // --- event ingress -------------------------------------------------------

  /**
   * The single entry point for every event source. Returns `true` when the event was handed to an open, identified
   * socket, `'queued'` when it is being held (either for Takaro to come back, or for delivery to be proven), and
   * `false` when it could not be accepted at all.
   */
  emit(type: GameEventType, rawData: unknown, seq?: number): boolean | 'queued' {
    // Takaro validates inbound gameEvents with `forbidNonWhitelisted: true`, so ONE extra key destroys the whole
    // event (measured: an `entity-killed` lost to `entityCode … whitelistValidation`). Every event passes through
    // here, so this is the one place that can guarantee it, and what was stripped is logged rather than hidden.
    const { data, removed } = sanitizeGameEvent(type, rawData);
    if (removed.length) {
      logger.debug(`Stripped ${removed.length} non-DTO field(s) from ${type} before sending: ${removed.join(', ')}`);
      this.strippedFields += removed.length;
    }
    // Events queued while the socket was congested go out first: sending past them would reorder delivery, and a
    // queue that only drained on a reconnect stayed stuck behind an open socket (a shutdown log burst once parked a
    // player-disconnected there indefinitely).
    if (this.pendingEvents.length && this.takaroDownSince === null) this.drainPending();
    const sent = !this.pendingEvents.length && this.options.takaro.sendGameEvent(type, data);
    if (!sent) {
      // `'queued'` — NOT `false` — is the honest answer once the event is safely in the outage queue: it tells the
      // plugin poller to advance its SCAN position (so the same seq is not read and queued again on every tick, which
      // used to fill the queue with duplicates of one event during an outage) while leaving the PERSISTED cursor
      // behind the hole, so a sidecar restart replays it rather than losing it. `false` is reserved for "this event
      // was not accepted at all", i.e. the queue was full and this very event is the one that got dropped.
      const accepted = this.queueEvent({ type, data, seq, at: this.now() });
      if (this.takaroDownSince === null) this.scheduleDrain();
      return accepted ? 'queued' : false;
    }
    this.noteConnectionEvent(type, data);
    if (type !== 'log') logger.debug(`Forwarded ${type}: ${JSON.stringify(data)}`);
    if (!this.confirms) return true;
    this.trackUnconfirmed({ type, data, seq });
    return 'queued';
  }

  /** Opens the location fallback window and keeps the online set / known cache in step with what Takaro was told. */
  noteConnectionEvent(type: string, data: unknown, now = Date.now()): void {
    if (type !== 'player-connected' && type !== 'player-disconnected') return;
    const player = ((data as { player?: Record<string, unknown> } | null)?.player ?? {}) as Record<string, unknown>;
    if (typeof player.gameId === 'string' && player.gameId) {
      // Keep the record so a getPlayer for this player after they leave still answers a valid IGamePlayer.
      this.adapter.remember(player as unknown as TakaroPlayer);
      if (type === 'player-connected') this.online.set(player.gameId, player as unknown as TakaroPlayer);
      else this.online.delete(player.gameId);
      this.saveOnline();
    }
    for (const key of [player.gameId, player.steamId, player.platformId]) {
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

  private saveOnline(): void {
    try {
      this.options.onlineStore?.save([...this.online.values()]);
    } catch (err) {
      logger.warn(`Could not persist online players: ${(err as Error).message}`);
    }
  }

  /**
   * Sends `player-disconnected` for every player Takaro was told is online but who is no longer on the server — the
   * case where the battlegroup restarted while they were connected, so no leave was ever observed.
   */
  async reconcileOnline(currentGameIds: Set<string>): Promise<TakaroPlayer[]> {
    const gone: TakaroPlayer[] = [];
    for (const player of [...this.online.values()]) {
      if (currentGameIds.has(player.gameId)) continue;
      const result = this.emit('player-disconnected', { player });
      logger.info(`Reconcile: ${player.name} (${player.gameId}) is no longer on the server; sent player-disconnected (${result})`);
      if (result !== false) {
        gone.push(player);
        this.reconciled.disconnected += 1;
      }
    }
    return gone;
  }

  // --- Takaro requests -----------------------------------------------------

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

  // --- delivery guarantees -------------------------------------------------

  private trackUnconfirmed(event: PendingEvent): void {
    if (!this.confirms) {
      if (event.seq !== undefined) this.options.pluginEvents?.markDelivered(event.seq);
      return;
    }
    this.unconfirmedEvents.push({ ...event, sendId: this.options.takaro.lastSendId?.() ?? 0 });
  }

  /** A heartbeat proved everything up to `sendId` arrived: release those events and advance the persisted cursor. */
  private confirmUpTo(sendId: number): void {
    let released = 0;
    while (this.unconfirmedEvents.length && (this.unconfirmedEvents[0].sendId ?? 0) <= sendId) {
      const event = this.unconfirmedEvents.shift();
      released += 1;
      if (event?.seq !== undefined) this.options.pluginEvents?.markDelivered(event.seq);
    }
    if (released) logger.debug(`Takaro heartbeat confirmed ${released} game event(s) up to sendId ${sendId}`);
  }

  /**
   * The socket died: everything written but unconfirmed may never have left the machine, so it goes back to the FRONT
   * of the pending queue, in order, to be re-sent after the next identify. Duplicates are possible but bounded by the
   * heartbeat interval; Takaro tolerates them, silent loss is not tolerable.
   */
  private requeueUnconfirmed(): void {
    if (!this.unconfirmedEvents.length) return;
    const lost = this.unconfirmedEvents.splice(0, this.unconfirmedEvents.length).map(({ sendId, ...e }) => {
      void sendId;
      return e;
    });
    logger.warn(`Takaro connection died with ${lost.length} game event(s) unconfirmed; they will be re-sent after the next identify`);
    this.pendingEvents.unshift(...lost);
    this.trimPending();
  }

  /** Queues an event. Returns false when the queue was full and THIS event is the one that was dropped. */
  private queueEvent(event: PendingEvent): boolean {
    this.pendingEvents.push(event);
    if (this.pendingEvents.length === 1) logger.warn('Takaro is not reachable; queueing game events until it is');
    return !this.trimPending().includes(event);
  }

  /**
   * Drops events over the cap and returns the ones that were dropped: the oldest `log` line first, because Takaro
   * keeps no logs and rate-limits them anyway, and only then the oldest event of any other type.
   */
  private trimPending(): PendingEvent[] {
    const lostAll: PendingEvent[] = [];
    while (this.pendingEvents.length > MAX_PENDING_EVENTS) {
      const logIndex = this.pendingEvents.findIndex((e) => e.type === 'log');
      const index = logIndex >= 0 ? logIndex : 0;
      const lost = this.pendingEvents.splice(index, 1)[0];
      if (!lost) break;
      lostAll.push(lost);
      this.droppedEvents += 1;
      logger.error(`Pending event queue full (${MAX_PENDING_EVENTS}); dropping ${lost.type} seq=${lost.seq ?? '-'}`);
      // The plugin cursor is a high-water mark: releasing an event dropped from behind older queued ones would skip
      // those on a restart. A mid-queue drop costs at most one replayed log line instead.
      if (index === 0) this.releaseCursor(lost);
    }
    return lostAll;
  }

  /**
   * An event is gone for good: release its plugin cursor so a restart does not replay everything behind it forever.
   * Only ever called for an event the sidecar has decided to DROP, never for one still in a queue.
   */
  private releaseCursor(event: PendingEvent): void {
    if (event.seq !== undefined) this.options.pluginEvents?.markDelivered(event.seq);
  }

  private now(): number {
    return this.options.now?.() ?? Date.now();
  }

  /**
   * Sends every queued event, in order, to the (now open and identified) Takaro socket. Stops at the first failure
   * and keeps the rest. Returns how many were written.
   */
  flushPending(): number {
    if (!this.pendingEvents.length) return 0;
    logger.info(`Flushing ${this.pendingEvents.length} game event(s) buffered while Takaro was unreachable`);
    const flushed = this.drainPending();
    if (this.pendingEvents.length) {
      logger.warn(`Takaro went away again after ${flushed} buffered event(s); keeping ${this.pendingEvents.length} queued`);
    }
    if (flushed) logger.info(`Flushed ${flushed} buffered game event(s)`);
    return flushed;
  }

  /**
   * Retries a congested-but-open socket until the queue is empty. A heartbeat cannot be the trigger: the client only
   * reports a confirmation when the confirmed sendId advances, which an idle socket never does.
   */
  private scheduleDrain(): void {
    if (this.drainTimer) return;
    this.drainTimer = setTimeout(() => {
      this.drainTimer = null;
      if (!this.pendingEvents.length || this.takaroDownSince !== null) return;
      this.drainPending();
      if (this.pendingEvents.length) this.scheduleDrain();
    }, this.options.drainRetryMs ?? 1000);
    this.drainTimer.unref?.();
  }

  /** Sends queued events, in order, until the first one the socket refuses. Returns how many were written. */
  private drainPending(): number {
    let flushed = 0;
    while (this.pendingEvents.length) {
      const event = this.pendingEvents[0];
      if (!this.options.takaro.sendGameEvent(event.type, event.data)) break;
      this.pendingEvents.shift();
      flushed += 1;
      this.noteConnectionEvent(event.type, event.data);
      this.trackUnconfirmed({ type: event.type, data: event.data, seq: event.seq, at: event.at });
    }
    return flushed;
  }

  // --- lifecycle -----------------------------------------------------------

  async refreshHealth(): Promise<void> {
    if (!this.options.dependenciesOk) return;
    let ok = false;
    let error: string | null = null;
    try {
      ok = await this.options.dependenciesOk();
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    }
    this.dependencyState = { ok, error };
    if (ok) {
      this.unreachableSince = null;
      return;
    }
    this.unreachableSince ??= this.now();
    const down = this.now() - this.unreachableSince;
    const limit = this.options.exitAfterDependencyLossMs ?? 0;
    if (limit > 0 && down >= limit) {
      logger.error(
        `The connector's dependencies (Postgres) have been unreachable for ${Math.round(down / 1000)}s; exiting so the ` +
          `container restart policy re-creates this sidecar against the live battlegroup.`,
      );
      process.exit(1);
    }
    logger.warn(`Dependency check failed (${Math.round(down / 1000)}s): ${error ?? 'not reachable'}`);
  }

  /**
   * Starts the dependency probe independently of the Takaro socket: when the battlegroup is re-created the sidecar
   * loses DNS as well as the database, the socket drops and never comes back, and a probe that stopped with the
   * socket could never notice or self-exit.
   */
  startHealthWatch(): void {
    this.watchAlways = true;
    if (!this.healthTimer) {
      this.healthTimer = setInterval(() => void this.refreshHealth(), this.options.healthCheckIntervalMs ?? 15000);
      this.healthTimer.unref?.();
    }
    void this.refreshHealth();
  }

  /**
   * Starts every event source and the plugin poller, ONCE, for the whole process lifetime.
   *
   * This is deliberately not called from the Takaro `identified` handler. The sources carry the connector's
   * **game-side** duties — the presence poller that ban enforcement reads its online set from, the chat consumer, the
   * plugin `/events` drain — and none of them is Takaro's business. While they ran only between `identified` and
   * `disconnected`, a Takaro outage silently turned off kick-on-sight ban enforcement (a banned player rejoined and
   * stayed) and stopped draining the plugin ring. Delivery, and delivery alone, waits for the socket.
   */
  async startSources(): Promise<void> {
    if (this.sourcesActive) return;
    this.sourcesActive = true;
    for (const source of this.options.sources ?? []) {
      try {
        await source.start();
      } catch (err) {
        logger.warn(`Event source '${source.name}' failed to start: ${(err as Error).message}`);
      }
    }
    this.options.pluginEvents?.start();
    if (!this.healthTimer) {
      this.healthTimer = setInterval(() => void this.refreshHealth(), this.options.healthCheckIntervalMs ?? 15000);
      this.healthTimer.unref?.();
    }
  }

  /** Shutdown only. A Takaro outage must never reach this. */
  stopSources(): void {
    this.sourcesActive = false;
    this.options.pluginEvents?.stop();
    for (const source of this.options.sources ?? []) source.stop();
    if (this.watchAlways) return;
    if (this.healthTimer) clearInterval(this.healthTimer);
    this.healthTimer = null;
  }

  /**
   * Takaro identified. Deliver what the outage queued — but groomed, not replayed blindly:
   *
   * 1. queued connect/disconnect events are DISCARDED and replaced by one reconciliation against the CURRENT online
   *    set, so a two-hour outage in which a player joined and left nine times produces the truth (are they on now?)
   *    instead of eighteen stale flaps;
   * 2. chat lines older than `outageChatMaxAgeMs` are dropped and counted, so the Discord bridge and every chat hook
   *    are not flooded with a conversation that ended an hour ago;
   * 3. everything else is delivered once, in order, oldest first.
   */
  async onTakaroUp(): Promise<{ flushed: number; collapsed: number; staleChat: number; reconciled: number }> {
    this.takaroUpSince = this.now();
    this.takaroDownSince = null;
    const collapsed = this.takeQueuedPresence();
    const staleChat = this.dropStaleChat();
    const flushed = this.flushPending();
    const reconciled = this.options.currentOnline ? await this.reconcilePresence(this.options.currentOnline()) : 0;
    if (collapsed || staleChat || flushed || reconciled) {
      logger.info(
        `Takaro is back: delivered ${flushed} queued event(s), collapsed ${collapsed} presence flap(s) into ` +
          `${reconciled} reconciliation event(s), dropped ${staleChat} stale chat line(s)`,
      );
    }
    return { flushed, collapsed, staleChat, reconciled };
  }

  /** The Takaro socket went away. Sources keep running; only the unconfirmed delivery window is rewound. */
  onTakaroDown(): void {
    this.takaroDownSince = this.now();
    this.takaroUpSince = null;
    this.requeueUnconfirmed();
  }

  /** Removes every queued connect/disconnect event; they are superseded by `reconcilePresence`. */
  private takeQueuedPresence(): number {
    let removed = 0;
    for (let i = this.pendingEvents.length - 1; i >= 0; i -= 1) {
      const event = this.pendingEvents[i];
      if (event.type !== 'player-connected' && event.type !== 'player-disconnected') continue;
      this.pendingEvents.splice(i, 1);
      this.releaseCursor(event);
      removed += 1;
    }
    this.collapsedPresence += removed;
    return removed;
  }

  private dropStaleChat(): number {
    const maxAge = this.options.outageChatMaxAgeMs ?? DEFAULT_OUTAGE_CHAT_MAX_AGE_MS;
    if (maxAge <= 0) return 0;
    const cutoff = this.now() - maxAge;
    let removed = 0;
    for (let i = this.pendingEvents.length - 1; i >= 0; i -= 1) {
      const event = this.pendingEvents[i];
      if (event.type !== 'chat-message') continue;
      // An event with no timestamp predates this field; it is delivered rather than guessed to be stale.
      if (event.at === undefined || event.at >= cutoff) continue;
      this.pendingEvents.splice(i, 1);
      this.releaseCursor(event);
      removed += 1;
    }
    this.droppedStaleChat += removed;
    if (removed) logger.warn(`Dropped ${removed} chat line(s) older than ${Math.round(maxAge / 1000)}s rather than replaying them to Takaro`);
    return removed;
  }

  /**
   * Brings Takaro's idea of who is online back in line with reality in ONE pass: a connect for everybody online that
   * Takaro was never told about, a disconnect for everybody Takaro still believes is on. Returns how many events it
   * emitted.
   */
  async reconcilePresence(current: TakaroPlayer[]): Promise<number> {
    const byId = new Map(current.filter((p) => p?.gameId).map((p) => [p.gameId, p]));
    let emitted = 0;
    for (const [gameId, player] of byId) {
      if (this.online.has(gameId)) continue;
      if (this.emit('player-connected', { player }) !== false) {
        emitted += 1;
        this.reconciled.connected += 1;
        logger.info(`Reconcile: ${player.name} (${gameId}) is online and Takaro did not know; sent player-connected`);
      }
    }
    emitted += (await this.reconcileOnline(new Set(byId.keys()))).length;
    return emitted;
  }

  stopHealthWatch(): void {
    this.watchAlways = false;
    if (this.healthTimer) clearInterval(this.healthTimer);
    this.healthTimer = null;
  }

  /** Are the game-side event sources running? True from boot to shutdown, regardless of Takaro. */
  isActive(): boolean {
    return this.sourcesActive;
  }
}
