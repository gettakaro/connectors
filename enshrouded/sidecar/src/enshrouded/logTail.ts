import fs from 'node:fs';
import type { GameEventType } from '../takaro/protocol.js';
import type { TakaroPlayer } from './types.js';

export interface LogEvent {
  type: Extract<GameEventType, 'player-connected' | 'player-disconnected'>;
  data: { player: TakaroPlayer };
}

const RE_ADDED_PEER = /\[online\] Added peer (\d+\((\d+)\)) \(steamid:(\d+)\)/;
const RE_REMOVED_PEER = /\[online\] Removed peer (\d+\(\d+\))/;
const RE_MACHINE_LOGIN = /\[server\] Machine '(\d+)': Player '[^']*' logged in/;
const RE_PLAYER_LOGIN = /\[server\] Player '(.+)' logged in with Permissions/;
const RE_REMOVE_PLAYER = /\[server\] Remove Player '(.+)'$/;

interface Peer {
  peerId: string;
  machine: string;
  steamId: string;
  name?: string;
}

/**
 * Stateful parser for enshrouded_server.log (see research/log-events.md).
 * SteamID only appears on `[online] Added peer P (steamid:X)`; the name only on
 * `Player '<name>' logged in with Permissions` ~30s later. We correlate by machine index
 * (peer "0(1)" <-> "Machine '1'") and fall back to the oldest unnamed peer.
 */
export class EnshroudedLogParser {
  private readonly peers = new Map<string, Peer>();
  private lastMachine: string | null = null;

  feed(rawLine: string): LogEvent[] {
    const line = rawLine.replace(/\r$/, '');
    let m: RegExpExecArray | null;

    if ((m = RE_ADDED_PEER.exec(line))) {
      this.peers.set(m[1], { peerId: m[1], machine: m[2], steamId: m[3] });
      return [];
    }
    if ((m = RE_MACHINE_LOGIN.exec(line))) {
      this.lastMachine = m[1];
      return [];
    }
    if ((m = RE_PLAYER_LOGIN.exec(line))) {
      const name = m[1];
      const unnamed = [...this.peers.values()].filter((p) => !p.name);
      const peer = unnamed.find((p) => p.machine === this.lastMachine) ?? unnamed[0];
      this.lastMachine = null;
      if (!peer) return []; // join happened before we started tailing; no SteamID to report
      peer.name = name;
      return [{ type: 'player-connected', data: { player: toPlayer(peer) } }];
    }
    if ((m = RE_REMOVE_PLAYER.exec(line))) {
      const peer = [...this.peers.values()].find((p) => p.name === m![1]);
      if (!peer) return [];
      this.peers.delete(peer.peerId);
      return [{ type: 'player-disconnected', data: { player: toPlayer(peer) } }];
    }
    if ((m = RE_REMOVED_PEER.exec(line))) {
      const peer = this.peers.get(m[1]);
      this.peers.delete(m[1]);
      // Named peer that vanished without a `Remove Player` line (crash/timeout) still counts as a disconnect.
      if (peer?.name) return [{ type: 'player-disconnected', data: { player: toPlayer(peer) } }];
      return [];
    }
    return [];
  }
}

function toPlayer(peer: Peer): TakaroPlayer {
  return { gameId: peer.steamId, name: peer.name ?? peer.steamId, steamId: peer.steamId, platformId: `steam:${peer.steamId}` };
}

export interface LogTailerOptions {
  file: string;
  onEvent: (event: LogEvent) => void;
  onError?: (err: Error) => void;
  intervalMs?: number;
  /** Start at end of file (default) so a restart does not replay old joins. */
  fromStart?: boolean;
}

/** Polling tailer that survives the server rotating the log on restart (file shrinks or inode changes). */
export class LogTailer {
  private timer: NodeJS.Timeout | null = null;
  private offset = -1;
  private inode = -1;
  private partial = '';
  private parser = new EnshroudedLogParser();

  constructor(private readonly options: LogTailerOptions) {}

  running(): boolean {
    return this.timer !== null;
  }

  start(): void {
    if (this.timer) return;
    this.offset = -1;
    this.timer = setInterval(() => this.poll(), this.options.intervalMs ?? 1000);
    this.poll();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  poll(): void {
    let stat: fs.Stats;
    try {
      stat = fs.statSync(this.options.file);
    } catch (err) {
      this.options.onError?.(err as Error);
      return;
    }
    if (this.offset < 0) {
      this.offset = this.options.fromStart ? 0 : stat.size;
      this.inode = stat.ino;
    } else if (stat.ino !== this.inode || stat.size < this.offset) {
      this.offset = 0;
      this.inode = stat.ino;
      this.partial = '';
      this.parser = new EnshroudedLogParser();
    }
    if (stat.size <= this.offset) return;

    const fd = fs.openSync(this.options.file, 'r');
    try {
      const length = stat.size - this.offset;
      const buf = Buffer.alloc(length);
      fs.readSync(fd, buf, 0, length, this.offset);
      this.offset = stat.size;
      const text = this.partial + buf.toString('utf8');
      const lines = text.split('\n');
      this.partial = lines.pop() ?? '';
      for (const line of lines) {
        for (const event of this.parser.feed(line)) this.options.onEvent(event);
      }
    } finally {
      fs.closeSync(fd);
    }
  }
}
