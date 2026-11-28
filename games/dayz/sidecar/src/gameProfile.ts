// Who this connector is: the game and connector slugs sent on identify, and the protocol v1
// `game.profile` capability with the DayZ server build and the game icon.

import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';

const PACKAGE_JSON = new URL('../package.json', import.meta.url);
// The release tarball ships the icon next to dist/; in the repository it is games/dayz/assets.
const ICON_PNGS = [new URL('../assets/icon-256.png', import.meta.url), new URL('../../assets/icon-256.png', import.meta.url)];

/** The connector version the release build stamped into package.json. */
export function connectorVersion(packageJson: URL = PACKAGE_JSON): string {
  try {
    const v = (JSON.parse(readFileSync(packageJson, 'utf8')) as { version?: unknown }).version;
    return typeof v === 'string' && v !== '' ? v : '0.0.0';
  } catch {
    return '0.0.0';
  }
}

/** Sent on identify; both slugs are stable because Takaro groups releases by them. */
export const CONNECTOR_IDENTITY = {
  game: 'dayz',
  connector: { name: 'takaro-dayz', version: connectorVersion() },
} as const;

export const GAME_DISPLAY_NAME = 'DayZ';

export interface GameIcon {
  revision: string;
  size: number;
  data: string;
}

/** The shipped icon, or null when the file is missing. The revision follows the bytes. */
export function loadGameIcon(paths: URL[] = ICON_PNGS): GameIcon | null {
  let bytes: Buffer | null = null;
  for (const path of paths) {
    try {
      bytes = readFileSync(path);
      break;
    } catch {
      /* try the next place */
    }
  }
  if (!bytes) return null;
  return {
    revision: createHash('sha256').update(bytes).digest('hex').slice(0, 12),
    size: bytes.length,
    data: bytes.toString('base64'),
  };
}

/**
 * The server build the mod reports (CGame.GetVersion, for example "1.30.164289"), or null when
 * it is missing or does not fit the 32 printable ASCII characters Takaro accepts.
 */
export function normalizeGameVersion(raw: unknown): string | null {
  if (typeof raw !== 'string') return null;
  const v = raw.trim();
  return /^[\x20-\x7e]{1,32}$/.test(v) ? v : null;
}

/** The protocol v1 `game.profile` capability; `version` once the mod has reported one. */
export function gameProfileCapability(gameVersion: string | null, icon: GameIcon | null): { name: 'game.profile'; parameters: unknown } {
  return {
    name: 'game.profile',
    parameters: {
      name: GAME_DISPLAY_NAME,
      ...(gameVersion ? { version: gameVersion } : {}),
      ...(icon ? { icon: { revision: icon.revision, size: icon.size } } : {}),
    },
  };
}
