import { HAGGA_BASIN } from './mapDimensions.js';
import { PLACE_NAMES } from './placeNames.js';
import type { TakaroLocation } from './types.js';

/**
 * `listLocations` from the world's map markers.
 *
 * The battlegroup's `markers` table holds every marker the map server has placed: resource nodes and unnamed helper
 * points (payload type `Default`, empty payload) and the named places a player sees on the in-game world map (payload
 * type `StaticLocation`, with a `DisplayName`). Only the named places are locations. Each carries a real position in
 * the same Unreal units as the player pawn, and `map_name_id` says which map it is on.
 *
 * The server stores the display name as a localisation reference, not as text:
 *
 *   LOCTABLE("/Game/.../ST_Localization_UI.ST_Localization_UI", "UI/EnemyCampDisplayName/ScavengerCamp")
 *   LOCTABLE("...ST_Localization_NPCs_World", "NPCS_AND_WORLD/WORLD_MAP_LOCATION_Survival_VermiliusGap_Cave15")
 *
 * The name is the English string of that key (`PLACE_NAMES`, from the server's own string tables): `Zein Grotto`,
 * `Scavenger Camp`, `Wreck of the Mithra`. A key the table lacks (a newer build) is spelled out from the key instead,
 * `Cave 15 (Vermilius Gap)`; a key that does not spell out to plain words is dropped rather than shown as an asset id.
 *
 * World partitions are not locations: a partition is a running map process and has no position.
 */
export interface MarkerRow {
  id: number;
  displayName: string | null;
  mapName: string | null;
  x: number;
  y: number;
  z: number;
}

/** The second argument of `LOCTABLE("table", "key")`, or the raw text when it is not a LOCTABLE reference. */
export function localisationKey(displayName: string): string {
  const m = /LOCTABLE\(\s*"[^"]*"\s*,\s*"([^"]+)"\s*\)/.exec(displayName);
  return (m ? m[1] : displayName).trim();
}

/** `HaggaBasinSouth` -> `Hagga Basin South`, `PoI3` -> `PoI 3`, `SISTER_MESA` -> `Sister Mesa`. */
function words(token: string): string {
  const spaced = token
    .replace(/_/g, ' ')
    .replace(/([a-z])([A-Z])/g, '$1 $2')
    .replace(/([A-Za-z])(\d)/g, '$1 $2')
    .replace(/\s+/g, ' ')
    .trim();
  return spaced
    .split(' ')
    .map((w) => (w === w.toUpperCase() && /[A-Z]/.test(w) ? w[0] + w.slice(1).toLowerCase() : w))
    .join(' ')
    .replace(/\bPo I\b/g, 'Point of Interest');
}

/**
 * A player-facing name for a marker's localisation key, plus whether the key names one specific place (so several
 * markers with it are one location) or a kind of place (`Scavenger Camp`, so each marker is its own location).
 */
export function nameForKey(key: string): { name: string; unique: boolean } | null {
  const last = key.split('/').pop() ?? '';
  const kind = /^UI\/EnemyCampDisplayName\//.test(key) || /^Map_Legend_Entry_/.test(last);
  const known = PLACE_NAMES[key]?.trim();
  if (known) return { name: known, unique: !kind };
  let name: string | null = null;
  let unique = true;

  // NPCS_AND_WORLD/WORLD_MAP_LOCATION_Survival_<Region>_<Place>, or WORLD_MAP_LOCATION_<PLACE>.
  let m = /^WORLD_MAP_LOCATION_(?:Survival_([A-Za-z]+)_)?([A-Za-z0-9]+(?:_[A-Za-z0-9]+)*)$/.exec(last);
  if (m) {
    const place = words(m[2]);
    name = m[1] ? `${place} (${words(m[1])})` : place;
  }
  // NPCS_AND_WORLD/NPC_NAMED_<NAME>_DISPLAYNAME — a named NPC stationed at one place.
  m = name ? null : /^NPC_NAMED_([A-Z0-9_]+?)_DISPLAYNAME$/.exec(last);
  if (m) name = words(m[1]);
  // UI/EnemyCampDisplayName/<Kind> and UI/UI/Map_Legend_Entry_<Kind>: a kind of place, repeated across the map.
  if (!name && /^UI\/EnemyCampDisplayName\//.test(key)) {
    name = words(last);
    unique = false;
  }
  m = name ? null : /^Map_Legend_Entry_(.+)$/.exec(last);
  if (m) {
    name = words(m[1]);
    unique = false;
  }
  if (!name || !/^[A-Za-z][A-Za-z0-9 '()-]*$/.test(name)) return null;
  return { name, unique };
}

/**
 * The Dimension a marker's map belongs to, spelled the way player positions spell it (`dimensionForMap`): Hagga
 * Basin is `survival_1`; any other map takes the matching `world_partition.map` (`DeepDesert` -> `deepdesert_1`), or
 * its own name lower-cased when no running partition names it.
 */
export function dimensionForMarkerMap(mapName: string | null, partitionMaps: readonly string[] = []): string {
  const map = mapName?.trim() ?? '';
  if (!map || map.toLowerCase() === 'haggabasin') return HAGGA_BASIN.id;
  const lower = map.toLowerCase();
  const match = partitionMaps.find((p) => p.toLowerCase() === lower || p.toLowerCase().replace(/_\d+$/, '') === lower);
  return (match ?? map).toLowerCase();
}

/**
 * Markers of one kind of place closer than this (Unreal units, 25 m) are one place marked twice. Real camps of one
 * kind stand at least ~55 m apart; the only closer pair seen on Hagga Basin is two Scavenger Camp markers 19 m apart.
 */
export const SAME_PLACE_RADIUS = 2500;

export function markerLocations(rows: readonly MarkerRow[], partitionMaps: readonly string[] = []): TakaroLocation[] {
  const groups = new Map<string, { name: string; dimension: string; points: MarkerRow[] }>();
  const sorted = [...rows].sort((a, b) => a.id - b.id);
  for (const row of sorted) {
    if (!row.displayName || ![row.x, row.y, row.z].every(Number.isFinite)) continue;
    const key = localisationKey(row.displayName);
    const named = nameForKey(key);
    if (!named) continue;
    const dimension = dimensionForMarkerMap(row.mapName, partitionMaps);
    // Several markers of one named place (a cave's entrances) collapse into one location at their centre. Each
    // marker of a kind of place (`Scavenger Camp`) is its own camp, unless it sits on top of an earlier one.
    let groupKey = `${dimension}|${key}`;
    if (!named.unique) {
      const near = [...groups.entries()].find(
        ([k, g]) => k.startsWith(`${groupKey}|`) && g.points.some((p) => Math.hypot(p.x - row.x, p.y - row.y) < SAME_PLACE_RADIUS),
      );
      groupKey = near ? near[0] : `${groupKey}|${row.id}`;
    }
    const group = groups.get(groupKey) ?? { name: named.name, dimension, points: [] };
    group.points.push(row);
    groups.set(groupKey, group);
  }
  const out: TakaroLocation[] = [];
  for (const { name, dimension, points } of groups.values()) {
    const mean = (f: 'x' | 'y' | 'z') => Math.round(points.reduce((s, p) => s + p[f], 0) / points.length);
    const id = Math.min(...points.map((p) => p.id));
    out.push({ code: `marker:${id}`, name, position: { x: mean('x'), y: mean('y'), z: mean('z'), dimension } });
  }
  return out.sort((a, b) => a.name.localeCompare(b.name) || a.code.localeCompare(b.code));
}
