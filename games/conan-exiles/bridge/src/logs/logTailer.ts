import fs from 'node:fs/promises';
import { parseLogLine, type LogParserOptions } from './logParser.js';
import type { EmitGameEvent } from '../events/playerPoller.js';

export class LogTailer {
  private offset: number | null = null;
  private carry = '';
  private timer: NodeJS.Timeout | null = null;

  constructor(
    private readonly file: string,
    private readonly emit: EmitGameEvent,
    private readonly intervalMs = 1000,
    private readonly parserOptions: LogParserOptions = {},
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

  async pollOnce(): Promise<void> {
    let stat;
    try {
      stat = await fs.stat(this.file);
    } catch {
      return;
    }

    if (this.offset === null || stat.size < this.offset) {
      this.offset = stat.size;
      this.carry = '';
      return;
    }

    if (stat.size === this.offset) return;

    const handle = await fs.open(this.file, 'r');
    try {
      const length = stat.size - this.offset;
      const buffer = Buffer.alloc(length);
      await handle.read(buffer, 0, length, this.offset);
      this.offset = stat.size;
      this.process(buffer.toString('utf8'));
    } finally {
      await handle.close();
    }
  }

  private process(chunk: string): void {
    const text = this.carry + chunk;
    const lines = text.split(/\r?\n/);
    this.carry = lines.pop() ?? '';

    for (const line of lines) {
      for (const event of parseLogLine(line, this.parserOptions)) {
        this.emit(event);
      }
    }
  }
}
