import type { ConanPlayer } from '../conan/parsers.js';
import type { GameEventType } from '../takaro/protocol.js';

export interface EmittedGameEvent {
  type: GameEventType;
  data: unknown;
}

export type EmitGameEvent = (event: EmittedGameEvent) => void;

export class PlayerPoller {
  private timer: NodeJS.Timeout | null = null;
  private initialized = false;
  private previous = new Map<string, ConanPlayer>();

  constructor(
    private readonly getPlayers: () => Promise<ConanPlayer[]>,
    private readonly emit: EmitGameEvent,
    private readonly intervalMs: number,
  ) {}

  start(): void {
    if (this.timer) return;
    void this.pollOnce();
    this.timer = setInterval(() => {
      void this.pollOnce();
    }, this.intervalMs);
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  reset(): void {
    this.initialized = false;
    this.previous.clear();
  }

  async pollOnce(): Promise<void> {
    const currentPlayers = await this.getPlayers();
    const current = new Map(currentPlayers.map((player) => [player.gameId, player]));

    if (!this.initialized) {
      this.initialized = true;
      this.previous = current;
      for (const player of current.values()) {
        this.emit({ type: 'player-connected', data: { player: eventPlayer(player) } });
      }
      return;
    }

    for (const [gameId, player] of current) {
      if (!this.previous.has(gameId)) {
        this.emit({ type: 'player-connected', data: { player: eventPlayer(player) } });
      }
    }

    for (const [gameId, player] of this.previous) {
      if (!current.has(gameId)) {
        this.emit({ type: 'player-disconnected', data: { player: eventPlayer(player) } });
      }
    }

    this.previous = current;
  }
}

function eventPlayer(player: ConanPlayer): Omit<ConanPlayer, 'online' | 'characterName' | 'rconId'> {
  const { online: _online, characterName: _characterName, rconId: _rconId, ...rest } = player;
  return rest;
}
