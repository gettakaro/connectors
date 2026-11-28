// Answers Takaro's legacy getMapInfo / getMapTile from the mod's world survey. Runs in the
// bridge, so tiles never touch the game thread.

import fs from 'node:fs';
import path from 'node:path';
import { logger } from '../logger.js';
import { encodePng } from './png.js';
import {
  FRAME_MAX_ZOOM,
  FRAME_MIN_ZOOM,
  FRAME_TILE_SIZE,
  MAP_BLOCK_SIZE,
  MAX_ZOOM,
  TILE_SIZE,
  isFrameTile,
  prepareSurvey,
  renderFrameTile,
  renderTile,
  tileInWorld,
  type PreparedSurvey,
} from './render.js';
import { parseSurvey } from './survey.js';

export interface MapInfo {
  enabled: boolean;
  mapBlockSize: number;
  maxZoom: number;
  mapSizeX: number;
  mapSizeY: number;
  mapSizeZ: number;
}

export const DISABLED_MAP_INFO: MapInfo = {
  enabled: false,
  mapBlockSize: 0,
  maxZoom: 0,
  mapSizeX: 0,
  mapSizeY: 0,
  mapSizeZ: 0,
};

/** Display names for the official worlds; any other world shows under its engine name. */
const WORLD_NAMES: Record<string, string> = {
  chernarusplus: 'Chernarus',
  enoch: 'Livonia',
  sakhal: 'Sakhal',
};

export function worldDisplayName(world: string): string {
  return WORLD_NAMES[world.toLowerCase()] ?? world;
}

/** A protocol v1 map Dimension. DayZ positions are metres, x east and z north, y is height. */
export interface MapDimension {
  id: string;
  name: string;
  axes: { horizontal: { field: 'x'; direction: 'right' }; vertical: { field: 'z'; direction: 'up' } };
  bounds: { horizontal: { min: number; max: number }; vertical: { min: number; max: number } };
  source: { type: 'tiled'; tileSize: number; minZoom: number; maxZoom: number };
}

export class MapTileError extends Error {
  constructor(
    readonly code: 'invalid_args' | 'not_found',
    message: string,
  ) {
    super(message);
  }
}

const RECHECK_MS = 30_000;
const CACHE_TILES = 2000;

export class MapService {
  private prepared: PreparedSurvey | null = null;
  private loadedFile = '';
  private loadedMtime = 0;
  private revision = ''; // survey file + mtime + size; part of every tile cache key
  private checkedAt = 0;
  private tiles = new Map<string, string>(); // key -> base64 PNG, insertion order = LRU
  private blank: string | null = null;

  constructor(
    private dir: string,
    private recheckMs = RECHECK_MS,
  ) {}

  /** Newest complete `<world>.survey` in the map directory, reloaded when it changes. */
  current(): PreparedSurvey | null {
    const now = Date.now();
    if (now - this.checkedAt < this.recheckMs && this.prepared) return this.prepared;
    this.checkedAt = now;
    let best: { file: string; mtime: number; size: number } | null = null;
    try {
      for (const name of fs.readdirSync(this.dir)) {
        if (!name.endsWith('.survey')) continue;
        const file = path.join(this.dir, name);
        const st = fs.statSync(file);
        if (!best || st.mtimeMs > best.mtime) best = { file, mtime: st.mtimeMs, size: st.size };
      }
    } catch {
      return this.prepared;
    }
    if (!best) return this.prepared;
    if (best.file === this.loadedFile && best.mtime === this.loadedMtime) return this.prepared;
    try {
      const started = Date.now();
      const survey = parseSurvey(fs.readFileSync(best.file, 'utf8'));
      this.prepared = prepareSurvey(survey);
      this.loadedFile = best.file;
      this.loadedMtime = best.mtime;
      this.revision = `${path.basename(best.file)}:${Math.round(best.mtime)}:${best.size}`;
      this.tiles.clear();
      logger.info(
        `Map survey loaded: ${survey.world} ${survey.size} m, ${survey.buildings.length} buildings, ` +
          `${survey.surfaceNames.length} surfaces, ${Date.now() - started} ms (${best.file}, revision ${this.revision})`,
      );
    } catch (err) {
      logger.warn(`Map survey ${best.file} not usable: ${(err as Error).message}`);
    }
    return this.prepared;
  }

  mapInfo(): MapInfo {
    const p = this.current();
    if (!p) return DISABLED_MAP_INFO;
    return {
      enabled: true,
      mapBlockSize: MAP_BLOCK_SIZE,
      maxZoom: MAX_ZOOM,
      mapSizeX: p.survey.size,
      // Vertical extent: smaller than the horizontal ones, which tells map viewers DayZ is Y-up.
      mapSizeY: Math.max(100, Math.ceil(p.survey.maxHeight / 100) * 100),
      mapSizeZ: p.survey.size,
    };
  }

  /** Base64 PNG for tile (x, y) at zoom z; a transparent tile outside the world. */
  tile(x: number, y: number, z: number): string {
    const p = this.current();
    if (!p) throw new Error('Map not available yet: the world survey has not finished (console: mapsurvey)');
    if (![x, y, z].every(Number.isInteger) || z < 0 || z > MAX_ZOOM) {
      throw new Error(`Invalid map tile ${z}/${x}/${y} (zoom 0..${MAX_ZOOM})`);
    }
    if (!tileInWorld(p.survey.size, z, x, y)) return this.blankTile();
    // Keyed by survey revision too, so a re-survey or wipe can never serve a tile of the old one.
    const key = `${this.revision}|${z}/${x}/${y}`;
    const hit = this.tiles.get(key);
    if (hit) {
      this.tiles.delete(key);
      this.tiles.set(key, hit);
      return hit;
    }
    const png = encodePng(TILE_SIZE, TILE_SIZE, renderTile(p, z, x, y)).toString('base64');
    this.tiles.set(key, png);
    if (this.tiles.size > CACHE_TILES) this.tiles.delete(this.tiles.keys().next().value!);
    return png;
  }

  /** The loaded world as a protocol v1 Dimension, or null before a survey has finished. */
  dimension(): MapDimension | null {
    const p = this.current();
    if (!p) return null;
    const size = p.survey.size;
    return {
      id: p.survey.world,
      name: worldDisplayName(p.survey.world),
      axes: { horizontal: { field: 'x', direction: 'right' }, vertical: { field: 'z', direction: 'up' } },
      bounds: { horizontal: { min: 0, max: size }, vertical: { min: 0, max: size } },
      source: { type: 'tiled', tileSize: FRAME_TILE_SIZE, minZoom: FRAME_MIN_ZOOM, maxZoom: FRAME_MAX_ZOOM },
    };
  }

  /** The protocol v1 getMapTile answer for the declared Dimension. */
  frameTile(dimension: unknown, zoom: number, x: number, y: number): { mimeType: 'image/png'; data: string } {
    const p = this.current();
    if (!p) throw new MapTileError('not_found', 'Map not available yet: the world survey has not finished (console: mapsurvey)');
    if (dimension !== p.survey.world) throw new MapTileError('not_found', `Unknown map dimension ${String(dimension)}`);
    if (!isFrameTile(zoom, x, y)) {
      throw new MapTileError('invalid_args', `Map tile ${zoom}/${x}/${y} is outside the declared pyramid (zoom ${FRAME_MIN_ZOOM}..${FRAME_MAX_ZOOM})`);
    }
    const key = `${this.revision}|v1|${zoom}/${x}/${y}`;
    let png = this.tiles.get(key);
    if (png) {
      this.tiles.delete(key);
    } else {
      png = encodePng(FRAME_TILE_SIZE, FRAME_TILE_SIZE, renderFrameTile(p, zoom, x, y)).toString('base64');
      if (this.tiles.size >= CACHE_TILES) this.tiles.delete(this.tiles.keys().next().value!);
    }
    this.tiles.set(key, png);
    return { mimeType: 'image/png', data: png };
  }

  /** Revision of the survey tiles are drawn from ('' before one is loaded). */
  surveyRevision(): string {
    this.current();
    return this.revision;
  }

  private blankTile(): string {
    if (!this.blank) this.blank = encodePng(TILE_SIZE, TILE_SIZE, new Uint8Array(TILE_SIZE * TILE_SIZE * 4)).toString('base64');
    return this.blank;
  }
}

/** getMapTile args: Takaro sends {x, y, z} (numbers or numeric strings). */
export function parseTileArgs(args: unknown): { x: number; y: number; z: number } {
  let obj: unknown = args;
  if (typeof args === 'string') obj = args ? JSON.parse(args) : {};
  const o = (obj ?? {}) as Record<string, unknown>;
  const num = (v: unknown): number => (typeof v === 'number' ? v : typeof v === 'string' && v.trim() !== '' ? Number(v) : NaN);
  return { x: num(o.x), y: num(o.y), z: num(o.z) };
}

/** Protocol v1 getMapTile args: {dimension, zoom, x, y}. */
export function parseFrameTileArgs(args: unknown): { dimension: unknown; zoom: number; x: number; y: number } {
  let obj: unknown = args;
  if (typeof args === 'string') obj = args ? JSON.parse(args) : {};
  const o = (obj ?? {}) as Record<string, unknown>;
  const num = (v: unknown): number => (typeof v === 'number' ? v : typeof v === 'string' && v.trim() !== '' ? Number(v) : NaN);
  const out = { dimension: o.dimension, zoom: num(o.zoom), x: num(o.x), y: num(o.y) };
  if (typeof out.dimension !== 'string' || ![out.zoom, out.x, out.y].every(Number.isInteger)) {
    throw new MapTileError('invalid_args', 'getMapTile needs { dimension, zoom, x, y }');
  }
  return out;
}

export function mapServiceFromEnv(env: NodeJS.ProcessEnv = process.env): MapService | null {
  const dir = env.DAYZ_MAP_DIR || (env.DAYZ_LOG_DIR ? path.join(env.DAYZ_LOG_DIR, 'TakaroIntegration', 'map') : '');
  return dir ? new MapService(dir) : null;
}
