import fs from 'node:fs';
import path from 'node:path';

export interface CursorState {
  seq: number;
  /** Plugin bootId the seq belongs to (absent for plugins < 0.4.1). */
  bootId?: string;
}

/** Persists the last forwarded plugin event seq so a sidecar restart neither replays nor drops events. */
export class FileCursorStore {
  constructor(private readonly file: string) {}

  load(): CursorState {
    try {
      const parsed = JSON.parse(fs.readFileSync(this.file, 'utf8')) as Partial<CursorState>;
      const seq = typeof parsed.seq === 'number' && Number.isFinite(parsed.seq) && parsed.seq >= 0 ? parsed.seq : 0;
      return typeof parsed.bootId === 'string' && parsed.bootId ? { seq, bootId: parsed.bootId } : { seq };
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
    return { ...this.state };
  }
  save(state: CursorState): void {
    this.state = { ...state };
  }
}

export type CursorStore = Pick<FileCursorStore, 'load' | 'save'>;
