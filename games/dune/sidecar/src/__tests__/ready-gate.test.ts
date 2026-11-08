import { describe, expect, it } from 'vitest';
import { waitForBattlegroup } from '../dune/readyGate.js';

function clock() {
  let t = 0;
  return { now: () => t, sleep: async (ms: number) => void (t += ms) };
}

describe('identify waits for the battlegroup', () => {
  it('returns as soon as the battlegroup is reachable', async () => {
    const c = clock();
    const answers = [false, false, true];
    const ok = await waitForBattlegroup(async () => ({ connectable: answers.shift() ?? true, reason: 'loading' }), {
      maxWaitMs: 600_000,
      intervalMs: 15_000,
      ...c,
    });
    expect(ok).toBe(true);
    expect(c.now()).toBe(30_000);
  });

  it('gives up after the limit so a broken game still registers', async () => {
    const c = clock();
    const ok = await waitForBattlegroup(async () => ({ connectable: false, reason: 'no partition' }), { maxWaitMs: 60_000, intervalMs: 15_000, ...c });
    expect(ok).toBe(false);
    expect(c.now()).toBe(60_000);
  });

  it('treats a failing probe as not ready', async () => {
    const c = clock();
    let calls = 0;
    const ok = await waitForBattlegroup(async () => {
      calls += 1;
      if (calls === 1) throw new Error('db down');
      return { connectable: true, reason: null };
    }, { maxWaitMs: 60_000, ...c });
    expect(ok).toBe(true);
  });

  it('a probe that never answers does not hold startup past the limit', async () => {
    const ok = await waitForBattlegroup(() => new Promise(() => {}), { maxWaitMs: 50, intervalMs: 10 });
    expect(ok).toBe(false);
  });
});
