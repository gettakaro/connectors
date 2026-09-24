import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

export interface BanMetadata {
  reason: string;
  expiresAt: string | null;
  state: 'pending' | 'active';
  pendingSince?: number;
}

export type BanMetadataState = Record<string, BanMetadata>;

export interface BanStore {
  load(): BanMetadataState;
  save(state: BanMetadataState): void;
}

const validId = (id: string): boolean => /^\d{17}$/.test(id);

function validate(value: unknown): BanMetadataState {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('Ban metadata is invalid');
  }
  const document = value as Record<string, unknown>;
  if (document.version !== 1 || !document.bans || typeof document.bans !== 'object' ||
      Array.isArray(document.bans)) throw new Error('Ban metadata version or entries are invalid');
  const entries = Object.entries(document.bans as Record<string, unknown>);
  if (entries.length > 4096) throw new Error('Ban metadata has too many entries');
  const result: BanMetadataState = Object.create(null) as BanMetadataState;
  for (const [id, raw] of entries) {
    if (!validId(id) || !raw || typeof raw !== 'object' || Array.isArray(raw)) {
      throw new Error('Ban metadata contains an invalid player entry');
    }
    const row = raw as Record<string, unknown>;
    if (typeof row.reason !== 'string' || [...row.reason].length > 10000 ||
        Buffer.byteLength(row.reason, 'utf8') > 40000 ||
        (row.expiresAt !== null &&
          (typeof row.expiresAt !== 'string' || !Number.isFinite(Date.parse(row.expiresAt)))) ||
        (row.state !== 'pending' && row.state !== 'active') ||
        (row.state === 'pending' &&
          (typeof row.pendingSince !== 'number' || !Number.isSafeInteger(row.pendingSince) ||
            row.pendingSince <= 0))) {
      throw new Error('Ban metadata contains invalid reason, expiry, or state');
    }
    result[id] = { reason: row.reason, expiresAt: row.expiresAt as string | null,
      state: row.state as BanMetadata['state'],
      ...(row.state === 'pending' ? { pendingSince: row.pendingSince as number } : {}) };
  }
  return result;
}

/** Kept beside the event cursor on the already persistent sidecar data mount. */
export class FileBanStore implements BanStore {
  constructor(private readonly file: string) {}

  load(): BanMetadataState {
    try {
      return validate(JSON.parse(fs.readFileSync(this.file, 'utf8')) as unknown);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === 'ENOENT') return Object.create(null) as BanMetadataState;
      throw new Error(`Cannot load ban metadata at ${this.file}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }

  save(state: BanMetadataState): void {
    validate({ version: 1, bans: state });
    const dir = path.dirname(path.resolve(this.file));
    fs.mkdirSync(dir, { recursive: true });
    const tmp = `${this.file}.${process.pid}.${randomUUID()}.tmp`;
    let fd: number | undefined;
    try {
      fd = fs.openSync(tmp, 'wx', 0o600);
      fs.writeFileSync(fd, JSON.stringify({ version: 1, bans: state }));
      fs.fsyncSync(fd);
      fs.closeSync(fd);
      fd = undefined;
      fs.renameSync(tmp, this.file);
      const dirFd = fs.openSync(dir, 'r');
      try { fs.fsyncSync(dirFd); } finally { fs.closeSync(dirFd); }
    } finally {
      if (fd !== undefined) fs.closeSync(fd);
      try { fs.unlinkSync(tmp); } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error;
      }
    }
  }
}

export class MemoryBanStore implements BanStore {
  private state: BanMetadataState = Object.create(null) as BanMetadataState;
  load(): BanMetadataState { return structuredClone(this.state) as BanMetadataState; }
  save(state: BanMetadataState): void { this.state = validate({ version: 1, bans: state }); }
}
