import fs from 'node:fs/promises';
import path from 'node:path';

/** What the TShock plugin writes next to the image it rendered (map.json). */
export interface MapImageMeta {
  revision: string;
  file: string;
  mimeType: 'image/png' | 'image/jpeg' | 'image/webp';
  size: number;
  width: number;
  height: number;
  bounds: { x: { min: number; max: number }; y: { min: number; max: number } };
}

export const MAP_DIMENSION_ID = 'world';
export const MAX_CHUNK_BYTES = 1024 * 1024;

/** The protocol v1 `map` capability for the rendered world, or null while there is none. */
export function mapCapability(meta: MapImageMeta | null): { name: 'map'; parameters: unknown } | null {
  if (!meta) return null;
  return {
    name: 'map',
    parameters: {
      dimensions: [
        {
          id: MAP_DIMENSION_ID,
          name: 'World',
          // Terraria positions are world pixels: x grows to the right, y grows downwards.
          axes: {
            horizontal: { field: 'x', direction: 'right' },
            vertical: { field: 'y', direction: 'down' },
          },
          bounds: {
            horizontal: { min: meta.bounds.x.min, max: meta.bounds.x.max },
            vertical: { min: meta.bounds.y.min, max: meta.bounds.y.max },
          },
          source: {
            type: 'image',
            revision: meta.revision,
            size: meta.size,
            width: meta.width,
            height: meta.height,
            mimeType: meta.mimeType,
          },
        },
      ],
    },
  };
}

export class MapImageError extends Error {
  constructor(readonly code: 'invalid_args' | 'not_found' | 'game_error', message: string) {
    super(message);
  }
}

/**
 * The map image the plugin rendered into mapDir. The bridge only reads it: refresh() picks up
 * a new revision, chunk() serves the bytes of the revision it last declared.
 */
export class MapImageStore {
  private meta: MapImageMeta | null = null;

  constructor(private dir: string) {}

  setDir(dir: string): void {
    this.dir = dir;
  }

  current(): MapImageMeta | null {
    return this.meta;
  }

  /** Re-reads map.json. Returns true when the declared image changed. */
  async refresh(): Promise<boolean> {
    let next: MapImageMeta | null = null;
    try {
      next = parseMeta(JSON.parse(await fs.readFile(path.join(this.dir, 'map.json'), 'utf8')));
      if (next) {
        const stat = await fs.stat(path.join(this.dir, next.file));
        if (stat.size !== next.size) next = null;
      }
    } catch {
      next = null;
    }
    const changed = (next?.revision ?? null) !== (this.meta?.revision ?? null);
    // A missing file after a valid one is kept: the plugin replaces files by rename, so a
    // read can fall in between. Only a new valid revision replaces the declared one.
    if (next) this.meta = next;
    return changed && next !== null;
  }

  async chunk(rawArgs: unknown): Promise<{ revision: string; data: string }> {
    const args = typeof rawArgs === 'string' ? safeJson(rawArgs) : rawArgs;
    const { dimension, offset, length } = (args ?? {}) as Record<string, unknown>;
    if (dimension !== MAP_DIMENSION_ID) throw new MapImageError('not_found', `Unknown map dimension ${String(dimension)}`);
    if (!Number.isInteger(offset) || (offset as number) < 0) throw new MapImageError('invalid_args', 'offset must be an integer >= 0');
    if (!Number.isInteger(length) || (length as number) < 1 || (length as number) > MAX_CHUNK_BYTES) {
      throw new MapImageError('invalid_args', `length must be an integer from 1 to ${MAX_CHUNK_BYTES}`);
    }
    const meta = this.meta;
    if (!meta) throw new MapImageError('not_found', 'The world map has not been rendered yet');
    if ((offset as number) >= meta.size) throw new MapImageError('invalid_args', `offset ${offset} is past the end (${meta.size} bytes)`);
    const count = Math.min(length as number, meta.size - (offset as number));
    let handle: fs.FileHandle | null = null;
    try {
      handle = await fs.open(path.join(this.dir, meta.file), 'r');
      const buffer = Buffer.alloc(count);
      const { bytesRead } = await handle.read(buffer, 0, count, offset as number);
      if (bytesRead !== count) throw new Error(`short read ${bytesRead}/${count}`);
      return { revision: meta.revision, data: buffer.toString('base64') };
    } catch (err) {
      throw new MapImageError('game_error', `Map image ${meta.file} unreadable: ${err instanceof Error ? err.message : String(err)}`);
    } finally {
      await handle?.close();
    }
  }
}

function safeJson(raw: string): unknown {
  try {
    return JSON.parse(raw);
  } catch {
    return {};
  }
}

const MIME_TYPES = new Set(['image/png', 'image/jpeg', 'image/webp']);

export function parseMeta(value: unknown): MapImageMeta | null {
  if (!value || typeof value !== 'object') return null;
  const v = value as Record<string, unknown>;
  const bounds = v.bounds as MapImageMeta['bounds'] | undefined;
  const range = (r: unknown): boolean => {
    const { min, max } = (r ?? {}) as { min?: unknown; max?: unknown };
    return Number.isFinite(min) && Number.isFinite(max) && (min as number) < (max as number);
  };
  const pixels = (n: unknown): boolean => Number.isInteger(n) && (n as number) >= 1 && (n as number) <= 8192;
  if (typeof v.revision !== 'string' || !/^[\x21-\x7e]{1,128}$/.test(v.revision)) return null;
  // The file name comes from disk; never let it point outside mapDir.
  if (typeof v.file !== 'string' || v.file !== path.basename(v.file)) return null;
  if (typeof v.mimeType !== 'string' || !MIME_TYPES.has(v.mimeType)) return null;
  if (!Number.isInteger(v.size) || (v.size as number) < 1 || (v.size as number) > 32 * 1024 * 1024) return null;
  if (!pixels(v.width) || !pixels(v.height)) return null;
  if (!bounds || !range(bounds.x) || !range(bounds.y)) return null;
  return {
    revision: v.revision,
    file: v.file,
    mimeType: v.mimeType as MapImageMeta['mimeType'],
    size: v.size as number,
    width: v.width as number,
    height: v.height as number,
    bounds: { x: { min: bounds.x.min, max: bounds.x.max }, y: { min: bounds.y.min, max: bounds.y.max } },
  };
}
