import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { after, test } from 'node:test';
import { describeStamp, readTargetStamp } from '../targetStamp.js';

const roots: string[] = [];

after(() => {
  for (const root of roots) fs.rmSync(root, { recursive: true, force: true });
});

function packageRoot(stamp?: unknown): string {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'takaro-conan-stamp-'));
  roots.push(root);
  if (stamp !== undefined) {
    fs.writeFileSync(
      path.join(root, 'takaro-target.json'),
      typeof stamp === 'string' ? stamp : JSON.stringify(stamp),
      'utf8',
    );
  }
  return root;
}

const RELEASE = {
  target: 'linux-25488622',
  fingerprint: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
  game: 'conan-exiles',
  platform: 'linux',
  revision: '25488622',
  connectorVersion: '1.0.2',
  sourceRevision: 'deadbeefcafe',
};

test('a released package names the catalog target it was built for', () => {
  const stamp = readTargetStamp(packageRoot(RELEASE));

  assert.deepEqual(stamp, RELEASE);
  assert.equal(
    describeStamp(stamp),
    'Takaro target: linux-25488622 (0123456789abcdef) revision 25488622 connector 1.0.2 source deadbeefcafe',
  );
});

test('the fingerprint in the log line is the first sixteen characters', () => {
  const stamp = readTargetStamp(packageRoot(RELEASE));

  assert.ok(stamp);
  assert.ok(describeStamp(stamp).includes(`(${stamp.fingerprint.slice(0, 16)})`));
  assert.ok(!describeStamp(stamp).includes(stamp.fingerprint));
});

test('a development tree is unstamped rather than broken', () => {
  assert.equal(readTargetStamp(packageRoot()), null);
  assert.equal(describeStamp(null), 'Takaro target: unstamped (development tree)');
});

test('an unreadable stamp is ignored, not fatal', () => {
  assert.equal(readTargetStamp(packageRoot('{ not json')), null);
  assert.equal(readTargetStamp(packageRoot({ connectorVersion: '1.0.2' })), null);
});
