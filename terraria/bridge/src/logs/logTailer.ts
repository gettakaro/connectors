import fs from 'node:fs';
import { parseLogLine } from './logParser.js';
import type { GameEvent } from '../takaro/protocol.js';

export class LogTailer {
  private offset: number | null = null;
  private timer: NodeJS.Timeout | null = null;

  constructor(
    private readonly file: string,
    private readonly emit: (event: GameEvent) => void,
    private readonly intervalMs = 1000,
    private readonly startAtEnd = true,
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

  async pollOnce(): Promise<void> {
    if (!fs.existsSync(this.file)) return;
    const stat = fs.statSync(this.file);
    if (this.offset === null) this.offset = this.startAtEnd ? stat.size : 0;
    if (stat.size < this.offset) this.offset = 0;
    if (stat.size === this.offset) return;
    const fd = fs.openSync(this.file, 'r');
    try {
      const buffer = Buffer.alloc(stat.size - this.offset);
      fs.readSync(fd, buffer, 0, buffer.length, this.offset);
      this.offset = stat.size;
      for (const line of buffer.toString('utf8').split(/\r?\n/)) {
        const event = parseLogLine(line);
        if (event) this.emit(event);
      }
    } finally {
      fs.closeSync(fd);
    }
  }
}
