import { createHash } from 'node:crypto';

/**
 * Takaro's IGamePlayer.platformId validator. A platformId that fails it makes Takaro reject the
 * whole event or getPlayers response.
 */
export const TAKARO_PLATFORM_ID = /^[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+$/;

const SAFE_KEY = /^[A-Za-z0-9_-]+$/;

/**
 * The one identity this connector gives a Terraria player, on every wire path.
 *
 * A Terraria server sees no Steam, GOG or Xbox account, only the character name (the TShock
 * REST player list, the chat/join log lines and the events plugin all agree on it). So gameId is
 * the character name everywhere and platformId is `terraria:<name>`. Names Takaro's platformId
 * pattern does not allow (spaces, dots, non-ASCII: common in Terraria) are made safe with `_`
 * and suffixed with 8 hex of their SHA-256, so two names that sanitise alike still differ.
 */
export function terrariaPlatformId(characterName: string): string {
  const name = characterName.trim();
  if (SAFE_KEY.test(name)) return `terraria:${name}`;
  const readable = name.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 32);
  const digest = createHash('sha256').update(name, 'utf8').digest('hex').slice(0, 8);
  return `terraria:${readable}-${digest}`;
}

export function terrariaIdentity(characterName: string): { gameId: string; platformId: string } {
  const gameId = characterName.trim();
  return { gameId, platformId: terrariaPlatformId(gameId) };
}
