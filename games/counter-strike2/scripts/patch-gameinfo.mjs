import { chmodSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';

const metamodLine = /^[\t ]*Game[\t ]+csgo\/addons\/metamod[\t ]*(?:\/\/[^\r\n]*)?\r?$/m;
const csgoLine = /^([\t ]*)Game[\t ]+csgo[\t ]*(?:\/\/[^\r\n]*)?\r?$/m;

export function wireMetamodSearchPath(value) {
  if (typeof value !== 'string') throw new TypeError('gameinfo content must be text');
  if (metamodLine.test(value)) return { status: 'present', content: value };

  const anchor = csgoLine.exec(value);
  if (!anchor) throw new Error('gameinfo.gi is missing an exact Game csgo search path');
  const newline = value.includes('\r\n') ? '\r\n' : '\n';
  const insertion = `${anchor[1]}Game csgo/addons/metamod${newline}`;
  return {
    status: 'inserted',
    content: `${value.slice(0, anchor.index)}${insertion}${value.slice(anchor.index)}`
  };
}

export function inspectMetamodSearchPath(value) {
  return wireMetamodSearchPath(value).status === 'present' ? 'present' : 'missing';
}

function inspectFile(file) {
  return inspectMetamodSearchPath(readFileSync(file, 'utf8'));
}

function wireFile(file) {
  const result = wireMetamodSearchPath(readFileSync(file, 'utf8'));
  if (result.status === 'present') return result.status;

  const mode = statSync(file).mode & 0o777;
  const temporary = `${file}.tmp-${process.pid}`;
  try {
    writeFileSync(temporary, result.content, { mode });
    chmodSync(temporary, mode);
    renameSync(temporary, file);
  } catch (error) {
    try { unlinkSync(temporary); } catch {}
    throw error;
  }
  return result.status;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [, , command, file] = process.argv;
  if (!file || !['inspect-file', 'wire-file'].includes(command)) {
    throw new Error('Usage: gameinfo-writer.mjs <inspect-file|wire-file> <gameinfo.gi>');
  }
  process.stdout.write(command === 'inspect-file' ? inspectFile(file) : wireFile(file));
}
