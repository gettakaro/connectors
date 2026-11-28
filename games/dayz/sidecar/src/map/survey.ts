// Reads the world survey the mod writes to $profile:TakaroIntegration/map/<world>.survey
// (format documented in src/takaro_integration/scripts/4_World/TakaroMapSurvey.c).

export interface Building {
  x: number; // world metres, east
  z: number; // world metres, north
  fx: number; // unit forward vector, east component
  fz: number; // unit forward vector, north component
  minX: number; // local box, metres (x = right, z = forward)
  minZ: number;
  maxX: number;
  maxZ: number;
}

export interface Survey {
  world: string;
  size: number; // world edge, metres
  cell: number; // terrain sample spacing, metres
  cols: number; // samples per row (rows == cols)
  heights: Float32Array; // row-major, row 0 = south, sample centred at (col + 0.5) * cell
  scenery: Uint8Array; // ground surface, index into surfaceNames, 255 = unknown
  water: Uint8Array; // 0 land, 1 sea, 2 pond
  treeCell: number;
  treeCols: number;
  trees: Uint8Array; // trees per tree cell, row 0 = south
  surfaceNames: string[];
  buildings: Building[];
  maxHeight: number;
}

const CODES = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!#$%&()*+,-/:;<=>?@[]^_`{|}';

export class SurveyError extends Error {}

export function parseSurvey(text: string): Survey {
  const lines = text.split('\n');
  if (lines[0]?.trim() !== 'takaro-dayz-survey 1') throw new SurveyError('not a takaro-dayz-survey 1 file');
  const head = (lines[1] ?? '').trim().split(/\s+/);
  const field = (name: string): string => {
    const i = head.indexOf(name);
    if (i < 0 || i + 1 >= head.length) throw new SurveyError(`header lacks ${name}`);
    return head[i + 1]!;
  };
  const world = field('world');
  const size = Number(field('size'));
  const cell = Number(field('cell'));
  const cols = Number(field('cols'));
  const treeCell = Number(field('treecell'));
  if (!(size > 0 && cell > 0 && cols > 0 && treeCell > 0)) throw new SurveyError('bad header numbers');
  const treeCols = Math.ceil(size / treeCell);

  const n = cols * cols;
  const heights = new Float32Array(n);
  const scenery = new Uint8Array(n).fill(255);
  const water = new Uint8Array(n);
  const trees = new Uint8Array(treeCols * treeCols);
  const surfaceNames: string[] = [];
  const buildings: Building[] = [];
  let maxHeight = 0;
  let ended = false;
  const rowsSeen = new Set<number>();

  const codeIndex = (c: string): number => {
    const i = CODES.indexOf(c);
    return i < 0 ? 255 : i;
  };

  for (let li = 2; li < lines.length; li++) {
    const line = lines[li]!.replace(/\r$/, '');
    if (!line) continue;
    const kind = line[0];
    const sp1 = line.indexOf(' ');
    const sp2 = line.indexOf(' ', sp1 + 1);
    if (kind === 'S') {
      const idx = codeIndex(line.slice(sp1 + 1, sp2));
      if (idx !== 255) surfaceNames[idx] = line.slice(sp2 + 1).trim();
      continue;
    }
    if (line.startsWith('end')) {
      ended = true;
      continue;
    }
    if (kind === 'b') {
      const v = line.split(' ').slice(1).map(Number);
      if (v.length < 8 || v.some((x) => !Number.isFinite(x))) continue;
      const len = Math.hypot(v[2]!, v[3]!) || 1;
      buildings.push({
        x: v[0]! / 10,
        z: v[1]! / 10,
        fx: v[2]! / len,
        fz: v[3]! / len,
        minX: v[4]! / 10,
        minZ: v[5]! / 10,
        maxX: v[6]! / 10,
        maxZ: v[7]! / 10,
      });
      continue;
    }
    const row = Number(line.slice(sp1 + 1, sp2));
    const data = line.slice(sp2 + 1);
    if (kind === 't') {
      if (!(row >= 0 && row < treeCols)) continue;
      for (let c = 0; c < treeCols && c * 2 + 1 < data.length; c++) {
        trees[row * treeCols + c] = parseInt(data.slice(c * 2, c * 2 + 2), 16) || 0;
      }
      continue;
    }
    if (!(row >= 0 && row < cols)) continue;
    const base = row * cols;
    if (kind === 'h') {
      rowsSeen.add(row);
      for (let c = 0; c < cols && c * 4 + 3 < data.length; c++) {
        const h = parseInt(data.slice(c * 4, c * 4 + 4), 16) / 10 - 100;
        heights[base + c] = h;
        if (h > maxHeight) maxHeight = h;
      }
    } else if (kind === 's') {
      for (let c = 0; c < cols && c < data.length; c++) {
        const ch = data[c]!;
        scenery[base + c] = ch === '.' ? 255 : codeIndex(ch);
      }
    } else if (kind === 'w') {
      for (let c = 0; c < cols && c < data.length; c++) water[base + c] = data.charCodeAt(c) - 48;
    }
  }
  if (!ended) throw new SurveyError('survey is incomplete (no end line)');
  if (rowsSeen.size !== cols) throw new SurveyError(`survey has ${rowsSeen.size}/${cols} terrain rows`);
  return {
    world,
    size,
    cell,
    cols,
    heights,
    scenery,
    water,
    treeCell,
    treeCols,
    trees,
    surfaceNames,
    buildings,
    maxHeight,
  };
}
