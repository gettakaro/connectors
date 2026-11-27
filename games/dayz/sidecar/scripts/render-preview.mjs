#!/usr/bin/env node
// Renders a whole-world overview PNG from a map survey, using the same code as the bridge's
// getMapTile. Usage: node scripts/render-preview.mjs <world.survey> <out.png> [zoom=1]
import fs from 'node:fs';
import { parseSurvey } from '../dist/map/survey.js';
import { prepareSurvey, renderTile, tileUnit, TILE_SIZE } from '../dist/map/render.js';
import { encodePng } from '../dist/map/png.js';

const [file, out, zoomArg] = process.argv.slice(2);
if (!file || !out) {
  console.error('usage: render-preview.mjs <world.survey> <out.png> [zoom=1]');
  process.exit(2);
}
const zoom = Number(zoomArg ?? 1);
const p = prepareSurvey(parseSurvey(fs.readFileSync(file, 'utf8')));
const n = Math.ceil(p.survey.size / tileUnit(zoom));
const W = n * TILE_SIZE;
const img = new Uint8Array(W * W * 4);
for (let ty = 0; ty < n; ty++) {
  for (let tx = 0; tx < n; tx++) {
    const tile = renderTile(p, zoom, tx, ty);
    // tile y grows north; image rows grow south
    const top = (n - 1 - ty) * TILE_SIZE;
    for (let r = 0; r < TILE_SIZE; r++) {
      img.set(tile.subarray(r * TILE_SIZE * 4, (r + 1) * TILE_SIZE * 4), ((top + r) * W + tx * TILE_SIZE) * 4);
    }
  }
}
fs.writeFileSync(out, encodePng(W, W, img));
const names = p.survey.surfaceNames.map((s, i) => `${s}=${p.sceneryClass[i]}`).join(' ');
console.log(`${out}: ${W}x${W} px, zoom ${zoom}, ${p.survey.buildings.length} buildings; surfaces: ${names}`);
