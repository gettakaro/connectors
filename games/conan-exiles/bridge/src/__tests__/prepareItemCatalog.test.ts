import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

const packageRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

test('prepare-item-catalog converts DevKit exports into compact catalog JSON', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'conan-devkit-catalog-'));
  try {
    const itemTable = path.join(dir, 'ItemTable.json');
    const names = path.join(dir, 'ItemNameToTemplateID.json');
    const output = path.join(dir, 'catalog.json');

    writeFileSync(
      itemTable,
      JSON.stringify([
        {
          RowName: '41001',
          TemplateID: 41001,
          Name: 'NSLOCTEXT("ItemTable", "IronOre", "Iron Ore")',
          ItemClass: '/Game/Items/Resources/BPGameItemResource.BPGameItemResource_C',
          Category: 'Resources',
          Tags: ['ore', 'metal'],
        },
        {
          RowName: 'XX_Debug_Item',
          TemplateID: 99999,
          Name: 'NSLOCTEXT("ItemTable", "Debug", "Debug Item")',
        },
      ]),
    );
    writeFileSync(
      names,
      JSON.stringify([
        { RowName: 'Iron_Ore', TemplateID: 41001 },
        { RowName: 'DEV_Test_Item', TemplateID: 99999 },
      ]),
    );

    const result = spawnSync(process.execPath, ['scripts/prepare-item-catalog.mjs', itemTable, names, output], {
      cwd: packageRoot,
      encoding: 'utf8',
    });

    assert.equal(result.status, 0, result.stderr || result.stdout);
    assert.deepEqual(JSON.parse(readFileSync(output, 'utf8')), {
      version: 1,
      source: 'DevKit ItemTable.json + ItemNameToTemplateID.json',
      items: [
        {
          templateId: '41001',
          code: 'Iron_Ore',
          name: 'Iron Ore',
          itemClass: '/Game/Items/Resources/BPGameItemResource.BPGameItemResource_C',
          category: 'Resources',
          tags: ['ore', 'metal'],
          aliases: ['41001', 'Iron Ore'],
        },
      ],
    });
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
