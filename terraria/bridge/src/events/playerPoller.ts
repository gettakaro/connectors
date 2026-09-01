import type { GameEvent } from '../takaro/protocol.js';
import type { TShockPlayer } from '../tshock/client.js';

export class PlayerPoller {
  private timer: NodeJS.Timeout | null = null;
  private previous = new Map<string, TShockPlayer>();
  private polling = false;
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
    } finally {
      this.polling = false;
    }
  }
}
