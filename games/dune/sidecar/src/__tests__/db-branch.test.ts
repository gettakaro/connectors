import { describe, expect, it, vi } from 'vitest';
import { logger } from '../logger.js';
import { DunePg, newerBranchDatabase } from '../dune/pg.js';
import type { Queryable } from '../dune/pg.js';

/**
 * Measured on the dev rig 2026-10-20: Steam build 25936522 moved the battlegroup from `dune_sb_1_5_3_0` to
 * `dune_sb_1_5_15_0`. A connector still configured with the old name connects fine and reads a frozen roster.
 */
describe('newer branch database', () => {
  it('finds the newest branch database above ours, comparing versions numerically', () => {
    expect(newerBranchDatabase('dune_sb_1_5_3_0', ['dune_sb_1_5_3_0', 'dune_sb_1_5_15_0'])).toBe('dune_sb_1_5_15_0');
    expect(newerBranchDatabase('dune_sb_1_5_3_0', ['dune_sb_1_5_15_0', 'dune_sb_1_6_0_0', 'dune_sb_1_4_0_0'])).toBe(
      'dune_sb_1_6_0_0',
    );
  });

  it('reports nothing when ours is the newest, or when the name is not a branch database', () => {
    expect(newerBranchDatabase('dune_sb_1_5_15_0', ['dune_sb_1_5_3_0', 'dune_sb_1_5_15_0'])).toBeNull();
    expect(newerBranchDatabase('dune', ['dune_sb_1_5_15_0'])).toBeNull();
    expect(newerBranchDatabase('dune_sb_1_5_3_0', [])).toBeNull();
  });

  function db(answer: () => { current: string; names: string[] | null }[]): Queryable {
    return {
      query: async <T>(sql: string): Promise<{ rows: T[] }> =>
        ({ rows: sql.includes('current_database()') ? answer() : [] }) as { rows: T[] },
      end: async () => {},
    };
  }

  it('probe() warns once with both names when the connector reads an old branch database', async () => {
    const warn = vi.spyOn(logger, 'warn').mockImplementation(() => {});
    try {
      const pg = new DunePg(db(() => [{ current: 'dune_sb_1_5_3_0', names: ['dune_sb_1_5_3_0', 'dune_sb_1_5_15_0'] }]));
      await pg.probe();
      expect(pg.isProbed()).toBe(true);
      expect(warn).toHaveBeenCalledTimes(1);
      expect(warn.mock.calls[0][0]).toContain("'dune_sb_1_5_3_0'");
      expect(warn.mock.calls[0][0]).toContain("'dune_sb_1_5_15_0'");
    } finally {
      warn.mockRestore();
    }
  });

  it('a failing lookup never fails the probe', async () => {
    const warn = vi.spyOn(logger, 'warn').mockImplementation(() => {});
    try {
      const pg = new DunePg(
        db(() => {
          throw new Error('permission denied for table pg_database');
        }),
      );
      await pg.probe();
      expect(pg.isProbed()).toBe(true);
      expect(warn).not.toHaveBeenCalled();
    } finally {
      warn.mockRestore();
    }
  });
});
