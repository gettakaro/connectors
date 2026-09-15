import type { GameEventType } from '../takaro/protocol.js';
import type { CursorStore } from './cursorStore.js';
import { mapPluginEvent } from './mapping.js';
import type { PluginEventsResponse } from './types.js';

export type EmitFn = (type: GameEventType, data: unknown) => boolean | void;

export interface EventPollerOptions {
  getEvents: (since: number) => Promise<PluginEventsResponse>;
  emit: EmitFn;
  store: CursorStore;
  /** Called when the plugin reports a different bootId than the stored cursor (game server restarted). */
  onRestart?: () => void;
  /** Event types that must not be forwarded from the plugin (e.g. while the log tail owns them). */
  suppress?: () => ReadonlySet<string>;
  onError?: (err: Error) => void;
  intervalMs?: number;
}

export class EventPoller {
  private timer: NodeJS.Timeout | null = null;
  private running = false;
  private seq: number;
  private bootId: string | undefined;

  constructor(private readonly options: EventPollerOptions) {
    const state = options.store.load();
    this.seq = state.seq;
    this.bootId = state.bootId;
  }

  cursor(): number {
    return this.seq;
  }

  start(): void {
    if (this.timer) return;
    this.timer = setInterval(() => void this.tick(), this.options.intervalMs ?? 1000);
    void this.tick();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  /** One poll. Advances + persists the cursor only past events that were delivered (emit !== false). */
  async pollOnce(): Promise<number> {
    const response = await this.options.getEvents(this.seq);
    const events = Array.isArray(response.events) ? [...response.events].sort((a, b) => a.seq - b.seq) : [];

    // Plugin restarted: a different bootId (plugin >= 0.4.1, reliable even if the new process already passed our
    // seq), or its seq went backwards below our cursor. Start over from its beginning.
    const newBoot = typeof response.bootId === 'string' && response.bootId ? response.bootId : undefined;
    const bootChanged = newBoot !== undefined && this.bootId !== undefined && newBoot !== this.bootId;
    const seqWentBack = this.seq > 0 && typeof response.seq === 'number' && response.seq >= 0 && response.seq < this.seq;
    if (bootChanged || seqWentBack) {
      this.seq = 0;
      this.bootId = newBoot;
      this.save(0);
      this.options.onRestart?.();
      return this.pollOnce();
    }
    if (newBoot !== undefined && this.bootId === undefined) {
      this.bootId = newBoot;
      if (this.seq === 0) this.save(0);
    }

    const suppressed = this.options.suppress?.() ?? new Set<string>();
    let advanced = this.seq;
    for (const event of events) {
      if (event.seq <= advanced) continue;
      let mapped = null;
      try {
        mapped = mapPluginEvent(event);
      } catch (err) {
        this.options.onError?.(new Error(`Dropping malformed plugin event seq=${event.seq}: ${(err as Error).message}`));
      }
      if (mapped && !suppressed.has(mapped.type)) {
        if (this.options.emit(mapped.type, mapped.data) === false) break; // Takaro offline: retry from here next tick
      }
      advanced = event.seq;
    }
    if (advanced !== this.seq) {
      this.seq = advanced;
      this.save(advanced);
    }
    return this.seq;
  }

  private save(seq: number): void {
    this.options.store.save(this.bootId ? { seq, bootId: this.bootId } : { seq });
  }

  private async tick(): Promise<void> {
    if (this.running) return;
    this.running = true;
    try {
      await this.pollOnce();
    } catch (err) {
      this.options.onError?.(err instanceof Error ? err : new Error(String(err)));
    } finally {
      this.running = false;
    }
  }
}
