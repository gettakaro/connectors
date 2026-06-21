import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { DatabaseSync } from 'node:sqlite';
import { ConanItemCatalog } from '../conan/itemCatalog.js';
import { ConanSaveDbReader } from '../conan/saveDb.js';

test('reads Conan player position, inventory, item templates, and actor classes from save DB', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'conan-save-db-'));
  const dbPath = path.join(dir, 'game_0.db');
  const db = new DatabaseSync(dbPath);
  try {
    db.exec(`
      create table account (id integer, user text, online bool, platformId text);
      create table characters (playerId text, id bigint, char_name text, level integer, rank integer, guild bigint, isAlive boolean, killerName text, lastTimeOnline integer, killerId text, lastServerTimeOnline real);
      create table actor_position (class text, map text, id bigint, x double precision, y double precision, z double precision, sx double precision, sy double precision, sz double precision, rx double precision, ry double precision, rz double precision, rw double precision);
      create table item_inventory (item_id bigint, owner_id bigint, inv_type bigint, template_id bigint, data blob);
    `);
    db.prepare('insert into account values (?, ?, ?, ?)').run(1, 'A-USER', 1, '76561198000000001');
    db.prepare('insert into characters values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)').run('1', 109, 'TestExile', 9, null, null, 1, '', 1, '', 1);
    db.prepare('insert into actor_position values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)').run('BasePlayerChar_C', 'ConanSandbox', 109, 10, 20, 30, 1, 1, 1, 0, 0, 0, 1);
    db.prepare('insert into actor_position values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)').run('/Game/Foo/BP_Wolf.BP_Wolf_C', 'ConanSandbox', 200, 1, 2, 3, 1, 1, 1, 0, 0, 0, 1);
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(3, 109, 1, 10001, Buffer.from('/Script/ConanSandbox.GameItemResource'));
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(4, 109, 1, 10001, Buffer.from('/Script/ConanSandbox.GameItemResource'));
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(5, 109, 6, 1, Buffer.from('/Script/ConanSandbox.FeatItem'));
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(6, 109, 7, 1, Buffer.from('/Script/ConanSandbox.EmoteItem'));
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(8, 109, 1, 51204, Buffer.from('/Game/Items/BPGameItemWeapon.BPGameItemWeapon_C'));
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(9, 109, 1, 51205, Buffer.from('/Game/Items/BPGameItemWeapon.BPGameItemWeapon_C'));
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(10, 109, 1, 12001, Buffer.from('/Script/ConanSandbox.GameItem'));
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(7, 109, 1, 1001, Buffer.from('/Game/Items/BPGameItemWeapon.BPGameItemWeapon_C'));
  } finally {
    db.close();
  }

  const reader = new ConanSaveDbReader(dbPath);

  assert.deepEqual(reader.getPlayerLocation('steam:76561198000000001'), {
    x: 10,
    y: 20,
    z: 30,
    dimension: 'ConanSandbox',
  });
  assert.deepEqual(reader.getPlayerInventory('TestExile'), [
    {
      code: 'Stone',
      name: 'Stone',
      amount: 2,
      quality: '1',
      position: { x: 3, y: 1 },
    },
    {
      code: '1001',
      name: 'Conan item 1001',
      amount: 1,
      quality: '1',
      position: { x: 7, y: 1 },
    },
    {
      code: 'PlantFiber',
      name: 'Plant Fiber',
      amount: 1,
      quality: '1',
      position: { x: 10, y: 1 },
    },
  ]);
  assert.deepEqual(reader.listItems(), [
    { code: '1001', name: 'Conan item 1001' },
    { code: 'PlantFiber', name: 'Plant Fiber' },
    { code: 'Stone', name: 'Stone' },
  ]);
  assert.deepEqual(reader.listEntities(), [
    { code: '/Game/Foo/BP_Wolf.BP_Wolf_C', name: 'BP Wolf' },
    { code: 'BasePlayerChar_C', name: 'BasePlayerChar' },
  ]);
  assert.deepEqual(reader.listPlayerLocations(), [
    {
      code: 'player:109',
      name: 'TestExile',
      x: 10,
      y: 20,
      z: 30,
      dimension: 'ConanSandbox',
    },
  ]);

  rmSync(dir, { recursive: true, force: true });
});

test('uses item catalog names and codes for save DB inventory and listItems', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'conan-save-db-'));
  const dbPath = path.join(dir, 'game_0.db');
  const db = new DatabaseSync(dbPath);
  try {
    db.exec(`
      create table account (id integer, user text, online bool, platformId text);
      create table characters (playerId text, id bigint, char_name text, level integer, rank integer, guild bigint, isAlive boolean, killerName text, lastTimeOnline integer, killerId text, lastServerTimeOnline real);
      create table item_inventory (item_id bigint, owner_id bigint, inv_type bigint, template_id bigint, data blob);
    `);
    db.prepare('insert into account values (?, ?, ?, ?)').run(1, 'A-USER', 1, '76561198000000001');
    db.prepare('insert into characters values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)').run('1', 109, 'TestExile', 9, null, null, 1, '', 1, '', 1);
    db.prepare('insert into item_inventory values (?, ?, ?, ?, ?)').run(1, 109, 1, 41001, Buffer.from('/Script/ConanSandbox.GameItemResource'));
  } finally {
    db.close();
  }

  const catalog = new ConanItemCatalog(
    {
      version: 1,
      source: 'fixture',
      items: [{ templateId: '41001', code: 'Iron_Ore', name: 'Iron Ore', aliases: ['iron'] }],
    },
    { hasExternalCatalog: true },
  );
  const reader = new ConanSaveDbReader(dbPath, catalog);

  assert.deepEqual(reader.getPlayerInventory('TestExile'), [
    {
      code: 'Iron_Ore',
      name: 'Iron Ore',
      amount: 1,
      quality: '1',
      position: { x: 1, y: 1 },
    },
  ]);
  assert.deepEqual(reader.listItems(), [
    { code: 'Iron_Ore', name: 'Iron Ore' },
    { code: 'PlantFiber', name: 'Plant Fiber' },
    { code: 'Stone', name: 'Stone' },
  ]);

  rmSync(dir, { recursive: true, force: true });
});
