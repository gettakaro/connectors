// takaro-xnb-export: Texture2D assets out of XNA content files (.xnb), as PNG, for the icon pipeline.
//
// Runs inside the decoder container: `node /app/export.mjs /work/request.json` with the game files mounted
// read-only at the request's `root` and a writable /out. Nothing here knows a game.
//
// Request:
//   {"schemaVersion": 1, "decoder": "xnb", "settings": {}, "root": "/game",
//    "exports": [{"id": "it5644", "kind": "texture", "file": "Content/Images/Item_5644.xnb"}]}
//
// Files are read one at a time. The pixels are written as decoded (no alpha conversion); the PNG is RGBA8.
// A failure of one export is recorded in the manifest and the tool still exits 0; a request failure exits non-zero.
import fs from 'node:fs';
import path from 'node:path';
import * as XNB from 'xnb';
import { encodePng } from './png.mjs';

const OUT = '/out';
const MAX_EDGE = 4096;
const ID_PATTERN = /^[a-z0-9][a-z0-9-]{0,63}$/;
const SURFACE_COLOR = 0;
const xnbVersion = JSON.parse(
  fs.readFileSync(new URL('./node_modules/xnb/package.json', import.meta.url), 'utf8'),
).version;

// The library reports progress on stdout; the protocol keeps stdout empty and logs to stderr.
console.log = (...parts) => console.error(...parts);

class Failure extends Error {
  constructor(message, kind = 'failed') {
    super(message);
    this.kind = kind;
  }
}

function resolveInside(root, relative) {
  if (typeof relative !== 'string' || relative === '' || path.isAbsolute(relative) || relative.split('/').includes('..')) {
    throw new Failure(`refusing file path ${JSON.stringify(relative)}`);
  }
  const resolved = path.resolve(root, relative);
  if (!resolved.startsWith(path.resolve(root) + path.sep)) {
    throw new Failure(`${relative} resolves outside ${root}`);
  }
  return resolved;
}

function readRegularFile(root, relative) {
  const resolved = resolveInside(root, relative);
  let info;
  try {
    info = fs.lstatSync(resolved);
  } catch {
    throw new Failure(relative, 'not-found');
  }
  if (!info.isFile()) {
    throw new Failure(`${relative} is not a regular file`);
  }
  return fs.readFileSync(resolved);
}

function texture(root, request) {
  const bytes = readRegularFile(root, request.file);
  // An exact ArrayBuffer, never the pooled Buffer: a Node Buffer's backing store is shared with other buffers.
  const exact = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  let data;
  try {
    data = XNB.bufferToXnb(exact);
  } catch (error) {
    throw new Failure(`${request.file} is not a readable XNB: ${error.message}`);
  }
  const image = data.content?.export;
  if (!image || image.type !== 'Texture2D') {
    throw new Failure(`${request.file} holds ${image?.type ?? 'no'} content, not Texture2D`, 'unsupported-format');
  }
  if (data.content.format !== SURFACE_COLOR) {
    throw new Failure(`${request.file} has SurfaceFormat ${data.content.format}`, 'unsupported-format');
  }
  const { width, height } = image;
  if (!(width > 0 && height > 0 && width <= MAX_EDGE && height <= MAX_EDGE)) {
    throw new Failure(`${request.file} is ${width}x${height}, outside 1..${MAX_EDGE}`);
  }
  const png = `png/${request.id}.png`;
  fs.writeFileSync(path.join(OUT, png), encodePng(width, height, image.data), { flag: 'wx' });
  return {
    id: request.id,
    ok: true,
    file: png,
    meta: {
      width,
      height,
      format: 'Color',
      xnbVersion: data.header.formatVersion,
      platform: data.header.target,
      compressed: Boolean(data.header.compressed & 0x80),
    },
  };
}

function main() {
  if (process.argv.length !== 3) {
    console.error('usage: export.mjs <request.json>');
    return 2;
  }
  const request = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
  if (request.schemaVersion !== 1 || request.decoder !== 'xnb') {
    console.error("unsupported request: schemaVersion 1 and decoder 'xnb' are required");
    return 2;
  }
  const seen = new Set();
  for (const entry of request.exports) {
    if (!ID_PATTERN.test(entry.id) || seen.has(entry.id)) {
      console.error(`invalid or duplicate export id '${entry.id}'`);
      return 2;
    }
    seen.add(entry.id);
  }
  fs.mkdirSync(path.join(OUT, 'png'), { recursive: true });
  const results = [];
  for (const entry of request.exports) {
    try {
      if (entry.kind !== 'texture') {
        throw new Failure(`unknown export kind '${entry.kind}'`);
      }
      results.push(texture(request.root, entry));
    } catch (error) {
      const kind = error instanceof Failure ? error.kind : 'failed';
      results.push({ id: entry.id, ok: false, error: String(error.message ?? error), errorKind: kind });
    }
  }
  const manifest = {
    schemaVersion: 1,
    tool: { name: 'takaro-xnb-export', version: '1', xnb: xnbVersion, node: process.version },
    results,
  };
  fs.writeFileSync(path.join(OUT, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
  console.error(`exported ${results.filter((result) => result.ok).length} of ${results.length} textures`);
  return 0;
}

process.exitCode = main();
