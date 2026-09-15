import { asRecord, isGameEventType, type GameEventType } from '../takaro/protocol.js';
import type {
  PluginInventoryItem,
  PluginPlayer,
  Position,
  TakaroBan,
  TakaroEntity,
  TakaroEntityType,
  TakaroItem,
  TakaroLocation,
  TakaroPlayer,
} from './types.js';

/** Takaro gameId is the SteamID64 (stable across sessions); falls back to the plugin id when the plugin has no SteamID. */
export function mapPlayer(raw: unknown): TakaroPlayer {
  const p = asRecord(raw);
  // plugin gameId is the SteamID64 (docs/API.md); treat a 17-digit 7656... id as a SteamID when steamId is absent
  const steamId = str(p.steamId) ?? steamFromPlatform(str(p.platformId)) ?? steamFromGameId(str(p.gameId));
  const gameId = steamId ?? str(p.gameId) ?? str(p.name);
  if (!gameId) throw new Error(`Plugin player has no identifier: ${JSON.stringify(raw)}`);
  const player: TakaroPlayer = { gameId, name: str(p.name) ?? gameId };
  if (steamId) {
    player.steamId = steamId;
    player.platformId = `steam:${steamId}`;
  }
  if (str(p.ip)) player.ip = str(p.ip)!;
  if (typeof p.ping === 'number' && Number.isFinite(p.ping)) player.ping = p.ping;
  return player;
}

export function mapPosition(raw: unknown): Position {
  const p = asRecord(raw);
  const x = num(p.x);
  const y = num(p.y);
  const z = num(p.z);
  if (x === null || y === null || z === null) {
    throw new Error(`Plugin returned an invalid position: ${JSON.stringify(raw)}`);
  }
  const pos: Position = { x, y, z };
  if (str(p.dimension)) pos.dimension = str(p.dimension)!;
  return pos;
}

export function mapInventoryItem(raw: PluginInventoryItem | unknown): TakaroItem {
  const i = asRecord(raw);
  const code = str(i.code) ?? str(i.name);
  if (!code) throw new Error(`Plugin inventory item has no code: ${JSON.stringify(raw)}`);
  const item: TakaroItem = { code, name: str(i.name) ?? code, amount: num(i.amount) ?? 1 };
  if (i.quality !== undefined && i.quality !== null && i.quality !== '') item.quality = String(i.quality);
  return item;
}

export function mapItemDefinition(raw: unknown): TakaroItem {
  const i = asRecord(raw);
  const code = str(i.code) ?? str(i.name);
  if (!code) throw new Error(`Plugin item has no code: ${JSON.stringify(raw)}`);
  const item: TakaroItem = { code, name: str(i.name) ?? code };
  if (str(i.description)) item.description = str(i.description)!;
  return item;
}

export function mapEntityType(raw: unknown): TakaroEntityType {
  const t = String(raw ?? '').toLowerCase();
  if (['hostile', 'enemy', 'monster', 'aggressive', 'shroud'].some((k) => t.includes(k))) return 'hostile';
  if (['friendly', 'ally', 'npc', 'villager', 'survivor', 'companion', 'pet'].some((k) => t.includes(k))) return 'friendly';
  return 'neutral';
}

export function mapEntity(raw: unknown): TakaroEntity {
  const e = asRecord(raw);
  const code = str(e.code) ?? str(e.name);
  if (!code) throw new Error(`Plugin entity has no code: ${JSON.stringify(raw)}`);
  const entity: TakaroEntity = { code, name: str(e.name) ?? code, type: mapEntityType(e.type) };
  if (str(e.description)) entity.description = str(e.description)!;
  return entity;
}

export function mapLocation(raw: unknown): TakaroLocation {
  const l = asRecord(raw);
  const code = str(l.code) ?? str(l.name);
  if (!code) throw new Error(`Plugin location has no code: ${JSON.stringify(raw)}`);
  const position = mapPosition(l.position ?? l);
  const loc: TakaroLocation = { code, name: str(l.name) ?? code, position };
  for (const key of ['radius', 'sizeX', 'sizeY', 'sizeZ'] as const) {
    const v = num(l[key]);
    if (v !== null) loc[key] = v;
  }
  return loc;
}

export function mapBan(raw: unknown): TakaroBan {
  const b = asRecord(raw);
  const playerSource = Object.keys(asRecord(b.player)).length ? b.player : b;
  const expires = b.expiresAt;
  return {
    player: mapPlayer(playerSource),
    reason: str(b.reason) ?? '',
    expiresAt: typeof expires === 'string' && expires ? expires : typeof expires === 'number' ? new Date(expires).toISOString() : null,
  };
}

export interface MappedEvent {
  type: GameEventType;
  data: Record<string, unknown>;
}

/** Normalises a plugin /events entry into a Takaro gameEvent payload. Returns null for unknown types. */
export function mapPluginEvent(event: { type: string; data: unknown; ts?: unknown }): MappedEvent | null {
  if (!isGameEventType(event.type)) return null;
  const d = asRecord(event.data);
  const withPlayer = (): Record<string, unknown> => {
    const source = Object.keys(asRecord(d.player)).length ? d.player : d;
    return { player: mapPlayer(source) };
  };

  switch (event.type) {
    case 'player-connected':
    case 'player-disconnected':
      return { type: event.type, data: withPlayer() };
    case 'chat-message': {
      const msg = str(d.msg) ?? str(d.message) ?? str(d.text) ?? '';
      const out: Record<string, unknown> = { msg, channel: mapChannel(d.channel) };
      if (Object.keys(asRecord(d.player)).length) out.player = mapPlayer(d.player);
      return { type: event.type, data: out };
    }
    case 'player-death': {
      const out = withPlayer();
      if (Object.keys(asRecord(d.attacker)).length) {
        try {
          out.attacker = mapPlayer(d.attacker);
        } catch {
          /* non-player attacker */
        }
      }
      if (d.position) out.position = mapPosition(d.position);
      // Takaro's EventPlayerDeath only has a player `attacker`; a creature/NPC killer goes into the base `msg` field.
      const killerEntity = str(d.killerEntity) ?? str(asRecord(d.killer).code);
      if (!out.attacker && killerEntity) {
        const who = (out.player as TakaroPlayer).name;
        out.msg = `${who} was killed by ${killerEntity}`;
      }
      return { type: event.type, data: out };
    }
    case 'entity-killed': {
      const out = withPlayer();
      out.entity = str(d.entity) ?? str(asRecord(d.entity).code) ?? 'unknown';
      out.weapon = str(d.weapon) ?? '';
      return { type: event.type, data: out };
    }
    case 'log':
      return { type: 'log', data: { msg: str(d.msg) ?? str(d.message) ?? str(d.line) ?? (typeof event.data === 'string' ? event.data : JSON.stringify(event.data)) } };
  }
}

function mapChannel(raw: unknown): string {
  const c = String(raw ?? '').toLowerCase();
  if (c === 'team' || c === 'friends' || c === 'whisper') return c;
  return 'global';
}

function steamFromGameId(gameId: string | null): string | null {
  return gameId && /^7656\d{13}$/.test(gameId) ? gameId : null;
}

function steamFromPlatform(platformId: string | null): string | null {
  if (!platformId) return null;
  const m = /^steam:(\d+)$/i.exec(platformId);
  return m ? m[1] : null;
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
