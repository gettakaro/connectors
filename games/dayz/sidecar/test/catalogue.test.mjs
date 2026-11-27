// Human catalogue names and the names in kill and death events; the game profile.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { Catalogue, cleanName, humanizeCatalogue, humanizeCode } from '../dist/catalogue.js';
import { CONNECTOR_IDENTITY, gameProfileCapability, loadGameIcon, normalizeGameVersion } from '../dist/gameProfile.js';
import { buildManifest } from '../dist/ws/capabilities.js';

test('untranslated markers, string keys and bare class names become readable', () => {
  assert.equal(cleanName('$UNT$Kiwi', 'Kiwi'), 'Kiwi');
  assert.equal(cleanName('$STR_CfgVehicles_Hatchet0', 'Hatchet'), 'Hatchet');
  assert.equal(cleanName('', 'StorageBox_Wooden'), 'Storage Box Wooden');
  assert.equal(cleanName('StorageBox_Wooden', 'StorageBox_Wooden'), 'Storage Box Wooden');
  assert.equal(cleanName('Cooking Pot', 'Pot'), 'Cooking Pot');
  assert.equal(humanizeCode('ZmbF_takiOilRigWorker_Red'), 'Female Taki Oil Rig Worker Red');
});

test('variants that share a name get the part of the class name that differs', () => {
  const out = humanizeCatalogue([
    { name: 'Armband', code: 'Armband_UEC', amount: 1 },
    { name: 'Armband', code: 'Armband_NavyBlue', amount: 1 },
    { name: 'Hatchet', code: 'Hatchet', amount: 1 },
  ]);
  assert.deepEqual(out.map((e) => e.name), ['Armband (UEC)', 'Armband (Navy Blue)', 'Hatchet']);
  assert.equal(out[0].amount, 1, 'other fields are kept');
});

test('every infected gets its own name', () => {
  const codes = ['ZmbM_VillagerOld_Blue', 'ZmbF_HikerSkinny_Red', 'ZmbM_NBC_Yellow', 'ZmbM_VillagerOld_Green'];
  const out = humanizeCatalogue(codes.map((code) => ({ name: 'Infected', code, type: 'hostile' })));
  assert.deepEqual(out.map((e) => e.name), [
    'Infected (Male Villager Old Blue)',
    'Infected (Female Hiker Skinny Red)',
    'Infected (Male NBC Yellow)',
    'Infected (Male Villager Old Green)',
  ]);
  assert.equal(new Set(out.map((e) => e.name)).size, codes.length);
});

test('classes whose variant parts collide fall back to the class name', () => {
  const out = humanizeCatalogue([
    { name: 'Wheel', code: 'Wheel_A_B' },
    { name: 'Wheel', code: 'Wheel_B_A' },
  ]);
  assert.deepEqual(out.map((e) => e.name), ['Wheel (Wheel_A_B)', 'Wheel (Wheel_B_A)']);
});

test('kill and death events carry catalogue names', () => {
  const c = new Catalogue();
  c.setItems(humanizeCatalogue([{ name: 'Mosin 91/30', code: 'Mosin9130' }]));
  c.setEntities(humanizeCatalogue([
    { name: 'Infected', code: 'ZmbM_HermitSkinny_Beige' },
    { name: 'Infected', code: 'ZmbF_JoggerSkinny_Red' },
  ]));
  const kill = c.translateEvent('entity-killed', { entity: 'zmbm_hermitskinny_beige', weapon: 'Mosin9130', player: { gameId: '1' } });
  assert.equal(kill.entity, 'Infected (Male Hermit Skinny Beige)');
  assert.equal(kill.weapon, 'Mosin 91/30');
  assert.deepEqual(kill.player, { gameId: '1' });
  assert.equal(c.translateEvent('player-death', { msg: 'killed with Mosin9130' }).msg, 'killed with Mosin 91/30');
  assert.equal(c.translateEvent('entity-killed', { entity: 'Animal_CervusElaphus', weapon: '' }).entity, 'Animal Cervus Elaphus');
  assert.equal(c.translateEvent('chat-message', { msg: 'killed with Mosin9130' }).msg, 'killed with Mosin9130');
});

test('game profile: identity slugs, server build and icon', () => {
  assert.equal(CONNECTOR_IDENTITY.game, 'dayz');
  assert.equal(CONNECTOR_IDENTITY.connector.name, 'takaro-dayz');
  assert.match(CONNECTOR_IDENTITY.connector.version, /^\d+\.\d+\.\d+/);
  assert.equal(normalizeGameVersion(' 1.30.164289 '), '1.30.164289');
  assert.equal(normalizeGameVersion('x'.repeat(33)), null);
  assert.equal(normalizeGameVersion(undefined), null);

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dayz-icon-'));
  const png = path.join(dir, 'icon.png');
  fs.writeFileSync(png, Buffer.from([0x89, 0x50, 0x4e, 0x47, 1, 2, 3]));
  const icon = loadGameIcon([pathToFileURL(path.join(dir, 'missing.png')), pathToFileURL(png)]);
  assert.equal(icon.size, 7);
  assert.match(icon.revision, /^[0-9a-f]{12}$/);
  assert.equal(loadGameIcon([pathToFileURL(path.join(dir, 'missing.png'))]), null);

  assert.deepEqual(gameProfileCapability('1.30.164289', icon), {
    name: 'game.profile',
    parameters: { name: 'DayZ', version: '1.30.164289', icon: { revision: icon.revision, size: 7 } },
  });
  assert.deepEqual(gameProfileCapability(null, null), { name: 'game.profile', parameters: { name: 'DayZ' } });
  const manifest = buildManifest(null, '1.30.164289', icon);
  assert.equal(manifest[0].name, 'game.profile');
  assert.ok(manifest.some((c) => c.name === 'items.list'));
});
