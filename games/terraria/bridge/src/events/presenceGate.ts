import type { GameEvent } from '../takaro/protocol.js';

/**
 * Joins and leaves reach the bridge twice: the REST poller sees the player list change and the
 * log tail reads TShock's "has joined." / "has left." lines. Both are kept (the log is
 * immediate, the poller catches what the log misses), so this gate lets through only the first
 * report of each change. Everything that is not a join or leave passes untouched.
 */
export class PresenceGate {
  private readonly online = new Set<string>();

  accept(event: GameEvent): boolean {
    if (event.type !== 'player-connected' && event.type !== 'player-disconnected') return true;
    const gameId = (event.data as { player?: { gameId?: string } }).player?.gameId;
    if (!gameId) return true;
    if (event.type === 'player-connected') {
      if (this.online.has(gameId)) return false;
      this.online.add(gameId);
      return true;
    }
    return this.online.delete(gameId);
  }

  reset(): void {
    this.online.clear();
  }
}
