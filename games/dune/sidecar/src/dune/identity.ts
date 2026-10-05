import type { DunePlayerRow, TakaroPlayer } from './types.js';

/** SteamID64: 17 digits starting with 7656119. */
export const STEAMID64_RE = /^7656\d{13}$/;

/** An FLS id as the DB stores it: 16 uppercase hex characters (e.g. `6FF6498F4074E3DE`). */
export const FLS_ID_RE = /^[0-9A-Fa-f]{16}$/;

/** A Funcom id as `accounts.funcom_id` stores it: `<TAG>#<digits>` (e.g. `PLAYER#12345`, `ADMIN#00001`). */
export const FUNCOM_ID_RE = /^[A-Za-z0-9_]+#\d+$/;

export function isFlsId(value: unknown): boolean {
  return typeof value === 'string' && FLS_ID_RE.test(value.trim());
}

export function isFuncomId(value: unknown): boolean {
  return typeof value === 'string' && FUNCOM_ID_RE.test(value.trim());
}

export function isSteamId64(value: unknown): boolean {
  return typeof value === 'string' && STEAMID64_RE.test(value.trim());
}

/** Strips a `steam:` / `platform:` prefix. Takaro sends `platformId` as `steam:<id>`. */
export function stripPlatform(id: string): string {
  return id.replace(/^(?:steam|epic|xbox|psn):/i, '');
}

export function str(value: unknown): string | null {
  if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  if (typeof value !== 'string') return null;
  const t = value.trim();
  return t || null;
}

export function num(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && value.trim() !== '' && Number.isFinite(Number(value))) return Number(value);
  return null;
}

export type PlayerLike = Partial<DunePlayerRow> | Record<string, unknown>;

/**
 * Campaign plan, Decision 3: Takaro `gameId` is the **FLS id** (`encrypted_accounts."user"`), because that is the id
 * the GM command envelope and the chat routing keys speak. `steamId` is `accounts.platform_id` when
 * `platform_name` says steam, and `platformId` is then `steam:<id>`. The display name is `player_state.character_name`
 * — never the funcom id, never the FLS id, when a character name exists.
 *
 * `ip` and `ping` are deliberately never set: neither Postgres nor RabbitMQ exposes them, and inventing them would be
 * worse than leaving Takaro's optional fields empty.
 *
 * A row with no FLS id still yields a record (the funcom id, then the platform id, then the character name is used as
 * `gameId`), because Takaro rejects every "no player" answer to `getPlayer` with a user-visible 400 and needs a valid
 * IGamePlayer whatever we have (wire gotcha F7 / Dragonwilds L4b).
 */
export function mapPlayer(source: PlayerLike): TakaroPlayer {
  const row = source as Record<string, unknown>;
  const flsId = str(row.flsId);
  const funcomId = str(row.funcomId);
  const platformIdRaw = str(row.platformId);
  const characterName = str(row.characterName);
  const gameId = flsId ?? funcomId ?? platformIdRaw ?? characterName ?? str(row.gameId);
  if (!gameId) throw new Error(`Dune player row has no usable identifier: ${JSON.stringify(row)}`);

  const player: TakaroPlayer = { gameId, name: characterName ?? str(row.name) ?? gameId };
  const platformName = (str(row.platformName) ?? '').toLowerCase();
  const steamId = platformIdRaw && (platformName.includes('steam') || isSteamId64(platformIdRaw)) ? platformIdRaw : null;
  if (steamId) {
    player.steamId = steamId;
    player.platformId = `steam:${steamId}`;
  } else if (platformIdRaw) {
    // Unknown platform: keep the raw id visible but do not claim it is a Steam id. Takaro rejects the whole
    // IGamePlayer (and so the event or response carrying it) unless platformId is `<platform>:<id>` with both
    // halves in [A-Za-z0-9_-], so anything that cannot be expressed that way is left out rather than sent.
    const platformId = platformIdOf(platformName, platformIdRaw);
    if (platformId) player.platformId = platformId;
  }
  return player;
}

/** Takaro's IGamePlayer.platformId pattern. */
export const TAKARO_PLATFORM_ID_RE = /^[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+$/;

/** `<platform>:<id>` for a non-Steam account, or null when the pair cannot form a valid Takaro platformId. */
export function platformIdOf(platformName: string, rawId: string): string | null {
  const prefix = platformName.toLowerCase().replace(/[^a-z0-9_-]+/g, '');
  if (!prefix) return null;
  const candidate = `${prefix}:${rawId}`;
  return TAKARO_PLATFORM_ID_RE.test(candidate) ? candidate : null;
}

/** Every identifier a Takaro caller might name this player by; used to index the known-player cache. */
export function identifiersOf(source: PlayerLike, player: TakaroPlayer): string[] {
  const row = source as Record<string, unknown>;
  const out = [player.gameId, player.steamId, player.platformId, player.name, str(row.funcomId), str(row.flsId), str(row.platformId)];
  return out.filter((v): v is string => typeof v === 'string' && v.length > 0);
}

/** Which of the two ids a GM command's `PlayerId` field should carry. Community tools disagree; settled on the rig. */
export type PlayerIdKind = 'fls' | 'funcom' | 'name';

export function gmPlayerId(row: Partial<DunePlayerRow>, kind: PlayerIdKind): string | null {
  if (kind === 'funcom') return str(row.funcomId) ?? str(row.flsId);
  if (kind === 'name') return str(row.characterName) ?? str(row.flsId);
  return str(row.flsId) ?? str(row.funcomId);
}
