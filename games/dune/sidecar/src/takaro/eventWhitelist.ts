import type { GameEventType } from './protocol.js';

/**
 * Takaro's gameEvent DTOs are STRICTLY whitelisted and an extra field kills the whole event.
 *
 * Measured on the live rig 2026-09-21: a perfectly good `entity-killed` (plugin seq 3, "Scavenger Thug", killed by
 * TakaroTest) was thrown away with
 *
 *   An instance of EventEntityKilled has failed the validation:
 *    - property entityCode has failed the following constraints: whitelistValidation
 *
 * because the sidecar attached its own debugging fields (`entityCode`, `nameIsClassName`, `weaponCode`,
 * `attribution`). The cause is in Takaro's own source: inbound game events are validated with
 * `{ forbidNonWhitelisted: true, forbidUnknownValues: true }` (`app-connector/src/lib/GameServerManager.ts`), unlike
 * the response DTOs (`listItems` and friends), which merely drop unknown keys.
 *
 * So this module is the single chokepoint every outgoing gameEvent passes through. Sources may carry as much context
 * as they like internally — it is useful in the debug log and in the plugin's own `/events` record — but exactly these
 * keys reach the wire. When a future Takaro version adds a field, it is added here once; when one of ours leaks, the
 * unit test fails instead of a player's kill disappearing in silence.
 *
 * Field lists transcribed from `packages/lib-modules/src/dto/gameEvents.ts` + `dto/base.ts`.
 */

/** `BaseEvent` + `BaseGameEvent`: every event may carry these. `type` is added by the frame builder, not the payload. */
export const BASE_EVENT_FIELDS = ['timestamp', 'msg'] as const;

/**
 * `IGamePlayer`. `platformId` additionally has to match `^[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+$`. `character` needs a Takaro
 * version with Characters support: earlier versions reject the whole event for it.
 */
export const PLAYER_FIELDS = ['gameId', 'name', 'steamId', 'epicOnlineServicesId', 'xboxLiveId', 'platformId', 'ip', 'ping', 'character'] as const;

/** `IPosition`. */
export const POSITION_FIELDS = ['x', 'y', 'z', 'dimension'] as const;

/** Per-event fields on top of `BASE_EVENT_FIELDS`, and which of them are nested DTOs. */
export const EVENT_FIELDS: Record<GameEventType, readonly string[]> = {
  log: [],
  'player-connected': ['player'],
  'player-disconnected': ['player'],
  'chat-message': ['player', 'channel', 'recipient', 'msg'],
  'player-death': ['player', 'attacker', 'position'],
  'entity-killed': ['player', 'entity', 'weapon'],
};

const PLAYER_KEYS: Record<string, readonly string[]> = {
  player: PLAYER_FIELDS,
  attacker: PLAYER_FIELDS,
  recipient: PLAYER_FIELDS,
  position: POSITION_FIELDS,
};

export interface SanitizedEvent {
  data: unknown;
  /** Dotted paths of the keys that were stripped, for the debug log and the tests. */
  removed: string[];
}

/** Every key this event type may carry at the top level. */
export function allowedFields(type: GameEventType): string[] {
  // Deduped: `chat-message` re-declares `msg` (it is required there rather than optional).
  return [...new Set([...BASE_EVENT_FIELDS, ...(EVENT_FIELDS[type] ?? [])])];
}

/**
 * Strips every key Takaro's DTO for `type` does not declare, at the top level and inside the nested `player` /
 * `attacker` / `recipient` / `position` objects. An unknown event type is passed through untouched — the frame builder
 * would not have accepted it anyway.
 */
export function sanitizeGameEvent(type: GameEventType, data: unknown): SanitizedEvent {
  const fields = EVENT_FIELDS[type];
  if (!fields || data === null || typeof data !== 'object' || Array.isArray(data)) return { data, removed: [] };
  const allowed = new Set(allowedFields(type));
  const removed: string[] = [];
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(data as Record<string, unknown>)) {
    if (!allowed.has(key)) {
      removed.push(key);
      continue;
    }
    const nested = PLAYER_KEYS[key];
    if (nested && value !== null && typeof value === 'object' && !Array.isArray(value)) {
      const allowedNested = new Set(nested);
      const inner: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
        if (allowedNested.has(k)) inner[k] = v;
        else removed.push(`${key}.${k}`);
      }
      out[key] = inner;
      continue;
    }
    out[key] = value;
  }
  return { data: out, removed };
}
