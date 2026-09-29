import fs from 'node:fs';
import path from 'node:path';
import { describe, expect, it } from 'vitest';
import { buildParityFixtures } from '../testing/parityFixtures.js';
import { FIXTURE_DIR } from '../testing/writeParityFixtures.js';

// The native connector's host tests replay these fixtures; they must be exactly what this sidecar produces.
describe('native parity fixtures', () => {
  it('match the sidecar behaviour (regenerate with npm run parity-fixtures)', async () => {
    const fixtures = await buildParityFixtures();
    for (const [name, value] of Object.entries(fixtures)) {
      const committed = JSON.parse(fs.readFileSync(path.join(FIXTURE_DIR, `parity-${name}.json`), 'utf8'));
      expect(committed, `parity-${name}.json`).toEqual(JSON.parse(JSON.stringify(value)));
    }
  });
});
