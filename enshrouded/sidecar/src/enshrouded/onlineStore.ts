import fs from 'node:fs';
import path from 'node:path';
import type { TakaroPlayer } from './types.js';

/**
 * Players the sidecar last told Takaro are online (connect forwarded, no disconnect yet). Persisted so that after a
 * game-server or sidecar restart the sidecar can send player-disconnected for players who are no longer on the server;
 * Takaro would otherwise keep them online, because the dead server process never logged their leave.
 */
export interface OnlineStore {
  load(): TakaroPlayer[];
  save(players: TakaroPlayer[]): void;
}

export class FileOnlineStore implements OnlineStore {
  constructor(private readonly file: string) {}

  load(): TakaroPlayer[] {
    try {
      const parsed = JSON.parse(fs.readFileSync(this.file, 'utf8')) as unknown;
      return Array.isArray(parsed) ? parsed.filter((p): p is TakaroPlayer => typeof p?.gameId === 'string' && typeof p?.name === 'string') : [];
    } catch {
      return [];
    }
  }

  save(players: TakaroPlayer[]): void {
    fs.mkdirSync(path.dirname(path.resolve(this.file)), { recursive: true });
    const tmp = `${this.file}.tmp`;
    fs.writeFileSync(tmp, JSON.stringify(players));
    fs.renameSync(tmp, this.file);
  }
}

export class MemoryOnlineStore implements OnlineStore {
  constructor(public players: TakaroPlayer[] = []) {}
  load(): TakaroPlayer[] {
    return [...this.players];
  }
  save(players: TakaroPlayer[]): void {
    this.players = [...players];
  }
}
