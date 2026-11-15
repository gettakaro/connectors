import { logger } from '../logger.js';
import type { PlayerPositionsSnapshot } from '../terraria/adapter.js';

export class PositionPoller {
  private timer: NodeJS.Timeout | null = null;
  private generation = 0;
  private running = false;
  private lastEmittedAt = 0;

  constructor(
    private readonly load: () => Promise<PlayerPositionsSnapshot>,
    private readonly emit: (snapshot: PlayerPositionsSnapshot) => void,
    private readonly intervalMs: number,
  ) {}

  start(): void {
    if (this.running) return;
    this.running = true;
    const generation = ++this.generation;
    const delay = Math.max(0, this.intervalMs - (Date.now() - this.lastEmittedAt));
    this.timer = setTimeout(() => void this.poll(generation), delay);
  }

  stop(): void {
    this.running = false;
    this.generation++;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
  }

  private async poll(generation: number): Promise<void> {
    try {
      const snapshot = await this.load();
      if (this.running && generation === this.generation) {
        this.emit(snapshot);
        this.lastEmittedAt = Date.now();
      }
    } catch (err) {
      if (this.running && generation === this.generation) {
        logger.warn(`Player positions poll failed: ${err instanceof Error ? err.message : String(err)}`);
      }
    } finally {
      if (this.running && generation === this.generation) {
        this.timer = setTimeout(() => void this.poll(generation), this.intervalMs);
      }
    }
  }
}
