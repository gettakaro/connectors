// Map tiles from a synthetic world survey: parse, render, tile scheme, v0 answers.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { inflateSync } from 'node:zlib';
import { parseSurvey, SurveyError } from '../dist/map/survey.js';
import { classifySurface, prepareSurvey, renderTile, tileInWorld, tileUnit, TILE_SIZE, MAX_ZOOM, MAP_BLOCK_SIZE } from '../dist/map/render.js';
import { encodePng, PNG_SIGNATURE } from '../dist/map/png.js';
import { MapService, parseTileArgs, DISABLED_MAP_INFO } from '../dist/map/service.js';

// A 1024 m world, 32 cells of 32 m: sea in the south quarter (rows 0..7), a hill in the
// north-east, a road along row 16, and one 20x10 m building at (512, 512) facing east.
function syntheticSurvey({ complete = true } = {}) {
  const cols = 32, cell = 32, size = 1024, treecell = 64, tcols = 16;
  const hex4 = (v) => v.toString(16).padStart(4, '0');
  const lines = ['takaro-dayz-survey 1', `world testland size ${size} cell ${cell} cols ${cols} rows ${cols} treecell ${treecell}`];
  for (let r = 0; r < cols; r++) {
    let h = '', s = '', w = '';
    for (let c = 0; c < cols; c++) {
      const sea = r < 8;
      const height = sea ? -10 : 5 + Math.max(0, 200 - Math.hypot(c - 24, r - 24) * 20);
      h += hex4(Math.round((height + 100) * 10));
      s += sea ? '0' : r === 16 ? '3' : c < 16 ? '1' : '2';
      w += sea ? '1' : '0';
    }
    lines.push(`h ${r} ${h}`, `s ${r} ${s}`, `w ${r} ${w}`);
  }
  lines.push('b 5120 5120 1000 0 -100 -50 100 50');
  for (let r = 0; r < tcols; r++) lines.push(`t ${r} ${(r > 10 ? '20' : '00').repeat(tcols)}`);
  lines.push('S 0 sea_floor', 'S 1 cp_grass', 'S 2 cp_conifer_common1', 'S 3 asphalt_ext');
  if (complete) lines.push('end 1 80');
  return lines.join('\n') + '\n';
}

const CODES_AT = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!#$%&()*+,-/:;<=>?@'.length - 1;
const px = (rgba, x, y) => Array.from(rgba.slice((y * TILE_SIZE + x) * 4, (y * TILE_SIZE + x) * 4 + 4));

test('decodes surface codes past the first 62', () => {
  const text = syntheticSurvey().replace('S 3 asphalt_ext', 'S 3 asphalt_ext\nS @ cp_conifer_common1');
  const s = parseSurvey(text.replace(/^s 20 (.)/m, 's 20 @'));
  assert.equal(s.surfaceNames[CODES_AT], 'cp_conifer_common1');
  assert.equal(s.scenery[20 * 32], CODES_AT);
});

test('parses a complete survey', () => {
  const s = parseSurvey(syntheticSurvey());
  assert.equal(s.world, 'testland');
  assert.equal(s.size, 1024);
  assert.equal(s.cols, 32);
  assert.equal(s.heights[0], -10);
  assert.ok(s.maxHeight > 150);
  assert.equal(s.water[0], 1);
  assert.equal(s.water[8 * 32], 0);
  assert.equal(s.scenery[16 * 32 + 5], 3);
  assert.equal(s.scenery[15 * 32 + 5], 1);
  assert.deepEqual(s.surfaceNames, ['sea_floor', 'cp_grass', 'cp_conifer_common1', 'asphalt_ext']);
  assert.equal(s.buildings.length, 1);
  assert.deepEqual(s.buildings[0], { x: 512, z: 512, fx: 1, fz: 0, minX: -10, minZ: -5, maxX: 10, maxZ: 5 });
  assert.equal(s.trees[11 * 16], 0x20);
});

test('refuses an unfinished survey', () => {
  assert.throws(() => parseSurvey(syntheticSurvey({ complete: false })), SurveyError);
  assert.throws(() => parseSurvey('nope\n'), SurveyError);
});

test('classifies DayZ surface names', () => {
  assert.equal(classifySurface('cp_conifer_common1'), 'forest');
  assert.equal(classifySurface('cp_broadleaf_dense2'), 'forest');
  assert.equal(classifySurface('cp_grass'), 'grass');
  assert.equal(classifySurface('asphalt_ext'), 'paved');
  assert.equal(classifySurface('gravel_large_ext'), 'track');
  assert.equal(classifySurface('dirt_ext'), 'track');
  assert.equal(classifySurface('asphalt_felt_ext'), 'unknown');
  assert.equal(classifySurface('cp_gravel'), 'gravel');
  assert.equal(classifySurface('cp_grass_tall'), 'meadow');
  assert.equal(classifySurface('fresh_water_ext'), 'water');
  assert.equal(classifySurface('cp_rock'), 'rock');
  assert.equal(classifySurface(undefined), 'unknown');
});

test('tile scheme: x east, y north, MAP_BLOCK_SIZE metres per tile at MAX_ZOOM', () => {
  assert.equal(tileUnit(MAX_ZOOM), MAP_BLOCK_SIZE);
  assert.equal(tileUnit(0), MAP_BLOCK_SIZE * 16);
  assert.ok(tileInWorld(1024, MAX_ZOOM, 0, 0));
  assert.ok(tileInWorld(1024, MAX_ZOOM, 3, 3));
  assert.ok(!tileInWorld(1024, MAX_ZOOM, 4, 0));
  assert.ok(!tileInWorld(1024, MAX_ZOOM, 0, -1));
  assert.ok(tileInWorld(1024, 0, 0, 0));
});

test('renders sea in the south, land in the north and the building where it stands', () => {
  const p = prepareSurvey(parseSurvey(syntheticSurvey()));
  // Tile (0,0) at zoom 4 covers east 0..256, north 0..256: sea (rows 0..7 = north 0..256).
  const sea = renderTile(p, 4, 0, 0);
  const [r, g, b, a] = px(sea, 64, 64);
  assert.equal(a, 255);
  assert.ok(b > r && b > g, `sea pixel should be blue, got ${[r, g, b]}`);
  // Tile (2,2) covers east 512..768, north 512..768. The building faces east, so its 10 m
  // forward extent runs east (507..517) and its 20 m right extent runs north (502..522).
  // Pixel (1, 125) is east 515, north 517: inside. Pixel (4, 125) is east 521: outside.
  const land = renderTile(p, 4, 2, 2);
  assert.deepEqual(px(land, 1, 125), [112, 92, 84, 255]);
  assert.notDeepEqual(px(land, 4, 125), [112, 92, 84, 255]);
  // North-west of the building is plain land, not building colour.
  assert.notDeepEqual(px(land, 60, 40), [112, 92, 84, 255]);
  // Outside the world: transparent.
  assert.equal(px(renderTile(p, 4, 9, 9), 0, 0)[3], 0);
  // North is up: in tile (0,1) (north 256..512) the bottom row is nearer the sea than the top.
  const coast = renderTile(p, 4, 0, 1);
  assert.equal(px(coast, 10, 0)[3], 255);
});

test('encodes a valid PNG', () => {
  const rgba = new Uint8Array(4 * 3 * 2).fill(200);
  const png = encodePng(3, 2, rgba);
  assert.ok(png.subarray(0, 8).equals(PNG_SIGNATURE));
  assert.equal(png.readUInt32BE(16), 3);
  assert.equal(png.readUInt32BE(20), 2);
  const idatLen = png.readUInt32BE(33);
  const raw = inflateSync(png.subarray(41, 41 + idatLen));
  assert.equal(raw.length, (3 * 4 + 1) * 2);
});

test('MapService answers getMapInfo and getMapTile from the newest survey', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dayz-map-'));
  const svc = new MapService(dir);
  assert.deepEqual(svc.mapInfo(), DISABLED_MAP_INFO);
  assert.throws(() => svc.tile(0, 0, 0), /survey has not finished/);
  fs.writeFileSync(path.join(dir, 'testland.survey.partial'), syntheticSurvey({ complete: false }));
  const svc2 = new MapService(dir);
  assert.equal(svc2.mapInfo().enabled, false);
  fs.writeFileSync(path.join(dir, 'testland.survey'), syntheticSurvey());
  const svc3 = new MapService(dir);
  const info = svc3.mapInfo();
  assert.deepEqual(info, { enabled: true, mapBlockSize: 256, maxZoom: 4, mapSizeX: 1024, mapSizeY: 300, mapSizeZ: 1024 });
  assert.ok(info.mapSizeY < info.mapSizeZ, 'Y must read as the up axis');
  const tile = Buffer.from(svc3.tile(1, 1, 4), 'base64');
  assert.ok(tile.subarray(0, 8).equals(PNG_SIGNATURE));
  assert.equal(svc3.tile(1, 1, 4), svc3.tile(1, 1, 4));
  const blank = Buffer.from(svc3.tile(50, 50, 4), 'base64');
  assert.ok(blank.subarray(0, 8).equals(PNG_SIGNATURE));
  assert.throws(() => svc3.tile(0, 0, 5), /Invalid map tile/);
  assert.throws(() => svc3.tile(Number.NaN, 0, 1), /Invalid map tile/);
  fs.rmSync(dir, { recursive: true });
});

test('MapService never mixes tiles of an old and a new survey', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dayz-map-'));
  const file = path.join(dir, 'testland.survey');
  fs.writeFileSync(file, syntheticSurvey());
  const svc = new MapService(dir, 0);
  const before = svc.tile(2, 2, 4); // holds the building at (512, 512)
  const rev = svc.surveyRevision();
  assert.ok(rev.startsWith('testland.survey:'));
  // Re-survey: the building is gone; the file is newer and a different size.
  fs.writeFileSync(file, syntheticSurvey().replace(/^b .*\n/m, ''));
  const later = new Date(Date.now() + 5000);
  fs.utimesSync(file, later, later);
  const after = svc.tile(2, 2, 4);
  assert.notEqual(svc.surveyRevision(), rev);
  assert.notEqual(after, before, 'tile must be drawn from the new survey, not the cache');
  assert.equal(svc.tile(2, 2, 4), after);
  fs.rmSync(dir, { recursive: true });
});

test('parseTileArgs accepts JSON strings and numeric strings', () => {
  assert.deepEqual(parseTileArgs('{"x":1,"y":-2,"z":"3"}'), { x: 1, y: -2, z: 3 });
  assert.deepEqual(parseTileArgs({ x: '0', y: 0, z: 0 }), { x: 0, y: 0, z: 0 });
  assert.ok(Number.isNaN(parseTileArgs('{}').x));
});
