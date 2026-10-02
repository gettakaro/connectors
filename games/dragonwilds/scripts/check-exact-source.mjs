#!/usr/bin/env node
// Do the plugin's developer build and the catalog target pin the same native sources?
//
// The release is built in Dockerfile.builder from the URLs and SHA-256s the catalog records
// in `build.deps`; every archive is hash-checked there before it is unpacked. Developers and
// the dev rig build with mod/Dockerfile.build instead. If the two disagreed, the plugin a
// developer tested would not be the plugin that ships. So every catalog dependency must
// appear in mod/Dockerfile.build with exactly the same URL and hash, and that file may fetch
// nothing the catalog does not record.
//
// Reads DRAGONWILDS_DEP_<NAME>_URL / _SHA256 pairs from the environment, which
// `takaro-maint targets resolve` writes from `build.deps`.
//
// Usage: node check-exact-source.mjs [path/to/Dockerfile.build]
// Exit codes: 0 fine, 7 the two drifted.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const DRIFT = 7;
const DEFAULT_DOCKERFILE = join(dirname(fileURLToPath(import.meta.url)), '..', 'mod', 'Dockerfile.build');

function pinnedDeps(env) {
  const deps = [];
  for (const [key, value] of Object.entries(env)) {
    const found = /^DRAGONWILDS_DEP_(.+)_URL$/.exec(key);
    if (!found) continue;
    const sha256 = env[`DRAGONWILDS_DEP_${found[1]}_SHA256`];
    if (!sha256) {
      console.error(`${key} is set but DRAGONWILDS_DEP_${found[1]}_SHA256 is not`);
      process.exit(DRIFT);
    }
    deps.push({ name: found[1], url: value, sha256: sha256.toLowerCase() });
  }
  return deps.sort((a, b) => a.name.localeCompare(b.name));
}

function main() {
  const deps = pinnedDeps(process.env);
  if (deps.length === 0) {
    console.error('no DRAGONWILDS_DEP_*_URL in the environment; resolve the target first');
    process.exit(DRIFT);
  }
  const path = process.argv[2] ?? DEFAULT_DOCKERFILE;
  const text = readFileSync(path, 'utf8');
  const problems = [];

  for (const dep of deps) {
    if (!text.includes(dep.url)) problems.push(`${dep.name}: ${path} does not fetch ${dep.url}`);
    if (!text.toLowerCase().includes(dep.sha256)) problems.push(`${dep.name}: ${path} does not check ${dep.sha256}`);
  }
  const recorded = new Set(deps.map((dep) => dep.url));
  for (const url of text.match(/https:\/\/[^\s'"\\]+/g) ?? []) {
    if (!recorded.has(url)) problems.push(`${path} fetches ${url}, which the catalog target does not record`);
  }

  if (problems.length) {
    console.error(`the developer build and the catalog target drifted:\n  ${problems.join('\n  ')}`);
    process.exit(DRIFT);
  }
  for (const dep of deps) console.log(`exact source: ${dep.name} ${dep.sha256}`);
}

main();
