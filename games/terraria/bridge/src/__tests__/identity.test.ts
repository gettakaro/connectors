import assert from 'node:assert/strict';
import { test } from 'node:test';
import { TAKARO_PLATFORM_ID, terrariaIdentity, terrariaPlatformId } from '../terraria/identity.js';

test('plain character names map to terraria:<name>', () => {
  assert.deepEqual(terrariaIdentity('CodexTest'), { gameId: 'CodexTest', platformId: 'terraria:CodexTest' });
  assert.equal(terrariaPlatformId('guide-user_2'), 'terraria:guide-user_2');
});

test('names outside the platformId alphabet still produce a valid, stable, distinct platformId', () => {
  const names = ['Bob the Builder', 'Mr. Smith', 'Ägir', 'a b', 'a_b', 'a.b', '  padded  '];
  const ids = names.map(terrariaPlatformId);
  for (const id of ids) assert.match(id, TAKARO_PLATFORM_ID);
  assert.equal(terrariaPlatformId('Bob the Builder'), terrariaPlatformId('Bob the Builder'));
  assert.ok(terrariaPlatformId('Bob the Builder').startsWith('terraria:Bob_the_Builder-'));
  // 'a b' and 'a.b' sanitise alike; the digest keeps them apart, and neither collides with 'a_b'.
  assert.equal(new Set(ids).size, ids.length);
  assert.equal(terrariaIdentity('  padded  ').gameId, 'padded');
});
