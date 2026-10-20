import type { GameEventType } from '../takaro/protocol.js';

/**
 * Exactly ONE `player-death` per death, preferring the attributed one.
 *
 * ---------------------------------------------------------------------------------------------
 * THE PROBLEM
 *
 * A single death in Dune is visible to the connector twice, from two independent sources:
 *
 *  * the **Postgres `life_state` edge** (`Alive` → `Dead` / `DeadByCoriolis` / `DeadBySandworm`),
 *    which the presence poller sees at its next tick. It always fires, and it has no killer.
 *  * the **plugin's death hook**, which fires within a frame and carries the attribution (killer,
 *    weapon, position) — but only when the plugin is present AND the death came through one of the
 *    UFunctions it filters on.
 *
 * Forwarding both would give Takaro two deaths for one, doubling every kill-feed line and every
 * module trigger. Forwarding only the DB edge would throw away the attribution that the whole plugin
 * exists for. So the two are coalesced here.
 *
 * ---------------------------------------------------------------------------------------------
 * THE RULE
 *
 *  * A **plugin** death is emitted immediately (it is the richer one) and marks the player.
 *  * A **life_state** death is HELD for `windowMs` and only emitted if no plugin death for that
 *    player arrived meanwhile. The hold is what makes the outcome deterministic instead of a race:
 *    the presence poll can legitimately observe the edge before the plugin poll has caught up.
 *  * Either way, a second death for the same player inside `windowMs` is dropped and counted, not
 *    emitted. The window is deliberately short (default 5 s): a real respawn-and-die-again is slower
 *    than that, and a genuinely fast second death being merged is a smaller error than every death
 *    being reported twice.
 *  * After that, the player stays dead for the coalescer until the presence poller sees them alive again
 *    (`revived`). The plugin's attributed death can trail the life_state edge by much more than the window:
 *    on Steam build 26051014 a dehydration defeat reached the plugin 13 s after Postgres already said
 *    `Dead`, and both went to Takaro. One life ends once, so any further death before a revival is a
 *    duplicate. `maxDeadMs` (default 10 min) bounds the mark in case a revival is never observed.
 *  * With **no plugin at all** the hold still applies, so a death is at most `windowMs` late and the
 *    behaviour is identical for both configurations. That is stated in the README rather than hidden.
 *
 * Timers are injectable so the tests drive this deterministically instead of sleeping.
 */
export interface DeathCoalescerOptions {
  emit: (type: GameEventType, data: Record<string, unknown>) => void;
  /** How long a life_state death waits for the plugin's attributed one. Default 5000 ms. */
  windowMs?: number;
  /** How long an emitted death stays the player's last one when no revival is observed. Default 600000 ms. */
  maxDeadMs?: number;
  now?: () => number;
  setTimer?: (fn: () => void, ms: number) => unknown;
  clearTimer?: (handle: unknown) => void;
}

interface Pending {
  data: Record<string, unknown>;
  handle: unknown;
  at: number;
}

export class DeathCoalescer {
  private readonly pending = new Map<string, Pending>();
  /** When each player's last death was emitted; cleared by `revived`. */
  private readonly emittedAt = new Map<string, number>();
  private readonly counters = { fromPlugin: 0, fromLifeState: 0, suppressedLifeState: 0, suppressedDuplicate: 0 };

  constructor(private readonly options: DeathCoalescerOptions) {}

  private now(): number {
    return this.options.now?.() ?? Date.now();
  }
  private windowMs(): number {
    return this.options.windowMs ?? 5000;
  }

  /** `clearTimer?.(h) ?? clearTimeout(h)` would run BOTH when the injected clearer returns void. */
  private clear(handle: unknown): void {
    if (this.options.clearTimer) this.options.clearTimer(handle);
    else clearTimeout(handle as NodeJS.Timeout);
  }

  private maxDeadMs(): number {
    return this.options.maxDeadMs ?? 600_000;
  }

  /** True when this player's death was already emitted and they have not been seen alive since. */
  private recentlyEmitted(gameId: string): boolean {
    const at = this.emittedAt.get(gameId);
    return at !== undefined && this.now() - at < Math.max(this.windowMs(), this.maxDeadMs());
  }

  /** The presence poller saw the player alive after a death (respawn, or a fresh join): the next death is new. */
  revived(gameId: string): void {
    this.emittedAt.delete(gameId);
  }

  /** The plugin's attributed death. Wins, always, and cancels a held life_state death. */
  fromPlugin(gameId: string, data: Record<string, unknown>): void {
    const held = this.pending.get(gameId);
    if (held) {
      this.clear(held.handle);
      this.pending.delete(gameId);
      this.counters.suppressedLifeState++;
    }
    if (this.recentlyEmitted(gameId)) {
      // The life_state edge already went out (it beat the plugin by more than the window) and the
      // player has not been seen alive since. Emitting now would be a second death for the same
      // life, so it is dropped and counted.
      this.counters.suppressedDuplicate++;
      return;
    }
    this.emittedAt.set(gameId, this.now());
    this.counters.fromPlugin++;
    this.options.emit('player-death', data);
  }

  /** The Postgres `life_state` edge. Held for the window, then emitted if nothing better arrived. */
  fromLifeState(gameId: string, data: Record<string, unknown>): void {
    if (this.recentlyEmitted(gameId) || this.pending.has(gameId)) {
      this.counters.suppressedDuplicate++;
      return;
    }
    const fire = (): void => {
      this.pending.delete(gameId);
      if (this.recentlyEmitted(gameId)) {
        this.counters.suppressedDuplicate++;
        return;
      }
      this.emittedAt.set(gameId, this.now());
      this.counters.fromLifeState++;
      this.options.emit('player-death', data);
    };
    const handle = this.options.setTimer
      ? this.options.setTimer(fire, this.windowMs())
      : (() => {
          const t = setTimeout(fire, this.windowMs());
          t.unref?.();
          return t;
        })();
    this.pending.set(gameId, { data, handle, at: this.now() });
  }

  /** Emits everything still held, ignoring the window. Called on shutdown so nothing is lost. */
  flush(): void {
    for (const [gameId, held] of [...this.pending]) {
      this.clear(held.handle);
      this.pending.delete(gameId);
      if (this.recentlyEmitted(gameId)) continue;
      this.emittedAt.set(gameId, this.now());
      this.counters.fromLifeState++;
      this.options.emit('player-death', held.data);
    }
  }

  stop(): void {
    for (const held of this.pending.values()) {
      this.clear(held.handle);
    }
    this.pending.clear();
  }

  pendingIds(): string[] {
    return [...this.pending.keys()];
  }

  status(): Record<string, unknown> {
    return { ...this.counters, windowMs: this.windowMs(), pending: this.pending.size };
  }
}
