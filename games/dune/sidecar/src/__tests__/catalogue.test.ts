import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { Catalogue } from '../dune/catalogue.js';
// The generator is plain ESM with no dependencies, so the test suite imports its real predicates rather than
// re-implementing them — the rule that decides what ships is the rule that is tested.
import { buildEntities, buildItems, cleanText, fixMojibake, isDevName, validate } from '../../scripts/gen-catalogue.mjs';
import { FIXTURE_ENTITIES, FIXTURE_ITEMS, harness } from './helpers.js';
import type { TakaroItem } from '../dune/types.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DATA_DIR = path.resolve(HERE, '..', '..', 'data');

describe('name quality gate', () => {
  it('rejects the asset-id shapes that have burned a previous campaign', () => {
    for (const bad of [
      'BP_Zombie_C',
      'DT_VehicleTemplates',
      'Power_Pack',
      'PowerPack2',
      'ArrakeenFormals Schematic',
      'D_Neut_Statues_Patent',
      'ITEM',
      'Thing07',
      '',
      '   ',
    ]) {
      expect(isDevName(bad, 'code'), bad).toBe(true);
    }
  });

  it('keeps real display names, including the awkward ones', () => {
    for (const good of [
      'Power Pack Mk2',
      'A Dart for Every Man',
      "Abulurd's Rapture",
      'Albatross Wing Module Mk4',
      'Artisan Disruptor M11',
      'Sandworm',
      // A single plain English word that happens to be its own template id is NOT a dev name.
      'Kindjal',
      'Literjon',
      'Corpse',
    ]) {
      expect(isDevName(good, good), good).toBe(false);
    }
  });

  it('flattens MediaWiki markup, which Takaro would otherwise render raw', () => {
    expect(cleanText('A [[Unique]] disruptor')).toBe('A Unique disruptor');
    expect(cleanText('This [[:Category:Unique|Unique]] blade')).toBe('This Unique blade');
    expect(cleanText("''italics'' and <b>tags</b>")).toBe('italics and tags');
    expect(cleanText('  spaced \n out  ')).toBe('spaced out');
    expect(cleanText(undefined)).toBeUndefined();
    expect(cleanText('   ')).toBeUndefined();
  });
});

describe('generator', () => {
  const api = [
    { item_id: 'PowerPack2', name: 'Power Pack Mk2', short_description: 'Provides power to [[equipment]].' },
    { item_id: 'Literjon', name: 'Literjon', long_description: 'Portable container.' },
    // dropped: no code
    { item_id: null, name: 'Nameless' },
    // dropped: dev-style name
    { item_id: 'D_ChoamSet', name: 'D_ChoamSet' },
    // dropped: duplicate code, first wins
    { item_id: 'PowerPack2', name: 'Power Pack Mk2 (dupe)' },
  ];

  it('drops exactly what it cannot ship, and says why', () => {
    const { rows, dropped } = buildItems(api);
    expect(rows.map((r) => r.code)).toEqual(['Literjon', 'PowerPack2']); // sorted by name
    expect(dropped).toEqual({ noCode: 1, noName: 0, devName: 1, duplicate: 1 });
    expect(rows.find((r) => r.code === 'PowerPack2')?.description).toBe('Provides power to equipment.');
  });

  it('sorts by display name, because that is the order a human browses', () => {
    const { rows } = buildItems([
      { item_id: 'z', name: 'Aaa' },
      { item_id: 'a', name: 'Zzz' },
    ]);
    expect(rows.map((r) => r.name)).toEqual(['Aaa', 'Zzz']);
  });

  it('its own validator passes on its own output', () => {
    const { rows } = buildItems(api);
    expect(validate(rows, 'items')).toEqual([]);
    expect(validate(buildEntities(), 'entities')).toEqual([]);
  });

  it('the validator is what would stop a bad catalogue being written', () => {
    expect(validate([{ code: 'BP_X', name: 'BP_X_C' }], 'items')).toHaveLength(1);
    expect(validate([{ code: 'A', name: 'Alpha' }, { code: 'a', name: 'Alpha again' }], 'items')).toContain('items: duplicate code a');
  });

  it('ships only entities it can actually name', () => {
    const entities = buildEntities();
    expect(entities.length).toBeGreaterThan(0);
    for (const entity of entities) {
      expect(isDevName(entity.name, entity.code)).toBe(false);
      expect(['hostile', 'friendly', 'neutral']).toContain(entity.type);
    }
    // Vehicle spawn codes live in the cooked `DT_VehicleTemplates` and are NOT recoverable from the Linux server
    // binary or the wiki API, so no vehicle row may be invented here. If that ever changes, this test changes with
    // it — deliberately.
    expect(entities.some((e) => /sandbike|buggy|ornithopter/i.test(e.name))).toBe(false);
  });
});

describe('the loader reports the catalogue honestly', () => {
  it('echoes the source so /health can tell a generated catalogue from a stub', () => {
    const catalogue = new Catalogue({ itemsFile: FIXTURE_ITEMS, entitiesFile: FIXTURE_ENTITIES });
    catalogue.load();
    expect(catalogue.status()).toMatchObject({ items: 3, itemsSource: 'test fixture', entities: 2 });
  });

  it('an absent catalogue is an empty list plus a warning, never a crash', () => {
    const catalogue = new Catalogue({ itemsFile: path.join(DATA_DIR, 'does-not-exist.json'), entitiesFile: '' });
    catalogue.load();
    expect(catalogue.listItems()).toEqual([]);
    expect(catalogue.status()).toMatchObject({ items: 0, itemsSource: 'none' });
  });

  it('listItems never returns a code as a name', async () => {
    const h = await harness();
    const items = (await h.adapter.handleAction('listItems', {})) as TakaroItem[];
    expect(items.length).toBeGreaterThan(0);
    for (const item of items) expect(isDevName(item.name, item.code)).toBe(false);
  });
});

/**
 * The generated catalogue is a gitignored build artefact, so this block only runs where the generator has been run
 * (the rig, and any dev box that ran `npm run catalogue`). Where it exists it must hold up to the same rule as the
 * fixtures — a real regression guard against a wiki edit that puts an asset id in a title field.
 */
const generated =
  fs.existsSync(path.join(DATA_DIR, 'items.json')) &&
  JSON.parse(fs.readFileSync(path.join(DATA_DIR, 'items.json'), 'utf8')).placeholder !== true;
describe.runIf(generated)('the generated catalogue on this machine', () => {
  const parsed = generated ? JSON.parse(fs.readFileSync(path.join(DATA_DIR, 'items.json'), 'utf8')) : { rows: [] };

  it('is large, licensed and attributed', () => {
    expect(parsed.rows.length).toBeGreaterThan(1000);
    expect(parsed.license).toBe('CC-BY-NC-SA-4.0');
    expect(parsed.sourceUrl).toMatch(/awakening\.wiki/);
    expect(fs.existsSync(path.join(DATA_DIR, 'SOURCES.md'))).toBe(true);
  });

  it('contains no dev-style name and no duplicate code', () => {
    expect(validate(parsed.rows, 'items')).toEqual([]);
  });
});

describe('fixMojibake', () => {
  it('repairs UTF-8 read back as Windows-1252, as one wiki item name was', () => {
    expect(fixMojibake('Cultivating Hufuf Vines in Artificial Environments \u00e2\u20ac\u201d E. Goetha')).toBe(
      'Cultivating Hufuf Vines in Artificial Environments \u2014 E. Goetha',
    );
    expect(fixMojibake('Caf\u00c3\u00a9')).toBe('Caf\u00e9');
  });
  it('leaves correct text alone', () => {
    for (const ok of ['A Dart for Every Man', 'Fila\u2019s Regret', 'Faufreluches \u2014 a dart', 'Caf\u00e9 \u00c2ge']) expect(fixMojibake(ok)).toBe(ok);
  });
});
