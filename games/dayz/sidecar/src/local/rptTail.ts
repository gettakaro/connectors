// F9 — `log` events on Linux.
//
// The Windows DLL bridge tails the server's .RPT file and forwards each line to
// Takaro as a `log` event. On the Linux/sidecar path nothing did, so two of the
// six required events had no source. This module reproduces the DLL behaviour in
// the sidecar: the DayZ profiles directory is mounted into the bridge container
// (DAYZ_LOG_DIR, e.g. /profiles) and we tail the newest *.RPT and script_*.log.
//
// Design notes:
//   * Poll-based (1 s) rather than fs.watch: the profiles mount is read-only and
//     may be a bind mount where inotify is unreliable.
//   * Start at end-of-file. A restart must never replay hours of old log lines
//     into Takaro's event stream.
//   * Rotation: DayZ writes a NEW timestamped .RPT per boot. Each poll we
//     re-scan for the newest matching file and switch to it (starting at 0, so
//     a fresh file is captured from its first line).
//   * Deny-list + rate limit, because an unfiltered DayZ RPT is mostly engine
//     spam and would drown the Takaro event feed.

import fs from 'node:fs';
import path from 'node:path';
import { logger } from '../logger.js';

/** Patterns dropped unless DAYZ_LOG_DENY overrides them. */
export const DEFAULT_DENY = [
  '\\[Takaro\\]\\[DBG\\]',
  'BattlEye Server: RCon',
  'Recv error',
  'Warning Message',
  'Cannot register',
];

export interface RptTailOptions {
  dir: string;
  /** Comma-separated regex list; falls back to DEFAULT_DENY. */
  denyPatterns?: string[];
  maxPerSec?: number;
  pollIntervalMs?: number;
  /** Filename filter. Defaults to *.RPT and script_*.log. */
  matches?: (name: string) => boolean;
  /** Cross-file duplicate window (ms). Default 60 000. */
  dedupeWindowMs?: number;
}

/** Strips DayZ's optional "H:MM:SS[.mmm] " RPT prefix and trims whitespace. */
export function normalizeLogLine(line: string): string {
  return line.replace(/^\s*\d{1,2}:\d{2}:\d{2}(\.\d{1,3})?\s+/, '').replace(/\s+/g, ' ').trim();
}

export interface LogEvent {
  type: 'log';
  timestamp: string;
  msg: string;
}

function defaultMatches(name: string): boolean {
  return /\.rpt$/i.test(name) || /^script_.*\.log$/i.test(name);
}

export function parseDenyList(raw: string | undefined): RegExp[] {
  const patterns = raw && raw.trim() !== '' ? raw.split(',') : DEFAULT_DENY;
  const out: RegExp[] = [];
  for (const p of patterns) {
    const trimmed = p.trim();
    if (!trimmed) continue;
    try {
      out.push(new RegExp(trimmed));
    } catch {
      logger.warn(`rptTail: ignoring invalid DAYZ_LOG_DENY pattern '${trimmed}'`);
    }
  }
  return out;
}

interface Tracked {
  file: string;
  offset: number;
  /** Carry for a trailing partial line between polls. */
  partial: string;
}

export class RptTail {
  private timer: NodeJS.Timeout | null = null;
  private tracked = new Map<string, Tracked>();
  private deny: RegExp[];
  private maxPerSec: number;
  private pollIntervalMs: number;
  private matches: (name: string) => boolean;
  private windowStart = 0;
  private windowCount = 0;
  private dropped = 0;
  private emitted = 0;
  private busy = false;
  /**
   * Cross-file dedupe. DayZ writes every SCRIPT line to BOTH the .RPT (with a
   * "HH:MM:SS " prefix) and script_*.log (without it, and flushed lazily, often
   * 10-20 s later). Key = line without the time prefix; a line already emitted
   * from the OTHER file within dedupeWindowMs is dropped.
   */
  private recent = new Map<string, { kind: string; at: number }>();
  private dedupeWindowMs: number;
  private deduped = 0;

  constructor(
    private opts: RptTailOptions,
    private onEvent: (ev: LogEvent) => void,
  ) {
    this.deny = (opts.denyPatterns ? parseDenyList(opts.denyPatterns.join(',')) : parseDenyList(undefined));
    this.maxPerSec = opts.maxPerSec && opts.maxPerSec > 0 ? opts.maxPerSec : 20;
    this.pollIntervalMs = opts.pollIntervalMs ?? 1000;
    this.matches = opts.matches ?? defaultMatches;
    this.dedupeWindowMs = opts.dedupeWindowMs ?? 60_000;
  }

  dedupedCount(): number {
    return this.deduped;
  }

  /** True if this line was already emitted from the other log file recently. */
  private isCrossFileDuplicate(line: string, kind: string): boolean {
    const now = Date.now();
    const key = normalizeLogLine(line);
    if (this.recent.size > 5000) {
      for (const [k, v] of this.recent) if (now - v.at > this.dedupeWindowMs) this.recent.delete(k);
    }
    const prev = this.recent.get(key);
    if (prev && prev.kind !== kind && now - prev.at <= this.dedupeWindowMs) {
      this.recent.delete(key); // consume: a genuine third occurrence is emitted
      this.deduped++;
      return true;
    }
    this.recent.set(key, { kind, at: now });
    return false;
  }

  /** Lines forwarded since boot (surfaced on /health as logsForwarded). */
  forwarded(): number {
    return this.emitted;
  }

  droppedCount(): number {
    return this.dropped;
  }

  start(): void {
    if (this.timer) return;
    logger.info(
      `rptTail: watching ${this.opts.dir} (deny=${this.deny.length} patterns, max ${this.maxPerSec}/s)`,
    );
    // Prime offsets at end-of-file so boot does not replay history.
    this.poll(true).catch((e) => logger.warn(`rptTail prime failed: ${String(e)}`));
    this.timer = setInterval(() => {
      void this.poll(false).catch((e) => logger.warn(`rptTail poll failed: ${String(e)}`));
    }, this.pollIntervalMs);
    this.timer.unref?.();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  /** Newest file per kind (.RPT and script log), so a rotation is picked up. */
  private newestFiles(): string[] {
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(this.opts.dir, { withFileTypes: true });
    } catch (e) {
      logger.debug(`rptTail: cannot read ${this.opts.dir}: ${String(e)}`);
      return [];
    }
    const best = new Map<string, { file: string; mtime: number }>();
    for (const ent of entries) {
      if (!ent.isFile() || !this.matches(ent.name)) continue;
      const full = path.join(this.opts.dir, ent.name);
      let mtime: number;
      try {
        mtime = fs.statSync(full).mtimeMs;
      } catch {
        continue;
      }
      const kind = /\.rpt$/i.test(ent.name) ? 'rpt' : 'script';
      const cur = best.get(kind);
      if (!cur || mtime > cur.mtime) best.set(kind, { file: full, mtime });
    }
    return [...best.values()].map((v) => v.file);
  }

  private allow(line: string): boolean {
    if (line.trim() === '') return false;
    for (const re of this.deny) {
      if (re.test(line)) return false;
    }
    return true;
  }

  private rateLimited(): boolean {
    const now = Date.now();
    if (now - this.windowStart >= 1000) {
      if (this.dropped > 0) {
        logger.debug(`rptTail: dropped ${this.dropped} lines to the rate limit`);
      }
      this.windowStart = now;
      this.windowCount = 0;
    }
    if (this.windowCount >= this.maxPerSec) {
      this.dropped++;
      return true;
    }
    this.windowCount++;
    return false;
  }

  /** One poll pass. `prime` starts new files at EOF instead of at 0. */
  async poll(prime: boolean): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try {
      const files = this.newestFiles();
      // Forget files we are no longer following (rotated away).
      for (const key of [...this.tracked.keys()]) {
        if (!files.includes(key)) this.tracked.delete(key);
      }
      for (const file of files) {
        let size: number;
        try {
          size = fs.statSync(file).size;
        } catch {
          continue;
        }
        let t = this.tracked.get(file);
        if (!t) {
          // A file we have never seen: at boot start at EOF (no replay); a file
          // that appeared later is a fresh rotation, so read it from the start.
          t = { file, offset: prime ? size : 0, partial: '' };
          this.tracked.set(file, t);
          logger.info(`rptTail: following ${file} from offset ${t.offset}`);
          if (prime) continue;
        }
        if (size < t.offset) {
          // Truncated in place — restart from the beginning.
          t.offset = 0;
          t.partial = '';
        }
        if (size === t.offset) continue;

        const chunk = this.readRange(file, t.offset, size);
        if (chunk === null) continue;
        t.offset = size;
        const text = t.partial + chunk;
        const kind = /\.rpt$/i.test(file) ? 'rpt' : 'script';
        const lines = text.split(/\r?\n/);
        t.partial = lines.pop() ?? '';
        for (const line of lines) {
          if (!this.allow(line)) continue;
          if (this.isCrossFileDuplicate(line, kind)) continue;
          if (this.rateLimited()) continue;
          this.emitted++;
          this.onEvent({ type: 'log', timestamp: new Date().toISOString(), msg: line });
        }
      }
    } finally {
      this.busy = false;
    }
  }

  private readRange(file: string, from: number, to: number): string | null {
    let fd: number | null = null;
    try {
      fd = fs.openSync(file, 'r');
      const len = to - from;
      const buf = Buffer.alloc(len);
      const read = fs.readSync(fd, buf, 0, len, from);
      return buf.subarray(0, read).toString('utf8');
    } catch (e) {
      logger.debug(`rptTail: read ${file} failed: ${String(e)}`);
      return null;
    } finally {
      if (fd !== null) fs.closeSync(fd);
    }
  }
}

/** Builds an RptTail from the environment, or null when DAYZ_LOG_DIR is unset. */
export function rptTailFromEnv(onEvent: (ev: LogEvent) => void): RptTail | null {
  const dir = process.env.DAYZ_LOG_DIR;
  if (!dir || dir.trim() === '') return null;
  const maxRaw = parseInt(process.env.DAYZ_LOG_MAX_PER_SEC || '20', 10);
  const tail = new RptTail(
    {
      dir,
      denyPatterns: process.env.DAYZ_LOG_DENY ? [process.env.DAYZ_LOG_DENY] : undefined,
      maxPerSec: Number.isFinite(maxRaw) && maxRaw > 0 ? maxRaw : 20,
    },
    onEvent,
  );
  return tail;
}
