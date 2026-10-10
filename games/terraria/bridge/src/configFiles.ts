import { randomUUID } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { SHIPPED_VALUES, parseKeyValues } from './config.js';

export const CONFIG_FILE_NAME = 'TakaroConfig.txt';

/**
 * Where the settings live.
 *
 * `userPath` is the file people edit: `BRIDGE_CONFIG` when set, else `TakaroConfig.txt` in
 * the working directory when one is there (where releases up to 0.2 read it), else the one
 * the release zip ships inside the bridge folder.
 *
 * `savedPath` is the bridge's own copy, kept next to the bridge folder rather than in it,
 * because the README's upgrade replaces the whole folder and the shipped file with it. It is
 * only used while the user file sits inside the bridge folder; a file elsewhere survives an
 * upgrade on its own.
 */
export interface ConfigLocation {
  userPath: string;
  savedPath: string | null;
}

/** The bridge folder: the one holding dist/ and package.json. */
export function bridgeRoot(): string {
  return path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
}

export function locateConfig(
  env: NodeJS.ProcessEnv = process.env,
  cwd = process.cwd(),
  root = bridgeRoot(),
): ConfigLocation {
  let userPath: string;
  if (env.BRIDGE_CONFIG) userPath = path.resolve(cwd, env.BRIDGE_CONFIG);
  else if (fs.existsSync(path.join(cwd, CONFIG_FILE_NAME))) userPath = path.resolve(cwd, CONFIG_FILE_NAME);
  else userPath = path.join(root, CONFIG_FILE_NAME);

  const inside = path.relative(root, userPath);
  const insideRoot = inside !== '' && !inside.startsWith('..') && !path.isAbsolute(inside);
  const savedPath = insideRoot
    ? path.join(path.dirname(root), `${path.basename(root)}.saved-config.txt`)
    : null;
  return { userPath, savedPath };
}

export type IdentitySource = 'config' | 'saved' | 'serverName' | 'generated';

export interface Resolution {
  /** The file settings in effect (the environment is applied later, by buildConfig). */
  values: Record<string, string>;
  identitySource: IdentitySource;
  /** Keys to write into the user file: the identity, and settings restored after an upgrade. */
  userWrites: Record<string, string>;
  /** Keys whose value came from the saved copy because the user file still held the shipped one. */
  restored: string[];
}

const TOKEN_KEY = 'registrationToken';
const IDENTITY_KEY = 'identityToken';

/**
 * Merges the user file with the saved copy.
 *
 * - The registration token comes from the user file when it sets one, else the saved copy,
 *   unless the user file emptied it since the previous read. It is never written back into
 *   the user file.
 * - The identity comes from the user file, else the saved copy. Neither: a file without
 *   `configFormat` predates this layout, and those bridges identified as `serverName`, so
 *   that stays their identity; a current-format file gets a new UUID.
 * - Any other setting the saved copy holds is restored only while the user file has no
 *   identity (just unpacked by an upgrade) and still holds the shipped value or nothing;
 *   a value the operator emptied (an allowlist) is restored empty.
 *   Once the identity is written back the user file is authoritative again, so setting
 *   something back to its default there takes effect.
 */
export function resolveFiles(
  userText: string,
  savedText: string | null,
  newIdentity: () => string = randomUUID,
  previousUserText: string | null = null,
): Resolution {
  const user = parseKeyValues(userText);
  const saved = savedText == null ? null : parseKeyValues(savedText);
  const values: Record<string, string> = { ...user };
  const userWrites: Record<string, string> = {};
  const restored: string[] = [];

  const unpacked = !user[IDENTITY_KEY];
  if (unpacked && saved) {
    for (const [key, value] of Object.entries(saved)) {
      if (key === TOKEN_KEY || key === IDENTITY_KEY) continue;
      const current = user[key];
      if (current && current !== SHIPPED_VALUES[key]) continue;
      if (current === value) continue;
      values[key] = value;
      userWrites[key] = value;
      restored.push(key);
    }
  }

  // Emptied while the bridge runs: the operator is taking the token away, so the saved one
  // must not come back.
  const cleared = previousUserText !== null && !user[TOKEN_KEY] && Boolean(parseKeyValues(previousUserText)[TOKEN_KEY]);
  values[TOKEN_KEY] = user[TOKEN_KEY] || (cleared ? '' : saved?.[TOKEN_KEY]) || '';

  let identitySource: IdentitySource;
  if (user[IDENTITY_KEY]) {
    identitySource = 'config';
  } else if (saved?.[IDENTITY_KEY]) {
    identitySource = 'saved';
    values[IDENTITY_KEY] = saved[IDENTITY_KEY];
  } else if (!('configFormat' in user)) {
    identitySource = 'serverName';
    values[IDENTITY_KEY] = user.serverName || SHIPPED_VALUES.serverName;
  } else {
    identitySource = 'generated';
    values[IDENTITY_KEY] = newIdentity();
  }
  if (identitySource !== 'config') userWrites[IDENTITY_KEY] = values[IDENTITY_KEY];

  return { values, identitySource, userWrites, restored };
}

export const SAVED_HEADER = [
  '# Written by the Takaro Terraria bridge: the settings it uses, kept outside the bridge',
  '# folder so that replacing the folder on an upgrade keeps the registration token and this',
  "# server's identity in Takaro. Edit TakaroConfig.txt inside the bridge folder instead; a",
  '# registrationToken set there takes precedence over this file.',
];

/**
 * The saved copy after a read: what the user file holds over what the copy held, so a key a
 * half-written or trimmed user file lacks is not lost from the copy an upgrade restores from.
 */
export function mergeSaved(saved: Record<string, string> | null, values: Record<string, string>): Record<string, string> {
  return { ...(saved ?? {}), ...values };
}

export function renderSaved(values: Record<string, string>): string {
  const lines = [...SAVED_HEADER, ''];
  for (const [key, value] of Object.entries(values)) lines.push(`${key}=${value}`);
  return `${lines.join('\n')}\n`;
}

/**
 * Sets keys in a key=value text, keeping its comments and order: the last line for a key
 * is replaced, a missing key is appended.
 */
export function setKeys(text: string, writes: Record<string, string>): string {
  const newline = text.includes('\r\n') ? '\r\n' : '\n';
  const lines = text.length ? text.split(/\r?\n/) : [];
  const pending = new Map(Object.entries(writes));
  // From the end: parseKeyValues lets the last line for a key win.
  for (let i = lines.length - 1; i >= 0 && pending.size; i--) {
    const trimmed = lines[i].trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const eq = trimmed.indexOf('=');
    if (eq < 0) continue;
    const key = trimmed.slice(0, eq).trim();
    if (!pending.has(key)) continue;
    lines[i] = `${key}=${pending.get(key)}`;
    pending.delete(key);
  }
  if (pending.size) {
    while (lines.length && lines[lines.length - 1] === '') lines.pop();
    for (const [key, value] of pending) lines.push(`${key}=${value}`);
    lines.push('');
  }
  return lines.join(newline);
}

/**
 * Replaces a file so a reader never sees it half-written. A file that cannot be renamed over
 * (a single-file docker bind mount answers EBUSY) is written in place instead.
 */
export function writeFileAtomic(file: string, text: string, mode?: number): void {
  const tmp = `${file}.tmp-${process.pid}`;
  // The file holds tokens: a replacement keeps the owner's permissions and drops group and
  // other access.
  let keep = mode;
  try {
    keep = fs.statSync(file).mode & 0o700;
  } catch {
    // A new file gets the mode asked for.
  }
  try {
    fs.writeFileSync(tmp, text, keep === undefined ? undefined : { mode: keep });
    if (keep !== undefined) fs.chmodSync(tmp, keep);
    fs.renameSync(tmp, file);
  } catch (err) {
    try {
      fs.rmSync(tmp, { force: true });
    } catch {
      // Nothing to clean up.
    }
    const code = (err as NodeJS.ErrnoException).code;
    if (code !== 'EBUSY' && code !== 'EXDEV' && code !== 'EPERM') throw err;
    fs.writeFileSync(file, text);
  }
}

export function readIfExists(file: string | null): string | null {
  if (!file) return null;
  try {
    return fs.readFileSync(file, 'utf8');
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === 'ENOENT') return null;
    throw err;
  }
}
