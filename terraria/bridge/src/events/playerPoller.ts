import { logger } from '../logger.js';
import type { GameEvent } from '../takaro/protocol.js';
import type { TShockPlayer } from '../tshock/client.js';

const FAILURE_LOG_INTERVAL_MS = 60_000;

export class PlayerPoller {
  private timer: NodeJS.Timeout | null = null;
  private previous = new Map<string, TShockPlayer>();
  private polling = false;
  private lastFailureMessage: string | null = null;
  private lastFailureLoggedAt = 0;
  private suppressedFailures = 0;
  lastPollAt: string | null = null;

  constructor(
    private readonly loadPlayers: () => Promise<TShockPlayer[]>,
    private readonly emit: (event: GameEvent) => void,
    private readonly intervalMs: number,
  ) {}

  start(): void {
    if (this.timer) return;
    void this.pollOnce();
    this.timer = setInterval(() => void this.pollOnce(), this.intervalMs);
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  reset(): void {
    this.previous.clear();
    this.lastPollAt = null;
    this.lastFailureMessage = null;
    this.lastFailureLoggedAt = 0;
    this.suppressedFailures = 0;
  }

  async pollOnce(): Promise<void> {
    if (this.polling) return;
    this.polling = true;
    try {
      const nextPlayers = await this.loadPlayers();
      const next = new Map(nextPlayers.map((player) => [player.gameId, player]));
      for (const [id, player] of next) {
        if (!this.previous.has(id)) this.emit({ type: 'player-connected', data: { player } });
      }
      for (const [id, player] of this.previous) {
        if (!next.has(id)) this.emit({ type: 'player-disconnected', data: { player } });
      }
      this.previous = next;
      this.lastPollAt = new Date().toISOString();
      this.notePollRecovered();
    } catch (err) {
      // A failed poll tells us nothing about who is online, so leave `previous` and
      // `lastPollAt` untouched: clearing them would emit false disconnects on the next
      // successful poll, and a stale lastPollAt is the correct /health liveness signal.
      this.notePollFailed(err);
    } finally {
      this.polling = false;
    }
  }

  private notePollFailed(err: unknown): void {
    const message = err instanceof Error ? err.message : String(err);
    const now = Date.now();
    const isNewError = message !== this.lastFailureMessage;
    const backoffElapsed = now - this.lastFailureLoggedAt >= FAILURE_LOG_INTERVAL_MS;
    if (isNewError || backoffElapsed) {
      const suppressed = this.suppressedFailures > 0 ? ` (${this.suppressedFailures} similar failures suppressed)` : '';
      logger.error(`Player poll failed: ${message}${suppressed}`);
      this.lastFailureMessage = message;
      this.lastFailureLoggedAt = now;
      this.suppressedFailures = 0;
      return;
    }
    this.suppressedFailures += 1;
  }

  private notePollRecovered(): void {
    if (!this.lastFailureMessage) return;
    const suppressed = this.suppressedFailures > 0 ? ` (${this.suppressedFailures} similar failures suppressed)` : '';
    logger.info(`Player poll recovered${suppressed}`);
    this.lastFailureMessage = null;
    this.lastFailureLoggedAt = 0;
    this.suppressedFailures = 0;
  }
}
