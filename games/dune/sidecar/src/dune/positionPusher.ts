import type { Position, TakaroPlayer } from './types.js';
import type { TakaroSink } from '../bridge.js';
import { logger } from '../logger.js';

export const POSITION_INTERVAL_MS = 5000;
const POSITION_CHECK_INTERVAL_MS = 250;
export const MAX_POSITION_BATCH = 1000;

/** A snapshot is ephemeral: never queue a coordinate through a Takaro outage. */
export class PositionPusher {
  private timer: NodeJS.Timeout | null = null;
  private busy = false;
  private lastSendAt = 0;

  constructor(private readonly deps: {
    online: () => TakaroPlayer[];
    location: (gameId: string) => Promise<Position | null>;
    ready?: () => Promise<boolean>;
    availability?: (available: boolean) => void;
    takaro: TakaroSink & { identified(): boolean; getProtocolVersion(): number };
    now?: () => number;
  }) {}

  start(): void {
    if (this.timer) return;
    // Check ahead of the send deadline. A timer firing a millisecond early must not defer
    // the next snapshot by another full interval.
    this.timer = setInterval(() => void this.tick(), POSITION_CHECK_INTERVAL_MS);
    this.timer.unref?.();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  async tick(): Promise<boolean> {
    if (this.busy || !this.deps.takaro.identified() || this.deps.takaro.getProtocolVersion() !== 1) return false;
    const now = this.deps.now?.() ?? Date.now();
    if (this.lastSendAt && now - this.lastSendAt < POSITION_INTERVAL_MS) return false;
    const players = this.deps.online();
    const ids = [...new Set(players.map((p) => p.gameId))];
    if (ids.length > MAX_POSITION_BATCH) return false;
    this.busy = true;
    try {
      if (this.deps.ready && !await this.deps.ready()) {
        this.deps.availability?.(false);
        return false;
      }
      const locations = await Promise.all(ids.map((id) => this.deps.location(id)));
      // One missing pawn invalidates the whole snapshot. A partial batch could make Takaro keep old coordinates
      // for the omitted player while believing this server is pushing current positions.
      if (locations.some((location) => !location)) {
        this.deps.availability?.(false);
        return false;
      }
      if (!this.deps.takaro.identified() || this.deps.takaro.getProtocolVersion() !== 1) return false;
      const sentAt = this.deps.now?.() ?? Date.now();
      if (this.lastSendAt && sentAt - this.lastSendAt < POSITION_INTERVAL_MS) return false;
      const positions = ids.map((gameId, i) => ({ gameId, position: locations[i]! }));
      this.deps.availability?.(true);
      const sent = this.deps.takaro.sendGameEvent('player-positions', { timestamp: new Date(sentAt).toISOString(), positions });
      if (sent) this.lastSendAt = sentAt;
      return sent;
    } catch (error) {
      this.deps.availability?.(false);
      logger.debug(`Live position snapshot unavailable: ${(error as Error).message}`);
      return false;
    } finally {
      this.busy = false;
    }
  }
}
