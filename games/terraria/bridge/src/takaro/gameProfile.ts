import { createHash } from 'node:crypto';
import { CONNECTOR_VERSION, GAME_ICON_PNG_BASE64 } from '../generated/buildInfo.js';

/** What identify reports on both protocol versions. Both slugs are stable: Takaro groups releases by them. */
export const CONNECTOR_IDENTITY = {
  game: 'terraria',
  connector: { name: 'takaro-terraria', version: CONNECTOR_VERSION },
} as const;

export const GAME_DISPLAY_NAME = 'Terraria';

export interface GameIcon {
  revision: string;
  size: number;
  data: string;
}

/** The shipped icon; the revision is derived from its bytes, so new bytes always mean a new revision. */
export function gameIcon(base64: string = GAME_ICON_PNG_BASE64): GameIcon {
  const bytes = Buffer.from(base64, 'base64');
  return {
    revision: createHash('sha256').update(bytes).digest('hex').slice(0, 12),
    size: bytes.length,
    data: base64,
  };
}

const ICON = gameIcon();

/**
 * The running game build as TShock reports it (`serverversion`, for example "v1.4.5.8"),
 * or null when it is missing or does not fit the 32 printable ASCII characters Takaro accepts.
 */
export function normalizeGameVersion(raw: unknown): string | null {
  if (typeof raw !== 'string') return null;
  const version = raw.trim().replace(/^v(?=\d)/, '');
  return /^[\x20-\x7e]{1,32}$/.test(version) ? version : null;
}

/** The protocol v1 `game.profile` capability; `version` only once TShock has reported one. */
export function gameProfileCapability(gameVersion: string | null): { name: 'game.profile'; parameters: unknown } {
  return {
    name: 'game.profile',
    parameters: {
      name: GAME_DISPLAY_NAME,
      ...(gameVersion ? { version: gameVersion } : {}),
      icon: { revision: ICON.revision, size: ICON.size },
    },
  };
}

/** The getGameIcon response: the whole PNG of the declared revision, never chunked. */
export function gameIconResponse(): { revision: string; data: string } {
  return { revision: ICON.revision, data: ICON.data };
}
