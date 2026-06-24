import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import {
  ConanItemCatalog,
  loadConanItemCatalog,
  normalizeCatalogAlias,
  seedConanItemCatalog,
} from '../conan/itemCatalog.js';

test('seed catalog resolves built-in Conan item aliases and preserves unknown numeric ids', () => {
  const catalog = seedConanItemCatalog();

  assert.equal(catalog.resolveTemplateId('Stone'), '10001');
  assert.equal(catalog.resolveTemplateId('plant-fiber'), '12001');
  assert.equal(catalog.resolveTemplateId('Plant Fiber'), '12001');
  assert.equal(catalog.resolveTemplateId('41001'), '41001');
  assert.equal(catalog.resolveTemplateId('Conan item 52562'), '52562');
  assert.equal(catalog.publicCodeForTemplate('10001'), 'Stone');
  assert.equal(catalog.nameForTemplate('12001'), 'Plant Fiber');
  assert.deepEqual(catalog.itemForTemplate('99999'), { code: '99999', name: 'Conan item 99999' });
});

test('catalog normalizes aliases by removing non-alphanumeric characters', () => {
  assert.equal(normalizeCatalogAlias(' Iron_Ore-01 '), 'ironore01');
});

test('catalog rejects alias collisions between different template ids', () => {
  assert.throws(
    () =>
      new ConanItemCatalog({
        version: 1,
        source: 'test',
        items: [
          { templateId: '41001', code: 'Iron_Ore', name: 'Iron Ore' },
          { templateId: '41002', code: 'IronOre', name: 'Different Iron' },
        ],
      }),
    /alias collision/i,
  );
});

test('catalog allows duplicate display names but does not resolve ambiguous names', () => {
  const catalog = new ConanItemCatalog({
    version: 1,
    source: 'test',
    items: [
      { templateId: '51706', code: '51706', name: 'Abysmal Blade' },
      { templateId: '51709', code: '51709', name: 'Abysmal Blade' },
      { templateId: '52562', code: '52562', name: 'Cimmerian Steel Pauldron' },
    ],
  });

  assert.equal(catalog.resolveTemplateId('Abysmal Blade'), 'Abysmal Blade');
  assert.equal(catalog.resolveTemplateId('Abysmal Blade (51706)'), '51706');
  assert.equal(catalog.resolveTemplateId('Abysmal Blade (51709)'), '51709');
  assert.equal(catalog.resolveTemplateId('Cimmerian Steel Pauldron'), '52562');
  assert.deepEqual(catalog.listKnownItems(), [
    { code: '51706', name: 'Abysmal Blade (51706)' },
    { code: '51709', name: 'Abysmal Blade (51709)' },
    { code: '52562', name: 'Cimmerian Steel Pauldron' },
    { code: 'PlantFiber', name: 'Plant Fiber' },
    { code: 'Stone', name: 'Stone' },
  ]);
});

test('catalog still rejects explicit alias collisions between different template ids', () => {
  assert.throws(
    () =>
      new ConanItemCatalog({
        version: 1,
        source: 'test',
        items: [
          { templateId: '51706', code: '51706', name: 'Abysmal Blade', aliases: ['abysmal'] },
          { templateId: '51709', code: '51709', name: 'Abysmal Blade', aliases: ['abysmal'] },
        ],
      }),
    /alias collision/i,
  );
});

test('catalog hides obvious internal rows from item picker lists but preserves numeric ids', () => {
  const catalog = new ConanItemCatalog({
    version: 1,
    source: 'test',
    items: [
      { templateId: '51204', code: '51204', name: 'XX_Unarmed Right' },
      { templateId: '51205', code: '51205', name: 'DEV_Test Blade' },
      { templateId: '14182', code: '14182', name: 'Demon Blood' },
    ],
  });

  assert.equal(catalog.resolveTemplateId('51204'), '51204');
  assert.equal(catalog.resolveTemplateId('XX_Unarmed Right'), '51204');
  assert.equal(catalog.nameForTemplate('51204'), 'XX_Unarmed Right');
  assert.deepEqual(catalog.listKnownItems(), [
    { code: '14182', name: 'Demon Blood' },
    { code: 'PlantFiber', name: 'Plant Fiber' },
    { code: 'Stone', name: 'Stone' },
  ]);
});

test('loads an external catalog JSON and exposes full sorted list', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'conan-item-catalog-'));
  try {
    const file = path.join(dir, 'items.json');
    writeFileSync(
      file,
      JSON.stringify({
        version: 1,
        source: 'fixture',
        items: [
          { templateId: '41002', code: 'Coal', name: 'Coal' },
          { templateId: '41001', code: 'Iron_Ore', name: 'Iron Ore', aliases: ['iron'] },
        ],
      }),
    );

    const catalog = loadConanItemCatalog(file);

    assert.equal(catalog.hasExternalCatalog, true);
    assert.equal(catalog.resolveTemplateId('Iron Ore'), '41001');
    assert.equal(catalog.resolveTemplateId('iron'), '41001');
    assert.deepEqual(catalog.listKnownItems(), [
      { code: 'Coal', name: 'Coal' },
      { code: 'Iron_Ore', name: 'Iron Ore' },
      { code: 'PlantFiber', name: 'Plant Fiber' },
      { code: 'Stone', name: 'Stone' },
    ]);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
