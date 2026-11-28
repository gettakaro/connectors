// Draws Takaro map tiles from a world survey: terrain colour by ground surface and tree
// density, hillshade, contour lines, sea and ponds, roads and building footprints.
//
// Tile scheme (Takaro's legacy getMapTile, the 7 Days to Die web map): 128 px tiles, tile x grows
// east and tile y grows north, and at MAX_ZOOM one tile covers MAP_BLOCK_SIZE metres. DayZ is Y-up
// (x east, z north, metres, origin at the south-west corner), so tile (x, y) at zoom z covers
// east x*unit .. (x+1)*unit and north y*unit .. (y+1)*unit, with unit = MAP_BLOCK_SIZE * 2^(MAX_ZOOM - z).

import type { Building, Survey } from './survey.js';

export const TILE_SIZE = 128;
export const MAX_ZOOM = 4;
export const MAP_BLOCK_SIZE = 256; // metres per tile at MAX_ZOOM: 2 m per pixel

export type SurfaceClass = 'forest' | 'grass' | 'meadow' | 'field' | 'rock' | 'sand' | 'gravel' | 'paved' | 'track' | 'water' | 'unknown';

/**
 * Ground class for a DayZ surface name (CfgSurfaces, e.g. cp_grass, cp_broadleaf_dense1,
 * asphalt_ext). Chernarus and Livonia paint roads into the ground layer, so road surfaces come
 * back as 'paved' (asphalt, concrete) or 'track' (dirt and gravel roads).
 */
export function classifySurface(name: string | undefined): SurfaceClass {
  if (!name) return 'unknown';
  const n = name.toLowerCase().replace(/\*$/, '');
  if (/(water)/.test(n)) return 'water';
  if (/^(asphalt(?!_felt)|concrete_ext|cp_concrete|tarmac|cobble|pavement)/.test(n)) return 'paved';
  if (/^(dirt_ext|gravel_large_ext|gravel_small_ext|gravel_ext|mud_ext)/.test(n)) return 'track';
  if (/(forest|conifer|broadleaf|wood_floor|leaves|needle|spruce|pine)/.test(n)) return 'forest';
  if (/(rock|stone|cliff|boulder)/.test(n)) return 'rock';
  if (/(sand|beach|pebble)/.test(n)) return 'sand';
  if (/(field|farm|crop|wheat|plough|plow)/.test(n)) return 'field';
  if (/(grass_tall|grass_dry|heather|flower|meadow)/.test(n)) return 'meadow';
  if (/(grass|lawn|moss)/.test(n)) return 'grass';
  if (/(gravel|dirt|soil|ground)/.test(n)) return 'gravel';
  return 'unknown';
}

type RGB = [number, number, number];

const GROUND: Record<SurfaceClass, RGB> = {
  forest: [126, 158, 104],
  grass: [178, 200, 138],
  meadow: [196, 204, 142],
  field: [214, 206, 150],
  rock: [170, 164, 152],
  sand: [226, 214, 172],
  gravel: [188, 192, 160],
  paved: [146, 142, 136],
  track: [184, 154, 110],
  water: [138, 182, 210],
  unknown: [182, 196, 146],
};
const FOREST: RGB = [92, 130, 80];
const SEA: RGB = [150, 190, 214];
const SEA_DEEP: RGB = [96, 142, 180];
const POND: RGB = [138, 182, 210];
const BUILDING: RGB = [112, 92, 84];
const CONTOUR: RGB = [120, 96, 64];

export interface PreparedSurvey {
  survey: Survey;
  sceneryClass: SurfaceClass[]; // by surface index
  buckets: Map<number, Building[]>;
  bucketSize: number;
  bucketCols: number;
}

export function prepareSurvey(survey: Survey): PreparedSurvey {
  const classes = survey.surfaceNames.map((s) => classifySurface(s));
  const bucketSize = 256;
  const bucketCols = Math.ceil(survey.size / bucketSize);
  const buckets = new Map<number, Building[]>();
  for (const b of survey.buildings) {
    const bx = Math.floor(b.x / bucketSize);
    const bz = Math.floor(b.z / bucketSize);
    const key = bz * bucketCols + bx;
    let list = buckets.get(key);
    if (!list) buckets.set(key, (list = []));
    list.push(b);
  }
  return { survey, sceneryClass: classes, buckets, bucketSize, bucketCols };
}

export function tileUnit(zoom: number): number {
  return MAP_BLOCK_SIZE * 2 ** (MAX_ZOOM - zoom);
}

/** True when tile (x, y) at zoom overlaps the world square [0, size). */
export function tileInWorld(size: number, zoom: number, x: number, y: number): boolean {
  const unit = tileUnit(zoom);
  return x * unit < size && (x + 1) * unit > 0 && y * unit < size && (y + 1) * unit > 0;
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

function mix(a: RGB, b: RGB, t: number): RGB {
  return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
}

export function renderTile(p: PreparedSurvey, zoom: number, x: number, y: number): Uint8Array {
  if (zoom < 0 || zoom > MAX_ZOOM || !tileInWorld(p.survey.size, zoom, x, y)) return new Uint8Array(TILE_SIZE * TILE_SIZE * 4);
  const unit = tileUnit(zoom);
  return renderRegion(p, x * unit, (y + 1) * unit, unit, TILE_SIZE, zoom);
}

/**
 * Protocol v1 tiled source: a standard XYZ pyramid over the declared bounds [0, size] on both
 * axes. Zoom z has 2^z by 2^z tiles of FRAME_TILE_SIZE px; tile row 0 is the north edge.
 */
export const FRAME_TILE_SIZE = 256;
export const FRAME_MIN_ZOOM = 0;
export const FRAME_MAX_ZOOM = 5;

export function frameTileSpan(size: number, zoom: number): number {
  return size / 2 ** zoom;
}

export function isFrameTile(zoom: number, x: number, y: number): boolean {
  return (
    Number.isInteger(zoom) && Number.isInteger(x) && Number.isInteger(y) &&
    zoom >= FRAME_MIN_ZOOM && zoom <= FRAME_MAX_ZOOM && x >= 0 && y >= 0 && x < 2 ** zoom && y < 2 ** zoom
  );
}

export function renderFrameTile(p: PreparedSurvey, zoom: number, x: number, y: number): Uint8Array {
  if (!isFrameTile(zoom, x, y)) return new Uint8Array(FRAME_TILE_SIZE * FRAME_TILE_SIZE * 4);
  const span = frameTileSpan(p.survey.size, zoom);
  // Detail (contour spacing) follows metres per pixel, like the legacy zoom level with the same scale.
  const legacyZoom = clamp(Math.round(MAX_ZOOM - Math.log2(span / FRAME_TILE_SIZE / (MAP_BLOCK_SIZE / TILE_SIZE))), 0, MAX_ZOOM);
  return renderRegion(p, x * span, p.survey.size - y * span, span, FRAME_TILE_SIZE, legacyZoom);
}

/** Draws the square east0..east0+unit by north1-unit..north1 (metres) into a tilePx by tilePx RGBA tile. */
function renderRegion(p: PreparedSurvey, east0: number, north1: number, unit: number, tilePx: number, zoom: number): Uint8Array {
  const s = p.survey;
  const out = new Uint8Array(tilePx * tilePx * 4);
  const mpp = unit / tilePx;
  const cols = s.cols;
  const cell = s.cell;

  const idx = (c: number, r: number): number => clamp(r, 0, cols - 1) * cols + clamp(c, 0, cols - 1);
  const height = (e: number, n: number): number => {
    const gx = e / cell - 0.5;
    const gz = n / cell - 0.5;
    const c0 = Math.floor(gx);
    const r0 = Math.floor(gz);
    const fx = gx - c0;
    const fz = gz - r0;
    const h00 = s.heights[idx(c0, r0)]!;
    const h10 = s.heights[idx(c0 + 1, r0)]!;
    const h01 = s.heights[idx(c0, r0 + 1)]!;
    const h11 = s.heights[idx(c0 + 1, r0 + 1)]!;
    return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
  };
  const waterAt = (e: number, n: number, kind: number): number => {
    const gx = e / cell - 0.5;
    const gz = n / cell - 0.5;
    const c0 = Math.floor(gx);
    const r0 = Math.floor(gz);
    const fx = gx - c0;
    const fz = gz - r0;
    const w = (c: number, r: number): number => (s.water[idx(c, r)] === kind ? 1 : 0);
    return (w(c0, r0) * (1 - fx) + w(c0 + 1, r0) * fx) * (1 - fz) + (w(c0, r0 + 1) * (1 - fx) + w(c0 + 1, r0 + 1) * fx) * fz;
  };
  // Share of the four nearest samples whose ground is road class `cls`, bilinear weighted.
  const roadAt = (e: number, n: number, cls: SurfaceClass): number => {
    const gx = e / cell - 0.5;
    const gz = n / cell - 0.5;
    const c0 = Math.floor(gx);
    const r0 = Math.floor(gz);
    const fx = gx - c0;
    const fz = gz - r0;
    const w = (c: number, r: number): number => (p.sceneryClass[s.scenery[idx(c, r)]!] === cls ? 1 : 0);
    return (w(c0, r0) * (1 - fx) + w(c0 + 1, r0) * fx) * (1 - fz) + (w(c0, r0 + 1) * (1 - fx) + w(c0 + 1, r0 + 1) * fx) * fz;
  };
  const treeDensity = (e: number, n: number): number => {
    // trees per 100 m², bilinear over the tree grid
    const tc = s.treeCell;
    const gx = e / tc - 0.5;
    const gz = n / tc - 0.5;
    const c0 = Math.floor(gx);
    const r0 = Math.floor(gz);
    const fx = gx - c0;
    const fz = gz - r0;
    const tcols = s.treeCols;
    const t = (c: number, r: number): number => s.trees[clamp(r, 0, tcols - 1) * tcols + clamp(c, 0, tcols - 1)]!;
    const v = (t(c0, r0) * (1 - fx) + t(c0 + 1, r0) * fx) * (1 - fz) + (t(c0, r0 + 1) * (1 - fx) + t(c0 + 1, r0 + 1) * fx) * fz;
    return (v * 100) / (tc * tc);
  };

  const contourStep = zoom >= 3 ? 10 : zoom === 2 ? 20 : 50;
  const slopeStep = Math.max(cell, mpp);

  for (let py = 0; py < tilePx; py++) {
    const n = north1 - (py + 0.5) * mpp;
    for (let px = 0; px < tilePx; px++) {
      const e = east0 + (px + 0.5) * mpp;
      const o = (py * tilePx + px) * 4;
      if (e < 0 || n < 0 || e >= s.size || n >= s.size) continue;
      const h = height(e, n);
      let rgb: RGB;
      const sea = waterAt(e, n, 1);
      const pond = waterAt(e, n, 2);
      if (sea >= 0.5) {
        rgb = mix(SEA, SEA_DEEP, clamp(-h / 40, 0, 1));
      } else if (pond >= 0.5) {
        rgb = POND;
      } else {
        const sample = idx(Math.floor(e / cell), Math.floor(n / cell));
        let sc = p.sceneryClass[s.scenery[sample]!] ?? 'unknown';
        // Roads: draw where the bilinear road share passes one half, so diagonals stay smooth
        // instead of following the 10 m sample grid.
        const paved = roadAt(e, n, 'paved');
        const track = roadAt(e, n, 'track');
        const roadShare = Math.max(paved, track);
        if (sc === 'paved' || sc === 'track') sc = 'unknown';
        rgb = GROUND[sc];
        const forest = roadShare >= 0.5 ? 0 : clamp(treeDensity(e, n) / 1.2, 0, 1);
        if (forest > 0) rgb = mix(rgb, FOREST, forest);
        // hillshade, light from the north-west
        const dx = (height(e + slopeStep, n) - height(e - slopeStep, n)) / (2 * slopeStep);
        const dz = (height(e, n + slopeStep) - height(e, n - slopeStep)) / (2 * slopeStep);
        const len = Math.hypot(dx, dz, 1);
        const shade = (dx * 0.577 - dz * 0.577 + 0.577) / len; // dot(normal, light)
        const k = clamp(0.55 + shade * 0.75, 0.6, 1.15);
        rgb = [rgb[0] * k, rgb[1] * k, rgb[2] * k];
        if (roadShare > 0.35) {
          const roadRgb = paved >= track ? GROUND.paved : GROUND.track;
          rgb = mix(rgb, roadRgb, clamp((roadShare - 0.35) / 0.3, 0, 1));
        }
        // contour lines about one pixel wide
        const grad = Math.hypot(dx, dz) * mpp;
        const level = h / contourStep;
        const frac = level - Math.floor(level);
        const width = Math.max(grad / contourStep, 0.0001);
        if (frac < width && grad < contourStep) {
          const major = Math.floor(level) % 5 === 0;
          rgb = mix(rgb, CONTOUR, major ? 0.55 : 0.3);
        }
      }
      out[o] = clamp(Math.round(rgb[0]), 0, 255);
      out[o + 1] = clamp(Math.round(rgb[1]), 0, 255);
      out[o + 2] = clamp(Math.round(rgb[2]), 0, 255);
      out[o + 3] = 255;
    }
  }
  drawBuildings(p, out, east0, north1, unit, mpp, tilePx);
  return out;
}

function drawBuildings(p: PreparedSurvey, out: Uint8Array, east0: number, north1: number, unit: number, mpp: number, tilePx: number): void {
  const margin = 60;
  const bs = p.bucketSize;
  const bx0 = Math.max(0, Math.floor((east0 - margin) / bs));
  const bx1 = Math.min(p.bucketCols - 1, Math.floor((east0 + unit + margin) / bs));
  const bz0 = Math.max(0, Math.floor((north1 - unit - margin) / bs));
  const bz1 = Math.min(p.bucketCols - 1, Math.floor((north1 + margin) / bs));
  for (let bz = bz0; bz <= bz1; bz++) {
    for (let bx = bx0; bx <= bx1; bx++) {
      const list = p.buckets.get(bz * p.bucketCols + bx);
      if (!list) continue;
      for (const b of list) fillBuilding(b, out, east0, north1, mpp, tilePx);
    }
  }
}

function fillBuilding(b: Building, out: Uint8Array, east0: number, north1: number, mpp: number, tilePx: number): void {
  // local axes: forward f = (fx, fz), right r = (fz, -fx) in (east, north)
  const rx = b.fz;
  const rz = -b.fx;
  const radius = Math.hypot(Math.max(-b.minX, b.maxX), Math.max(-b.minZ, b.maxZ));
  const pxMin = Math.max(0, Math.floor((b.x - radius - east0) / mpp));
  const pxMax = Math.min(tilePx - 1, Math.floor((b.x + radius - east0) / mpp));
  const pyMin = Math.max(0, Math.floor((north1 - (b.z + radius)) / mpp));
  const pyMax = Math.min(tilePx - 1, Math.floor((north1 - (b.z - radius)) / mpp));
  if (pxMin > pxMax || pyMin > pyMax) return;
  let painted = false;
  for (let py = pyMin; py <= pyMax; py++) {
    const n = north1 - (py + 0.5) * mpp;
    for (let px = pxMin; px <= pxMax; px++) {
      const e = east0 + (px + 0.5) * mpp;
      const dx = e - b.x;
      const dz = n - b.z;
      const lx = dx * rx + dz * rz;
      const lz = dx * b.fx + dz * b.fz;
      if (lx < b.minX || lx > b.maxX || lz < b.minZ || lz > b.maxZ) continue;
      const o = (py * tilePx + px) * 4;
      out[o] = BUILDING[0];
      out[o + 1] = BUILDING[1];
      out[o + 2] = BUILDING[2];
      out[o + 3] = 255;
      painted = true;
    }
  }
  // Buildings smaller than a pixel still show as one dot where the map is detailed enough.
  if (!painted && mpp <= 4) {
    const px = Math.floor((b.x - east0) / mpp);
    const py = Math.floor((north1 - b.z) / mpp);
    if (px >= 0 && py >= 0 && px < tilePx && py < tilePx) {
      const o = (py * tilePx + px) * 4;
      out[o] = BUILDING[0];
      out[o + 1] = BUILDING[1];
      out[o + 2] = BUILDING[2];
      out[o + 3] = 255;
    }
  }
}
