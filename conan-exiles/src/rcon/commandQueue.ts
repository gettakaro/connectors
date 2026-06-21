export type CommandExecutor = (command: string) => Promise<string>;

export class RconCommandQueue {
  private tail: Promise<unknown> = Promise.resolve();

  constructor(
    private readonly execute: CommandExecutor,
    private readonly gapMs = 0,
  ) {}

  run(command: string): Promise<string> {
    const runAfterTail = this.tail.then(async () => {
      if (this.gapMs > 0) {
        await new Promise((resolve) => setTimeout(resolve, this.gapMs));
      }
      return this.execute(command);
    });

    this.tail = runAfterTail.catch(() => undefined);
    return runAfterTail;
  }
}
