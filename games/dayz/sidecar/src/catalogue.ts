// Human names for the item and entity catalogues, and for the class names in kill and death
// events.
//
// The mod reads `displayName` from the game config. DayZ leaves some of those untranslated
// ("$UNT$Kiwi"), some classes have none (the mod then sends the class name), and colour or
// faction variants share one name ("Armband" for 41 classes, "Infected" for every zombie).
// Takaro shows names to admins and players, so each one is cleaned here and variants get the
// part of their class name that tells them apart: "Armband (UEC)", "Infected (Male Villager
// Old Blue)". The events carry class names; they are looked up in the last catalogue the mod
// answered, so a kill reads "Infected (Male Hermit Skinny Beige)" with "Mosin 91/30".

export interface CatalogueEntry {
  name: string;
  code: string;
  [key: string]: unknown;
}

const UNTRANSLATED = /^\$UNT\$/;
const STRING_KEY = /^[$#]?STR_/i;
/** Class-name prefixes of the infected that mean something to a reader. */
const TOKEN_WORDS: Record<string, string> = { zmbm: 'Male', zmbf: 'Female' };

/** "ZmbM_VillagerOld_Blue" -> ["ZmbM", "VillagerOld", "Blue"] */
function codeTokens(code: string): string[] {
  return code.split(/[_\s]+/).filter((t) => t !== '');
}

/** "VillagerOld" -> "Villager Old", "takiOilRigWorker" -> "Taki Oil Rig Worker", "ZmbM" -> "Male". */
function humanToken(token: string): string {
  const word = TOKEN_WORDS[token.toLowerCase()];
  if (word) return word;
  const spaced = token.replace(/([a-z])([A-Z])/g, '$1 $2').replace(/([A-Za-z])(\d)/g, '$1 $2');
  return spaced.charAt(0).toUpperCase() + spaced.slice(1);
}

/** A readable name made from a class name: "StorageBox_Wooden" -> "Storage Box Wooden". */
export function humanizeCode(code: string): string {
  return codeTokens(code).map(humanToken).join(' ');
}

/** One displayName cleaned up, falling back to the class name when the game gave none. */
export function cleanName(name: unknown, code: string): string {
  let n = typeof name === 'string' ? name.replace(UNTRANSLATED, '').trim() : '';
  if (n === '' || STRING_KEY.test(n) || n === code) n = humanizeCode(code);
  return n;
}

/**
 * Cleans every name and gives entries that share a name a variant suffix from the parts of
 * their class name the others do not have. Order and every other field are kept.
 */
export function humanizeCatalogue<T extends CatalogueEntry>(entries: T[]): T[] {
  const cleaned = entries.map((e) => ({ ...e, name: cleanName(e.name, String(e.code ?? '')) }));
  const groups = new Map<string, number[]>();
  cleaned.forEach((e, i) => {
    const key = e.name.toLowerCase();
    const g = groups.get(key);
    if (g) g.push(i);
    else groups.set(key, [i]);
  });
  for (const indexes of groups.values()) {
    if (indexes.length < 2) continue;
    const tokenSets = indexes.map((i) => codeTokens(String(cleaned[i]!.code)));
    const common = new Set(
      tokenSets[0]!.map((t) => t.toLowerCase()).filter((t) => tokenSets.every((ts) => ts.some((x) => x.toLowerCase() === t))),
    );
    const base = cleaned[indexes[0]!]!.name;
    // The base class (all of its class-name parts shared, e.g. PorkCan next to PorkCan_Opened)
    // keeps the plain name; every variant gets the parts that tell it apart.
    const named = indexes.map((_, k) => {
      const label = tokenSets[k]!.filter((t) => !common.has(t.toLowerCase())).map(humanToken).join(' ');
      return label ? `${base} (${label})` : base;
    });
    // Two classes can still end up alike (same tokens in another order): name those by class.
    const seen = new Map<string, number>();
    named.forEach((n) => seen.set(n.toLowerCase(), (seen.get(n.toLowerCase()) ?? 0) + 1));
    indexes.forEach((i, k) => {
      const n = named[k]!;
      cleaned[i]!.name = seen.get(n.toLowerCase())! > 1 ? `${base} (${cleaned[i]!.code})` : n;
    });
  }
  return cleaned as T[];
}

/** Damage sources that are not items: the engine reports them by their ammo-type name. */
const DEATH_CAUSES: Record<string, string> = {
  falldamagehealth: 'a fall',
  falldamage: 'a fall',
  fireplacefire: 'fire',
  bleeding: 'bleeding',
};

/** The class-name to human-name lookup the events use, fed by every catalogue the mod answers. */
export class Catalogue {
  private items = new Map<string, string>();
  private entities = new Map<string, string>();

  setItems(entries: CatalogueEntry[]): void {
    this.items = toMap(entries);
  }

  setEntities(entries: CatalogueEntry[]): void {
    this.entities = toMap(entries);
  }

  hasItems(): boolean {
    return this.items.size > 0;
  }

  hasEntities(): boolean {
    return this.entities.size > 0;
  }

  /** The item's name, or a readable form of the class name when it is not in the catalogue. */
  itemName(code: string): string {
    if (!code) return code;
    return this.items.get(code.toLowerCase()) ?? humanizeCode(code);
  }

  /** The entity's name; entities outside the catalogue (a player, a vehicle) get the readable class name. */
  entityName(code: string): string {
    if (!code) return code;
    return this.entities.get(code.toLowerCase()) ?? this.items.get(code.toLowerCase()) ?? humanizeCode(code);
  }

  /** Rewrites the class names inside an entity-killed or player-death event. */
  translateEvent<T extends { type?: string; entity?: unknown; weapon?: unknown; msg?: unknown }>(type: string, event: T): T {
    if (type === 'entity-killed') {
      const out = { ...event };
      if (typeof out.entity === 'string') out.entity = this.entityName(out.entity);
      if (typeof out.weapon === 'string' && out.weapon !== '') out.weapon = this.itemName(out.weapon);
      return out;
    }
    if (type === 'player-death' && typeof event.msg === 'string') {
      const m = /^killed with (.+)$/.exec(event.msg);
      if (m) {
        const cause = DEATH_CAUSES[m[1]!.toLowerCase()];
        return { ...event, msg: cause ? `died from ${cause}` : `killed with ${this.itemName(m[1]!)}` };
      }
    }
    return event;
  }
}

function toMap(entries: CatalogueEntry[]): Map<string, string> {
  const m = new Map<string, string>();
  for (const e of entries) if (e && typeof e.code === 'string' && typeof e.name === 'string') m.set(e.code.toLowerCase(), e.name);
  return m;
}
