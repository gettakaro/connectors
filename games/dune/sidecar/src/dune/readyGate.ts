import { logger } from '../logger.js';

/**
 * Waits until the battlegroup answers reachable, or until `maxWaitMs` has passed.
 *
 * Takaro syncs a new server's items and entities once, right after the first identify, and gives up after about a
 * minute. A battlegroup that is still loading its map fails every attempt, and the catalogue then stays empty until
 * the hourly sweep. Identifying only once the game is ready gives that first sync a server that can answer.
 */
export async function waitForBattlegroup(
  probe: () => Promise<{ connectable: boolean; reason: string | null }>,
  options: { maxWaitMs: number; intervalMs?: number; sleep?: (ms: number) => Promise<void>; now?: () => number },
): Promise<boolean> {
  const now = options.now ?? Date.now;
  const sleep = options.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
  const deadline = now() + options.maxWaitMs;
  let reason: string | null = null;
  for (;;) {
    const interval = options.intervalMs ?? 15_000;
    // A probe that hangs on a database connect must not hold startup past the limit.
    const answer = await Promise.race([
      probe().catch((err: Error) => ({ connectable: false, reason: err.message })),
      new Promise<{ connectable: boolean; reason: string }>((resolve) => {
        setTimeout(() => resolve({ connectable: false, reason: 'probe timed out' }), interval).unref?.();
      }),
    ]);
    if (answer.connectable) return true;
    reason = answer.reason;
    if (now() >= deadline) break;
    logger.info(`Waiting for the battlegroup before identifying with Takaro: ${reason ?? 'not ready'}`);
    await sleep(interval);
  }
  logger.warn(`Battlegroup still not ready after ${Math.round(options.maxWaitMs / 1000)} s (${reason ?? 'unknown'}); identifying anyway`);
  return false;
}
