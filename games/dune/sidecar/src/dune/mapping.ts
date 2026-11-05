import { logger } from '../logger.js';
import { asRecord, isGameEventType, type GameEventType } from '../takaro/protocol.js';
import { mapPlayer, num, str } from './identity.js';
import type {
  DuneInventoryRow,
  DuneProgressionRows,
  Position,
  TakaroBan,
  TakaroEntity,
  TakaroEntityType,
  TakaroItem,
  TakaroLocation,
  TakaroPlayer,
  TakaroProgressionTrack,
} from './types.js';

export { mapPlayer, num, str } from './identity.js';

export function mapPosition(raw: unknown): Position {
  const p = asRecord(raw);
  // Dune speaks UE's uppercase vector fields on the wire (`{"X":..,"Y":..,"Z":..}`) and lowercase in our own code.
  const x = num(p.x) ?? num(p.X);
  const y = num(p.y) ?? num(p.Y);
  const z = num(p.z) ?? num(p.Z);
  if (x === null || y === null || z === null) throw new Error(`Not a position: ${JSON.stringify(raw)}`);
  const pos: Position = { x, y, z };
  if (str(p.dimension)) pos.dimension = str(p.dimension)!;
  return pos;
}

/** Tolerant variant for wire payloads where the position may simply be missing. */
export function maybePosition(raw: unknown): Position | null {
  try {
    return mapPosition(raw);
  } catch {
    return null;
  }
}

/**
 * One `dune.items` row → a Takaro IItemDTO. `code` is the item's `template_id` (the stable FName, and exactly what
 * `AddItemToInventory.ItemName` takes); `name` is the display name from the catalogue when we have one, because a
 * catalogue must never present a dev identifier as the player-facing name (memory: catalogue-human-names).
 */
export function mapInventoryRow(row: DuneInventoryRow, displayName?: (code: string) => string | undefined): TakaroItem {
  const code = str(row.templateId);
  if (!code) throw new Error(`Inventory row has no template_id: ${JSON.stringify(row)}`);
  const item: TakaroItem = { code, name: displayName?.(code) ?? code, amount: num(row.stackSize) ?? 1 };
  const quality = num(row.qualityLevel);
  // quality_level 0 is "no tier" in this schema, not a tier called "0".
  if (quality !== null && quality > 0) item.quality = String(quality);
  return item;
}

export function mapItemDefinition(raw: unknown): TakaroItem {
  const i = asRecord(raw);
  const code = str(i.code) ?? str(i.templateId) ?? str(i.name);
  if (!code) throw new Error(`Item definition has no code: ${JSON.stringify(raw)}`);
  const item: TakaroItem = { code, name: str(i.name) ?? code };
  if (str(i.description)) item.description = str(i.description)!;
  return item;
}

export function mapEntityType(raw: unknown): TakaroEntityType {
  const t = String(raw ?? '').toLowerCase();
  if (['hostile', 'enemy', 'aggressive', 'sandworm', 'raider', 'bandit', 'soldier', 'scavenger', 'mob'].some((k) => t.includes(k))) {
    return 'hostile';
  }
  if (['friendly', 'ally', 'npc', 'vendor', 'merchant', 'trader', 'vehicle', 'ornithopter'].some((k) => t.includes(k))) {
    return 'friendly';
  }
  return 'neutral';
}

export function mapEntity(raw: unknown): TakaroEntity {
  const e = asRecord(raw);
  const code = str(e.code) ?? str(e.name);
  if (!code) throw new Error(`Entity definition has no code: ${JSON.stringify(raw)}`);
  const entity: TakaroEntity = { code, name: str(e.name) ?? code, type: mapEntityType(str(e.type) ?? code) };
  if (str(e.description)) entity.description = str(e.description)!;
  return entity;
}

export function mapLocation(raw: unknown): TakaroLocation {
  const l = asRecord(raw);
  const code = str(l.code) ?? str(l.name);
  if (!code) throw new Error(`Location has no code: ${JSON.stringify(raw)}`);
  const loc: TakaroLocation = { code, name: str(l.name) ?? code, position: mapPosition(l.position ?? l) };
  for (const key of ['radius', 'sizeX', 'sizeY', 'sizeZ'] as const) {
    const v = num(l[key]);
    if (v !== null) loc[key] = v;
  }
  return loc;
}

export function mapBan(raw: unknown): TakaroBan {
  const b = asRecord(raw);
  const playerSource = Object.keys(asRecord(b.player)).length ? asRecord(b.player) : b;
  const expires = b.expiresAt;
  return {
    player: mapPlayer(playerSource),
    reason: str(b.reason) ?? '',
    expiresAt:
      typeof expires === 'string' && expires ? expires : typeof expires === 'number' ? new Date(expires).toISOString() : null,
  };
}

/**
 * Dune's chat channels → Takaro's chat channel enum (`global` | `team` | `friends` | `whisper`).
 *
 * `m_ChannelType` arrives either short (`Map`, `Whispers`) or fully qualified
 * (`ETextChatChannelType::Whispers`); both forms are accepted. Takaro has no proximity channel, so the two
 * proximity-limited Dune channels map to `team` — the closest "not everyone hears this" channel Takaro has, which is
 * what makes a module's `onlyGlobalChat` setting exclude them instead of relaying them server-wide.
 */
export function mapChannel(raw: unknown): string {
  const c = String(raw ?? '')
    .replace(/^.*::/, '')
    .toLowerCase();
  if (c === 'whispers' || c === 'whisper' || c === 'private') return 'whisper';
  if (c === 'party' || c === 'guild' || c === 'squad') return 'team';
  if (c === 'proximity' || c === 'local') return 'team';
  if (c === 'friends') return 'friends';
  return 'global';
}

/** Short `m_ChannelType` name for an inbound value in either spelling (`ETextChatChannelType::Map` → `Map`). */
export function shortChannel(raw: unknown): string {
  return String(raw ?? '').replace(/^.*::/, '');
}

export interface MappedEvent {
  type: GameEventType;
  data: Record<string, unknown>;
}

/**
 * ISO-8601 for an event timestamp (ISO string, epoch ms, or UE's `%Y.%m.%d-%H.%M.%S`). Takaro overwrites the
 * `timestamp` on receipt, but stamping it keeps replayed events (pending queue after an outage) self-describing.
 */
export function eventTimestamp(ts: unknown): string | null {
  if (typeof ts === 'number' && Number.isFinite(ts) && ts > 0) return new Date(ts).toISOString();
  const s = str(ts);
  if (!s) return null;
  const ue = parseUeTimestamp(s);
  if (ue) return ue;
  const parsed = Date.parse(s);
  return Number.isFinite(parsed) ? new Date(parsed).toISOString() : null;
}

/** UE's `2026.05.21-02.43.11` (the spelling Dune's chat payload uses) → ISO-8601, treated as UTC. */
export function parseUeTimestamp(value: string): string | null {
  const m = /^(\d{4})\.(\d{2})\.(\d{2})-(\d{2})\.(\d{2})\.(\d{2})$/.exec(value.trim());
  if (!m) return null;
  const [, y, mo, d, h, mi, s] = m;
  return `${y}-${mo}-${d}T${h}:${mi}:${s}.000Z`;
}

/** Formats a Date in UE's `%Y.%m.%d-%H.%M.%S` (UTC), the form the client-visible whisper payload was confirmed with. */
export function formatUeTimestamp(date: Date): string {
  const p = (n: number): string => String(n).padStart(2, '0');
  return (
    `${date.getUTCFullYear()}.${p(date.getUTCMonth() + 1)}.${p(date.getUTCDate())}` +
    `-${p(date.getUTCHours())}.${p(date.getUTCMinutes())}.${p(date.getUTCSeconds())}`
  );
}

/**
 * Resolves a plugin-side player reference to a Takaro player.
 *
 * The plugin never knows the FLS id (see `pluginJoin.ts`), so a payload arrives carrying `ref`,
 * `accountId` and `characterName` and nothing Takaro can use as a `gameId`. The resolver is the
 * sidecar's Postgres join. Returning `null` means "not joinable", and the caller then drops the field
 * rather than inventing a `gameId` out of a character name — a name is not an identity, and a
 * mis-joined kill is worse than an unattributed one.
 */
export type PluginPlayerResolver = (source: Record<string, unknown>) => TakaroPlayer | null;

export interface MapPluginEventOptions {
  resolvePlayer?: PluginPlayerResolver;
  /**
   * Turns a plugin `entityCode` (a UE class name such as `DuneNpcCharacter`) into a DISPLAY name from
   * the sidecar's own catalogue, or undefined when the catalogue cannot name it. This is the hook that
   * keeps the catalogue rule (memory: catalogue-human-names) honest: without a name, the event carries
   * `nameIsClassName: true` and the caller can drop it instead of shipping a dev name to Takaro.
   */
  entityName?: (code: string) => string | undefined;
  /**
   * Turns an ITEM template id (`items.template_id`, e.g. `ScrapMetalKnife`) into its catalogue display
   * name ("Scrap Metal Knife"). Lane L2d: the plugin now reads the weapon off the killer as an item
   * template id — the live NPC's `WeaponActorComponent::m_WeaponName` was the verbatim catalogue code
   * `ChoamSda2` — so this is the hook that turns it into the `weapon` string Takaro shows. Without it
   * the weapon degrades to a damage-type CATEGORY, never to a dev name (memory: catalogue-human-names).
   */
  itemName?: (code: string) => string | undefined;
  /**
   * Called when a plugin event is refused or re-labelled by one of the guards, with a stable reason key
   * (`entityKilledUnnamed`, `entityKilledPlayerCharacter`). Surfaced on `/health` as
   * `droppedEvents.<reason>`: a guard that fires silently is indistinguishable from a plugin that never
   * sent anything, and that is how the `BP_DunePlayerCharacter_C` kill reached Takaro in the first place.
   */
  onDrop?: (reason: string) => void;
}

/**
 * Normalises an optional-plugin `/events` entry into a Takaro gameEvent payload. Returns null for
 * unknown types, and also for an event whose subject cannot be identified at all — a `player-death`
 * with no resolvable player is not a death Takaro can record.
 */
export function mapPluginEvent(
  event: { type: string; data: unknown; ts?: unknown },
  options: MapPluginEventOptions = {},
): MappedEvent | null {
  if (!isGameEventType(event.type)) return null;
  const d = asRecord(event.data);
  const resolve = (source: Record<string, unknown>): TakaroPlayer | null => {
    if (!Object.keys(source).length) return null;
    if (options.resolvePlayer) {
      const joined = options.resolvePlayer(source);
      if (joined) return joined;
      // A payload that already carries a usable identifier (the legacy plugin shape, or a test
      // fixture) still maps directly; a payload with only a `ref` does not.
      if (!str(source.flsId) && !str(source.gameId) && !str(source.funcomId) && !str(source.platformId)) return null;
    }
    try {
      return mapPlayer(source);
    } catch {
      return null;
    }
  };
  const withPlayer = (): Record<string, unknown> | null => {
    const source = Object.keys(asRecord(d.player)).length ? asRecord(d.player) : d;
    const player = resolve(source);
    return player ? { player } : null;
  };

  let mapped: MappedEvent;
  switch (event.type) {
    case 'player-connected':
    case 'player-disconnected': {
      const out = withPlayer();
      // The plugin's connect/disconnect are PRECISE EDGES, not identities: they say `hint: true` and
      // `identityAuthority: sidecar/postgres` themselves. Without a join there is no `gameId`, and a
      // connect for an unidentified player would create a phantom Takaro player — so it is dropped and
      // the sidecar's own Postgres presence poller reports the join a poll later.
      if (!out) return null;
      mapped = { type: event.type, data: out };
      break;
    }
    case 'chat-message': {
      const out: Record<string, unknown> = {
        msg: str(d.msg) ?? str(d.message) ?? '',
        channel: mapChannel(d.channel ?? d.channelType),
      };
      if (Object.keys(asRecord(d.player)).length) out.player = mapPlayer(asRecord(d.player));
      mapped = { type: event.type, data: out };
      break;
    }
    case 'player-death': {
      const out = withPlayer();
      if (!out) return null;
      const pos = maybePosition(d.position);
      if (pos) out.position = pos;
      // The killer arrives as `killer` (the plugin's own shape) or `attacker` (the legacy one), and is
      // only ever set when the join resolved it: Takaro's `attacker` is a PLAYER, so an NPC killer has
      // to be named in `msg` instead (see withDeathMessage).
      const killerSource = Object.keys(asRecord(d.killer)).length ? asRecord(d.killer) : asRecord(d.attacker);
      const killer = resolve(killerSource);
      // GUARD 3 — nobody killed him. The plugin passes the victim's own pawn as the instigator for a fall, a storm or
      // any other environmental death, and naming a player as their own killer reads as a suicide and scores as PvP
      // (the same trap as VEIN L2c). Self-attribution is therefore dropped: the death simply has no `attacker`, and
      // `withDeathMessage` says "died" rather than "was killed by".
      const victimRef = str(asRecord(d.player).ref) ?? str((out.player as Record<string, unknown>).gameId);
      const killerRef = str(killerSource.ref) ?? (killer ? str(killer.gameId) : null);
      const selfInflicted = Boolean(victimRef && killerRef && victimRef === killerRef);
      if (selfInflicted) logger.debug(`player-death instigator == victim (${victimRef}); reporting no attacker (self/environment death)`);
      if (killer && !selfInflicted) out.attacker = killer;
      // Takaro's `EventPlayerDeath` whitelist is `player` + `attacker` + `position` only (see
      // takaro/eventWhitelist.ts) — there is NO weapon field, and an extra key destroys the whole
      // event. So the weapon an NPC killed him with can only reach a human through `msg`, which is
      // whitelisted, and only ever as a catalogue display name.
      mapped = { type: event.type, data: withDeathMessage(out, d, resolveWeaponName(d, options.itemName)) };
      break;
    }
    case 'entity-killed': {
      // `player` is the KILLER here, and Takaro requires one: an NPC killing an NPC is not a kill
      // Takaro can credit, so it is dropped rather than attributed to nobody.
      const killerSource = Object.keys(asRecord(d.player)).length ? asRecord(d.player) : asRecord(d.killer);
      const player = resolve(killerSource);
      if (!player) return null;
      const out: Record<string, unknown> = { player };
      // ⚠️ Catalogue rule (memory: catalogue-human-names). The plugin cannot produce a display name —
      // this server build ships no `Content/Localization/` and `DuneNpcCharacter::m_Name` is a
      // data-table row name — so it sends `entity: null` + `entityCode` + `nameIsClassName: true`. A
      // dev name must never reach Takaro as if it were a creature's name, so the resolver below only
      // uses `entity` when the plugin actually had one, and otherwise leaves the naming to
      // `resolveEntityName`, which is backed by the sidecar's own catalogue.
      const declared = str(d.entity);
      const code = str(d.entityCode) ?? str(asRecord(d.entity).code) ?? str(d.entityDevName) ?? str(d.entityClass);
      const named = declared && d.nameIsClassName !== true ? declared : options.entityName?.(code ?? '');

      // GUARD 2 — this is not an entity kill at all.
      //
      // Measured 2026-09-21 (plugin seqs 4 and 5): the player's own environment/self deaths came through as
      // `entity-killed` with `entityClass: BP_DunePlayerCharacter_C` and the victim unresolved, so Takaro was told
      // TakaroTest had killed a `BP_DunePlayerCharacter_C`. A player character is never an entity kill. It is
      // re-labelled as a `player-death` for that same player, which the DeathCoalescer then merges with the
      // Postgres `life_state` edge so exactly ONE death reaches Takaro — instead of a phantom creature kill plus a
      // death. L2c fixes the classification in the plugin; this guard means a build without that fix cannot poison
      // Takaro's kill feed. (No `attacker`: instigator == victim is a self/environment death, see GUARD 3.)
      if (isPlayerCharacterClass(code) || isPlayerCharacterClass(declared)) {
        options.onDrop?.('entityKilledPlayerCharacter');
        logger.info(`Plugin reported a player character (${code ?? declared}) as an entity kill; re-labelling it as that player's own death`);
        mapped = { type: 'player-death', data: withDeathMessage({ player }, d) };
        break;
      }

      // GUARD 1 — a dev name must never reach Takaro as if it were a creature's name (memory:
      // catalogue-human-names). The plugin cannot produce a display name, so it flags `nameIsClassName: true` and we
      // fall back to the sidecar's own catalogue. When the catalogue has no row either there is no honest name to
      // send, and the event is DROPPED rather than forwarded with `BP_Something_C` standing in for a creature.
      // Counted on /health so the gap is visible instead of silent.
      if (!named || isDevEntityName(named)) {
        options.onDrop?.('entityKilledUnnamed');
        logger.info(`Dropping entity-killed: no human name for '${code ?? declared ?? '(none)'}' (a dev/class name must not reach Takaro)`);
        return null;
      }
      out.entity = named;
      // Takaro REQUIRES `weapon` to be a string and drops the whole event when it is absent (VEIN F20); the empty
      // string is the one value that says "not known" without naming something that did not kill anything.
      out.weapon = resolveWeaponName(d, options.itemName) ?? '';
      // ⚠️ `entityCode`, `nameIsClassName`, `weaponCode` and `attribution` used to be attached here. Takaro validates
      // gameEvents with `forbidNonWhitelisted: true`, so each of them destroyed the entire event
      // (`property entityCode has failed the following constraints: whitelistValidation`, measured on a real kill
      // 2026-09-21). `EventEntityKilled` accepts only `player`, `entity`, `weapon` + the base fields. The context is
      // still in the plugin's own `/events` record and in the sidecar's debug log, which is where it belongs.
      logger.debug(`entity-killed context (not sent to Takaro): code=${code ?? '-'} named=${named ?? '-'} weaponItemCode=${str(d.weaponItemCode) ?? '-'} weaponSource=${str(d.weaponSource) ?? '-'} damageTypeCode=${str(d.damageTypeCode) ?? '-'} attribution=${str(d.attribution) ?? '-'}`);
      mapped = { type: event.type, data: out };
      break;
    }
    case 'log':
      mapped = {
        type: 'log',
        data: {
          msg: str(d.msg) ?? str(d.line) ?? (typeof event.data === 'string' ? event.data : JSON.stringify(event.data)),
        },
      };
      break;
    default:
      return null;
  }
  const timestamp = eventTimestamp(event.ts);
  if (timestamp) mapped.data.timestamp = timestamp;
  return mapped;
}

/**
 * Takaro's EventPlayerDeath only carries a *player* `attacker`; a sandworm, a Coriolis storm or a fall has to be named
 * in the base `msg`. Dune's `life_state` distinguishes `DeadBySandworm` / `DeadByCoriolis` from a plain `Dead`, and
 * without the plugin that is the only attribution available — so it is the only attribution we claim.
 */
/**
 * A last-resort weapon name from the plugin's `weaponCode`.
 *
 * Two values are refused outright:
 *  - `BlueprintGeneratedClass` — a plugin-side cosmetic bug: it read the UE **class of the class object** instead of
 *    the asset's own name, so every weapon came out with that same string. It names no weapon, so it becomes null.
 *  - anything that still looks like a dev/asset id (`BP_Dart_C`, `SK_Something`, CamelCase runs, snake_case), because
 *    a dev name must never reach Takaro as if it were a display name (memory: catalogue-human-names).
 */
/**
 * UE classes that ARE the player's own character. A kill on one of these is never an entity kill; it is the player's
 * own death, usually environmental. Matched loosely (with or without the `BP_` prefix and the `_C` suffix) because
 * the plugin reports the class under three different keys depending on which hook saw it.
 */
export function isPlayerCharacterClass(value: unknown): boolean {
  const code = str(value);
  if (!code) return false;
  return /DunePlayerCharacter|PlayerCharacter|PlayerPawn/i.test(code);
}

/**
 * Does this still look like an asset/class id rather than a creature's name? Anything that does must not reach
 * Takaro as a display name (memory: catalogue-human-names) — `BP_DunePlayerCharacter_C` did, and it was wrong twice
 * over.
 */
export function isDevEntityName(value: unknown): boolean {
  const name = str(value);
  if (!name) return true;
  if (/^(BP|SK|SM|DT|WBP|ABP|A|U)_/.test(name)) return true;
  if (/_C$/.test(name)) return true;
  if (/_/.test(name)) return true;
  // A CamelCaseRun with no spaces: `DuneCritterBase`, `DunePlayerCharacter`.
  if (!/\s/.test(name) && /[a-z][A-Z]/.test(name)) return true;
  return false;
}

/**
 * A damage-type CLASS mapped to a short human CATEGORY — the last resort, and deliberately a category
 * rather than a name.
 *
 * `BP_DmgType_Melee_Quick_C` names no weapon; "Melee" at least tells a player how he died and is
 * obviously a category rather than a thing you can pick up, which is what keeps it clear of the
 * catalogue rule (memory: catalogue-human-names). An environmental damage type gets NOTHING: "Fall" in
 * a kill feed's weapon column would read as a weapon, and for an `entity-killed` it would be a lie.
 */
export function damageTypeCategory(value: unknown): string | null {
  const code = str(value);
  if (!code) return null;
  if (/melee|sword|knife|kindjal|blade|slash/i.test(code)) return 'Melee';
  if (/dart|bullet|projectile|gun|pistol|rifle|smg|lasgun|beam|shot|ranged|hitscan/i.test(code)) return 'Ranged';
  if (/explos|grenade|rocket|mine\b/i.test(code)) return 'Explosive';
  if (/vehicle|ornithopter|buggy|ram|collision|impact/i.test(code)) return 'Vehicle';
  return null;
}

/**
 * The `weapon` string for a Takaro death/kill event, best evidence first.
 *
 * Lane L2d. The plugin reads the weapon off the KILLER as an ITEM TEMPLATE ID (`weaponItemCode`),
 * because the death frame carries only a damage-TYPE class — so the first real answer is a catalogue
 * lookup in exactly the code space `listItems` and every shop listing already use. Everything below it
 * is a documented degrade, and none of the steps can emit a dev name:
 *
 *   1. `weapon`            — a display name the plugin already had (it never has one today).
 *   2. `weaponItemCode`    — an item template id -> catalogue display name. THE path that matters.
 *   3. `weaponCode`        — the same lookup, because the plugin also puts the item id here when it has
 *                            one; harmless when it holds the old damage-type class (no catalogue row).
 *   4. `weaponFromCode`    — a code that is already human-shaped (no underscores, no CamelCase run).
 *   5. `damageTypeCategory`— "Melee" / "Ranged": a category, clearly not a weapon's name.
 *
 * `null` means "nothing honest to say"; the caller sends `''`, which is the one value that says unknown
 * without naming something that killed nobody.
 */
export function resolveWeaponName(
  d: Record<string, unknown>,
  itemName?: (code: string) => string | undefined,
): string | null {
  const declared = str(d.weapon);
  if (declared) return declared;
  for (const key of ['weaponItemCode', 'weaponCode'] as const) {
    const code = str(d[key]);
    if (!code) continue;
    const name = itemName?.(code);
    if (name && !isDevEntityName(name)) return name;
  }
  return weaponFromCode(d.weaponCode) ?? damageTypeCategory(d.damageTypeCode ?? d.weaponCode);
}

export function weaponFromCode(value: unknown): string | null {
  const code = str(value);
  if (!code || code === 'BlueprintGeneratedClass') return null;
  if (/_/.test(code) || /[a-z][A-Z]/.test(code) || /^(BP|SK|SM|DT|WBP|ABP)/.test(code)) return null;
  return code;
}

export function withDeathMessage(
  out: Record<string, unknown>,
  d: Record<string, unknown>,
  weapon?: string | null,
): Record<string, unknown> {
  if (out.attacker) return out;
  const who = (out.player as TakaroPlayer).name;
  // Lane L2d: `msg` is the only whitelisted place a weapon can appear on a `player-death`, and it is
  // only ever appended to a "was killed by …" sentence — a fall did not kill anyone WITH anything.
  const withWeapon = (msg: string): string => (weapon ? `${msg} with a ${weapon}` : msg);
  // `killerEntity` is a DISPLAY name when the plugin or the catalogue had one. `killerEntityCode` is a
  // UE class name, and it is deliberately NOT interpolated into a player-facing message: a Discord
  // kill feed reading "killed by ADuneNpcCharacter" is exactly the dev-name leak the catalogue rule
  // forbids (memory: catalogue-human-names). The code travels in its own field for diagnostics.
  const killerEntity = str(d.killerEntity) ?? str(asRecord(d.killer).code);
  if (killerEntity) {
    out.msg = withWeapon(`${who} was killed by ${killerEntity}`);
    return out;
  }
  const killerCode = str(d.killerEntityCode);
  if (killerCode) {
    out.msg = withWeapon(`${who} was killed by an NPC`);
    out.killerEntityCode = killerCode;
    return out;
  }
  const cause = str(d.cause) ?? causeFromLifeState(str(d.lifeState));
  out.msg = cause ? `${who} died (${cause})` : `${who} died`;
  return out;
}

/** `DeadBySandworm` → `sandworm`, `DeadByCoriolis` → `coriolis storm`, `Dead` → null (cause unknown out of process). */
export function causeFromLifeState(lifeState: string | null): string | null {
  if (!lifeState) return null;
  const m = /^Dead(?:By)?(.+)$/i.exec(lifeState.trim());
  if (!m) return null;
  const rest = m[1];
  if (/coriolis/i.test(rest)) return 'coriolis storm';
  return rest.replace(/([a-z])([A-Z])/g, '$1 $2').toLowerCase();
}

/**
 * Dune stores one `dune.items` row per stack, and a player can hold several stacks of the same template in several
 * inventories (backpack + gear slots). Takaro renders one row per IItemDTO, so identical (code, quality) pairs are
 * summed, keeping first-appearance order and the first row's name.
 */
export function aggregateInventory(items: TakaroItem[]): TakaroItem[] {
  const out: TakaroItem[] = [];
  const index = new Map<string, TakaroItem>();
  for (const item of items) {
    const key = JSON.stringify([item.code, item.quality ?? null]);
    const existing = index.get(key);
    if (existing) {
      existing.amount = (existing.amount ?? 1) + (item.amount ?? 1);
      continue;
    }
    const copy: TakaroItem = { ...item, amount: item.amount ?? 1 };
    index.set(key, copy);
    out.push(copy);
  }
  return out;
}

const SPECIALIZATION_TRACKS: Record<string, { key: string; name: string }> = {
  Crafting: { key: 'specialization.crafting', name: 'Crafting Specialization' },
  Gathering: { key: 'specialization.gathering', name: 'Gathering Specialization' },
  Exploration: { key: 'specialization.exploration', name: 'Exploration Specialization' },
  Combat: { key: 'specialization.combat', name: 'Combat Specialization' },
  Sabotage: { key: 'specialization.sabotage', name: 'Sabotage Specialization' },
};

/**
 * The game stores no level, it derives one from XP through a curve in its cooked content. Every level past the first
 * grants one skill point and keystones add theirs on top, both in `TotalSkillPoints`, so the level is that total minus
 * the keystone bonus, plus the first level. `progress` is left out for the character because it needs that curve.
 *
 * A row that cannot make a valid track is dropped rather than answered: one invalid track fails the whole reading, and
 * a track missing from a reading is merely unknown to Takaro.
 */
export function mapProgression(rows: DuneProgressionRows): TakaroProgressionTrack[] {
  const tracks: TakaroProgressionTrack[] = [];
  const character = rows.character;
  if (character && Number.isInteger(character.totalSkillPoints)) {
    const level = character.totalSkillPoints! - (character.keystoneBonusSkillPoints ?? 0) + 1;
    if (Number.isInteger(level) && level >= 1) {
      tracks.push({
        key: 'character',
        name: 'Level',
        level,
        ...(Number.isFinite(character.totalXpEarned) ? { experience: character.totalXpEarned! } : {}),
      });
    }
  }
  for (const row of rows.specializations) {
    const track = SPECIALIZATION_TRACKS[row.track];
    if (!track || !Number.isFinite(row.level) || row.level! < 0) continue;
    const level = Math.floor(row.level!);
    tracks.push({
      ...track,
      level,
      ...(Number.isFinite(row.xp) ? { experience: row.xp! } : {}),
      progress: row.level! - level,
    });
  }
  return tracks;
}
