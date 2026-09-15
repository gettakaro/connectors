import fsSync from 'node:fs';
import fs from 'node:fs/promises';
import path from 'node:path';
import { logger } from '../logger.js';
import { parseLogLine } from './logParser.js';
import type { GameEvent } from '../takaro/protocol.js';

const FAILURE_LOG_INTERVAL_MS = 60_000;

/** Matches the TShock log naming scheme, e.g. `2026-09-01_07-31-49.log`. */
const DEFAULT_LOG_PATTERN = '*.log';

interface ActiveFile {
  path: string;
  /** Identifies the physical file so a same-path recreate is seen as a new file. */
  inode: number;
  offset: number;
}

/**
 * Tails TShock logs and emits parsed events.
 *
 * TShock names each log after the server start time and opens a brand-new file on every
 * restart, so a tailer pinned to one path goes permanently silent once the server bounces.
 * When constructed with a directory (or a `dir/glob` pattern) this class re-resolves the
 * newest matching file on every poll and follows the rotation.
 */
export class LogTailer {
  private active: ActiveFile | null = null;
  private timer: NodeJS.Timeout | null = null;
  private polling = false;
  /** False until the first successful resolve, which selects the first-start read position. */
  private started = false;
  private lastFailureMessage: string | null = null;
  private lastFailureLoggedAt = 0;
  private suppressedFailures = 0;

  private readonly directory: string | null;
  private readonly pattern: string;

  constructor(
    private readonly source: string,
    private readonly emit: (event: GameEvent) => void,
    private readonly intervalMs = 1000,
    private readonly startAtEnd = true,
    private readonly excludePatterns: readonly string[] = [],
  ) {
    const { directory, pattern } = splitSource(source);
    this.directory = directory;
    this.pattern = pattern;
  }

  /** True when a line matches any configured exclude pattern (case-insensitive substring). */
  private isExcluded(line: string): boolean {
    if (this.excludePatterns.length === 0) return false;
    const haystack = line.toLowerCase();
    return this.excludePatterns.some((pattern) => haystack.includes(pattern.toLowerCase()));
  }

  start(): void {
    if (this.timer) return;
    void this.pollOnce();
    this.timer = setInterval(() => void this.pollOnce(), this.intervalMs);
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  /** The file currently being followed, or null before the first successful resolve. */
  currentFile(): string | null {
    return this.active?.path ?? null;
  }

  async pollOnce(): Promise<void> {
    // Polls can outlive their interval on a slow disk; overlapping runs would double-read
    // the same byte range and emit every event twice.
    if (this.polling) return;
    this.polling = true;
    try {
      const target = await this.resolveTarget();
      if (!target) return;
      await this.readFrom(target);
      this.noteResolveRecovered();
    } catch (err) {
      // A missing directory, an unreadable file, or a log deleted mid-rotation are all
      // transient: keep the current offset, log with backoff, and retry on the next tick.
      this.noteResolveFailed(err);
    } finally {
      this.polling = false;
    }
  }

  /** Picks the file to read this tick, switching to a newer or recreated file when one appears. */
  private async resolveTarget(): Promise<ActiveFile | null> {
    const candidate = this.directory === null ? this.source : await this.newestMatch(this.directory);
    if (!candidate) return null;

    const stat = await fs.stat(candidate);
    if (!stat.isFile()) return null;
    const inode = Number(stat.ino);

    if (this.active && this.active.path === candidate && this.active.inode === inode) {
      // Same physical file. A size going backwards means it was truncated in place, so the
      // bytes we already consumed are gone and the file must be re-read from the start.
      if (stat.size < this.active.offset) this.active.offset = 0;
      return this.active;
    }

    if (!this.started) {
      // FIRST START: an existing log may hold hours of history. Honour startAtEnd so the
      // bridge does not replay stale chat and deaths into Takaro on boot.
      this.started = true;
      this.active = { path: candidate, inode, offset: this.startAtEnd ? stat.size : 0 };
      return this.active;
    }

    // ROTATION: the server restarted and wrote plugin markers into the new file immediately,
    // so this file is read from byte 0 regardless of startAtEnd or nothing is lost.
    // Duplicates are impossible: the previous file's offset is discarded with the file
    // itself, and this offset only ever advances within the new inode.
    logger.info(`Log rotation detected, following ${candidate}`);
    this.active = { path: candidate, inode, offset: 0 };
    return this.active;
  }

  private async readFrom(target: ActiveFile): Promise<void> {
    const handle = await fs.open(target.path, 'r');
    try {
      const stat = await handle.stat();
      if (stat.size <= target.offset) return;

      const buffer = Buffer.alloc(stat.size - target.offset);
      const { bytesRead } = await handle.read(buffer, 0, buffer.length, target.offset);
      // Advance before emitting: a throwing consumer must not cause the same bytes to be
      // re-read and re-emitted on the next poll.
      target.offset += bytesRead;
      for (const line of buffer.subarray(0, bytesRead).toString('utf8').split(/\r?\n/)) {
        const event = parseLogLine(line);
        if (!event) continue;
        // Only plain `log` passthrough is filtered. A gameplay event (a death, a kill, a chat
        // line) still carries its own meaning if its text happens to match an exclude pattern,
        // so dropping it here would lose real data rather than noise.
        if (event.type === 'log' && this.isExcluded(line)) continue;
        this.emit(event);
      }
    } finally {
      await handle.close();
    }
  }

  /** Newest matching log in `directory` by mtime, or null when the directory holds none. */
  private async newestMatch(directory: string): Promise<string | null> {
    const entries = await fs.readdir(directory, { withFileTypes: true });
    let newest: { path: string; mtimeMs: number } | null = null;

    for (const entry of entries) {
      if (!entry.isFile() || !matchesPattern(entry.name, this.pattern)) continue;
      const candidate = path.join(directory, entry.name);
      try {
        const stat = await fs.stat(candidate);
        if (!newest || stat.mtimeMs > newest.mtimeMs) newest = { path: candidate, mtimeMs: stat.mtimeMs };
      } catch {
        // Raced with a rotation that removed the file; the next poll re-resolves.
      }
    }

    return newest?.path ?? null;
  }

  private noteResolveFailed(err: unknown): void {
    const message = err instanceof Error ? err.message : String(err);
    const now = Date.now();
    const isNewError = message !== this.lastFailureMessage;
    const backoffElapsed = now - this.lastFailureLoggedAt >= FAILURE_LOG_INTERVAL_MS;
    if (isNewError || backoffElapsed) {
      const suppressed = this.suppressedFailures > 0 ? ` (${this.suppressedFailures} similar failures suppressed)` : '';
      logger.error(`Log tail failed for ${this.source}: ${message}${suppressed}`);
      this.lastFailureMessage = message;
      this.lastFailureLoggedAt = now;
      this.suppressedFailures = 0;
      return;
    }
    this.suppressedFailures += 1;
  }

  private noteResolveRecovered(): void {
    if (!this.lastFailureMessage) return;
    const suppressed = this.suppressedFailures > 0 ? ` (${this.suppressedFailures} similar failures suppressed)` : '';
    logger.info(`Log tail recovered for ${this.source}${suppressed}`);
    this.lastFailureMessage = null;
    this.lastFailureLoggedAt = 0;
    this.suppressedFailures = 0;
  }
}

/**
 * Splits a configured source into a watch directory and a filename pattern.
 *
 * A plain file path keeps a null directory so it is followed verbatim, preserving the
 * existing `logFiles=<absolute file>` contract exactly.
 */
function splitSource(source: string): { directory: string | null; pattern: string } {
  const base = path.basename(source);
  if (base.includes('*')) return { directory: path.dirname(source) || '.', pattern: base };
  // A trailing separator is an explicit directory even before it exists on disk.
  if (/[\\/]$/.test(source)) return { directory: source.replace(/[\\/]+$/, '') || '/', pattern: DEFAULT_LOG_PATTERN };
  if (isExistingDirectory(source)) return { directory: source, pattern: DEFAULT_LOG_PATTERN };
  return { directory: null, pattern: base };
}

function isExistingDirectory(candidate: string): boolean {
  try {
    // Sync is confined to construction, where the config's directory-vs-file shape is fixed.
    return fsSync.statSync(candidate).isDirectory();
  } catch {
    return false;
  }
}

/** Minimal `*` glob matching; no dependency needed for the one wildcard TShock names require. */
function matchesPattern(name: string, pattern: string): boolean {
  if (!pattern.includes('*')) return name === pattern;
  const escaped = pattern.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*');
  return new RegExp(`^${escaped}$`, 'i').test(name);
}
