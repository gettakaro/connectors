import fs from 'node:fs';
import path from 'node:path';

export interface CursorState {
  seq: number;
  /** Native bootId the seq belongs to; a nonzero seq is invalid without it. */
  bootId?: string;
}

export function validCursor(value: Partial<CursorState>): CursorState {
  const bootId = typeof value.bootId === 'string' && value.bootId.trim() ? value.bootId : undefined;
  const seq = typeof value.seq === 'number' && Number.isSafeInteger(value.seq) && value.seq >= 0 ? value.seq : 0;
  return bootId ? { bootId, seq } : { seq: 0 };
}

/** Persists the last transport-confirmed native seq. Replays can duplicate events. */
export class FileCursorStore {
  constructor(private readonly file: string) {}

  load(): CursorState {
    try {
      const parsed = JSON.parse(fs.readFileSync(this.file, 'utf8')) as Partial<CursorState>;
      return validCursor(parsed);
    } catch {
      return { seq: 0 };
    }
  }

  save(state: CursorState): void {
    fs.mkdirSync(path.dirname(path.resolve(this.file)), { recursive: true });
    const tmp = `${this.file}.tmp`;
    fs.writeFileSync(tmp, JSON.stringify(state));
    fs.renameSync(tmp, this.file);
  }
}

export class MemoryCursorStore {
  constructor(public state: CursorState = { seq: 0 }) {}
  load(): CursorState {
    return validCursor(this.state);
  }
  save(state: CursorState): void {
    this.state = { ...state };
  }
}

export type CursorStore = Pick<FileCursorStore, 'load' | 'save'>;
