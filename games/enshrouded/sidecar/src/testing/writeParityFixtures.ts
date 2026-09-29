import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildParityFixtures } from './parityFixtures.js';

export const FIXTURE_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../../mod/tests/fixtures');

export function fixtureText(value: unknown): string {
  return `${JSON.stringify(value, null, 1)}\n`;
}

const isMain = process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1]);
if (isMain) {
  const fixtures = await buildParityFixtures();
  fs.mkdirSync(FIXTURE_DIR, { recursive: true });
  for (const [name, value] of Object.entries(fixtures)) {
    const file = path.join(FIXTURE_DIR, `parity-${name}.json`);
    fs.writeFileSync(file, fixtureText(value));
    console.log(`wrote ${file}`);
  }
}
