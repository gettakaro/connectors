#!/usr/bin/env node
/**
 * gen-catalogue.mjs — builds the sidecar's item and entity catalogues.
 *
 * Why this exists: Takaro shows `name` in the item picker, in `itemSearch`, in every shop listing and in every
 * module message. A catalogue that ships `SMG_Unique_LargeMag_06` as a name is a catalogue that has burned a
 * previous campaign (memory: catalogue-human-names). So `code` is the id the GAME takes and `name` is what a
 * PLAYER reads, and a row that cannot supply both is dropped rather than faked.
 *
 *   code → `items.template_id` in Postgres, and what `AddItemToInventory.ItemName` expects
 *   name → the display name
 *
 * Source: the Dune: Awakening Community Wiki's public read-only API (https://api.awakening.wiki/items), whose
 * `item_id` field is the game's template id. **It is CC BY-NC-SA 4.0** — see `data/SOURCES.md`; the generated file
 * is a regenerable artefact that carries its own attribution, not something we relicense.
 *
 * Nothing here is copied from a community tool: this script and its output shape are ours (own-code rule).
 *
 * Usage:
 *   node gen-catalogue.mjs                       # fetch and write into the sidecar's data/
 *   node gen-catalogue.mjs --out DIR             # write somewhere else
 *   node gen-catalogue.mjs --input items.json    # use a saved API response instead of the network
 *   node gen-catalogue.mjs --dry-run             # report only, write nothing
 *   node gen-catalogue.mjs --licence             # print the attribution block and exit
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_OUT = path.resolve(HERE, '..', 'data');

const SOURCE = {
  name: 'Dune: Awakening Community Wiki API',
  url: 'https://api.awakening.wiki/items',
  site: 'https://awakening.wiki',
  licence: 'CC-BY-NC-SA-4.0',
  licenceName: 'Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International',
  licenceUrl: 'https://creativecommons.org/licenses/by-nc-sa/4.0/',
  note: 'Dune: Awakening game content and materials © Funcom Oslo AS.',
};

const PAGE_SIZE = 200;

// ---------------------------------------------------------------------------
// Name quality — the whole point of the generator
// ---------------------------------------------------------------------------

/**
 * A "dev name" is anything that looks like an asset id rather than English. The wiki is hand-written, so most rows
 * are fine, but a few are stubs where an editor pasted the asset name into the title field — those are exactly the
 * rows that must not reach Takaro.
 */
export function isDevName(name, code) {
  if (!name) return true;
  const n = name.trim();
  if (!n) return true;
  if (/_/.test(n)) return true; // Snake_Case / Asset_Names
  if (/^(BP|DT|SK|SM|T|D|UI|WBP|ABP)_/.test(n)) return true; // UE asset prefixes
  if (/[a-z][A-Z]/.test(n)) return true; // a CamelCaseRun anywhere
  if (!/[a-z]/.test(n)) return true; // ALLCAPS ids
  // `Thing07` — but ONLY as a single token: "Albatross Wing Module Mk4" and "Artisan Disruptor M11" are real
  // in-game names and must survive.
  if (!/\s/.test(n) && /^[A-Za-z]+\d+$/.test(n)) return true;
  // `name === code` is NOT by itself a dev name: a single plain English word can legitimately be its own id
  // ("Kindjal", "Literjon", "Corpse"). It is only suspicious once it also looks like an asset id, which the rules
  // above already cover — so anything reaching here is accepted.
  return false;
}

// Windows-1252 characters for bytes 0x80-0x9F, which Latin-1 leaves as control codes.
const CP1252 = new Map(
  [...'\u20ac\u0081\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\u0160\u2039\u0152\u008d\u017d\u008f\u0090\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\u0161\u203a\u0153\u009d\u017e\u0178'].map((ch, i) => [ch, 0x80 + i]),
);

/**
 * Some wiki rows were saved as UTF-8 read back as Windows-1252 ("â€”" for "—"). Undo that when the text decodes
 * cleanly as UTF-8; anything else is returned unchanged.
 */
export function fixMojibake(value) {
  if (typeof value !== 'string' || !/[\u00c2-\u00f4][\u0080-\u00bf\u0152-\u2122]/.test(value)) return value;
  const bytes = [];
  for (const ch of value) {
    const code = ch.codePointAt(0);
    if (code <= 0xff && !(code >= 0x80 && code <= 0x9f)) bytes.push(code);
    else if (CP1252.has(ch)) bytes.push(CP1252.get(ch));
    else return value;
  }
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(Uint8Array.from(bytes));
  } catch {
    return value;
  }
}

/** Wiki text carries MediaWiki link markup; Takaro shows it raw, so it is flattened here. */
export function cleanText(value) {
  if (typeof value !== 'string') return undefined;
  const out = fixMojibake(value)
    .replace(/\[\[:?(?:[^\]|]*\|)?([^\]|]+)\]\]/g, '$1') // [[Category:Unique|Unique]] → Unique
    .replace(/'{2,}/g, '')
    .replace(/<[^>]+>/g, '')
    .replace(/\s+/g, ' ')
    .trim();
  return out || undefined;
}

// ---------------------------------------------------------------------------
// Fetch
// ---------------------------------------------------------------------------

async function fetchAllItems() {
  const rows = [];
  for (let page = 1; ; page += 1) {
    const url = `${SOURCE.url}?limit=${PAGE_SIZE}&page=${page}`;
    const response = await fetch(url, { headers: { accept: 'application/json' } });
    if (!response.ok) throw new Error(`${url} answered HTTP ${response.status}`);
    const body = await response.json();
    const list = Array.isArray(body?.list) ? body.list : [];
    rows.push(...list);
    process.stderr.write(`  page ${page}: ${list.length} row(s), ${rows.length}/${body?.pageInfo?.totalRows ?? '?'}\n`);
    if (body?.pageInfo?.isLastPage || list.length === 0) break;
    if (page > 200) throw new Error('refusing to page forever; the API never reported a last page');
  }
  return rows;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

export function buildItems(raw) {
  const rows = [];
  const byCode = new Map();
  const dropped = { noCode: 0, noName: 0, devName: 0, duplicate: 0 };
  const droppedExamples = [];

  for (const entry of raw) {
    const code = typeof entry?.item_id === 'string' ? entry.item_id.trim() : '';
    if (!code) {
      dropped.noCode += 1;
      continue;
    }
    const name = typeof entry?.name === 'string' ? fixMojibake(entry.name).trim() : '';
    if (!name) {
      dropped.noName += 1;
      if (droppedExamples.length < 20) droppedExamples.push({ code, name, why: 'no name' });
      continue;
    }
    if (isDevName(name, code)) {
      dropped.devName += 1;
      if (droppedExamples.length < 20) droppedExamples.push({ code, name, why: 'dev-style name' });
      continue;
    }
    if (byCode.has(code)) {
      dropped.duplicate += 1;
      continue;
    }
    const row = { code, name };
    const description = cleanText(entry.short_description) ?? cleanText(entry.long_description);
    if (description) row.description = description.length > 400 ? `${description.slice(0, 397)}…` : description;
    byCode.set(code, row);
    rows.push(row);
  }

  rows.sort((a, b) => a.name.localeCompare(b.name, 'en'));
  return { rows, dropped, droppedExamples };
}

/**
 * Entities.
 *
 * Honest state of play: `SpawnVehicleAt` takes a `ClassName` + `TemplateName` that live in the cooked content
 * (`DT_VehicleTemplates`), NOT in the shipped Linux server ELF and NOT in the wiki API — the wiki's `/vehicles`
 * endpoint lists vehicle MODULES (engines, hulls, wings), which are ordinary items and are already in items.json.
 * The only entity classes recoverable from the server binary are C++ RTTI names (`ASandwormPawn`,
 * `ADuneNpcCharacter`, …), and a C++ class name is not a display name.
 *
 * So this ships the one entity that can be both identified and NAMED, and says so. Adding rows whose `name` would
 * be `ADuneNpcCharacter` would fail the only rule this catalogue has.
 */
export function buildEntities() {
  return [
    {
      code: 'ASandwormPawn',
      name: 'Sandworm',
      // Takaro's entity type vocabulary is hostile | friendly | neutral.
      type: 'hostile',
      description: 'Shai-Hulud. Draws on ground vibration and destroys anything it reaches.',
    },
  ];
}

// ---------------------------------------------------------------------------
// Supplement — codes the wiki does not carry, observed in a real inventory
// ---------------------------------------------------------------------------

/**
 * The wiki's item list is a list of *things players craft and use*, so a handful of template ids that really do appear
 * in `dune.items` are simply absent from it. When the catalogue has no row, the inventory mapper falls back to the code
 * — which is how Tester ended up looking at `Emote_Bow_01` and `SolarisCoin` in Takaro's inventory screen. A dev name
 * reaching Takaro as a display name is the one catalogue rule this campaign must not break.
 *
 * So these rows are hand-written here, from the live database
 * (`select distinct template_id from dune.items`, TakaroTest's pawn, 2026-09-21), and each carries:
 *
 *  - `name`     — the human name, derived from the code by a human (nothing is machine-guessed).
 *  - `category` — `emote` / `cosmetic` / `currency`.
 *  - `giveable` — `false` for anything it is not meaningful to grant. The sidecar keeps every row RESOLVABLE (so an
 *                 inventory or a log line shows "Emote: Bow") but withholds non-giveable rows from `listItems`, i.e.
 *                 from Takaro's item picker, the shop and module `giveItem`. The field is a marker for OUR loader
 *                 only: Takaro's `IItemDTO` validates with `forbidNonWhitelisted: false, whitelist: true`, so an extra
 *                 key on a listItems row is neither rejected nor preserved — which is exactly why the exclusion is
 *                 done on our side rather than by shipping a flag and hoping Takaro honours it.
 *
 * These are the only hand-written item rows, they are marked `derived: true`, and `SOURCES.md` says so.
 */
const EMOTES = [
  ['Emote_Bow_01', 'Bow'],
  ['Emote_Clap_01', 'Clap'],
  ['Emote_Follow_01', 'Follow Me'],
  ['Emote_IxianSecret_01', 'Ixian Secret'],
  ['Emote_No_01', 'No'],
  ['Emote_Point_01', 'Point'],
  ['Emote_ShakeOffSand_01', 'Shake Off Sand'],
  ['Emote_Sit_01', 'Sit'],
  ['Emote_Threaten_01', 'Threaten'],
  ['Emote_Yes_01', 'Yes'],
];

const COSMETICS = [
  ['Social_Choam_MaulaCastOffs01_Bottom', 'Maula Cast-Offs Trousers'],
  ['Social_Choam_MaulaCastOffs01_Gloves', 'Maula Cast-Offs Gloves'],
  ['Social_Choam_MaulaCastOffs01_Shoes', 'Maula Cast-Offs Boots'],
  ['Social_Choam_MaulaCastOffs01_Top_Fremkit', 'Maula Cast-Offs Fremkit Top'],
];

export const SUPPLEMENT = [
  ...EMOTES.map(([code, label]) => ({
    code,
    name: `Emote: ${label}`,
    description: 'An unlocked emote animation. Not a carryable item; excluded from item lists and grants.',
    category: 'emote',
    giveable: false,
    derived: true,
  })),
  ...COSMETICS.map(([code, name]) => ({
    code,
    name,
    description: 'A cosmetic outfit piece from the CHOAM social wardrobe.',
    category: 'cosmetic',
    giveable: false,
    derived: true,
  })),
  {
    code: 'SolarisCoin',
    // Funcom's own SQL confirms this id is the currency stack in the backpack:
    // `migrate_clamp_max_allow_solaris` … `WHERE inventory_type = 0 AND template_id = 'SolarisCoin'`.
    // The in-game name of the currency is Solari.
    name: 'Solari',
    description: 'The currency of the Imperium. Stored as a single stack in the backpack.',
    category: 'currency',
    giveable: true,
    derived: true,
  },
];

/** Adds the supplement, letting a real wiki row win on any code the wiki does cover. */
export function withSupplement(rows, supplement = SUPPLEMENT) {
  const have = new Set(rows.map((r) => r.code.toLowerCase()));
  const added = supplement.filter((r) => !have.has(r.code.toLowerCase()));
  return { rows: [...rows, ...added].sort((a, b) => a.name.localeCompare(b.name, 'en')), added: added.length };
}

// ---------------------------------------------------------------------------
// Validate — the generator refuses to write a catalogue it would not defend
// ---------------------------------------------------------------------------

export function validate(rows, label) {
  const problems = [];
  const seen = new Set();
  for (const row of rows) {
    if (!row.code) problems.push(`${label}: a row has no code`);
    if (!row.name) problems.push(`${label}: ${row.code} has no name`);
    if (seen.has(row.code.toLowerCase())) problems.push(`${label}: duplicate code ${row.code}`);
    seen.add(row.code.toLowerCase());
    // The dev-name gate applies to the hand-written supplement too. Its CODES are asset ids on purpose (that is what
    // the game takes), but its NAMES were written by a human and must pass the same bar as a wiki row.
    if (isDevName(row.name, row.code)) problems.push(`${label}: ${row.code} has a dev-style name '${row.name}'`);
  }
  return problems;
}

function sourcesMarkdown(itemCount, entityCount, generatedAt) {
  return `# Catalogue sources

Generated by \`scripts/gen-catalogue.mjs\` on ${generatedAt}. **Do not edit \`items.json\` or \`entities.json\` by
hand — re-run the generator.**

## items.json (${itemCount} rows)

| | |
|---|---|
| Source | [${SOURCE.name}](${SOURCE.url}) — [${SOURCE.site}](${SOURCE.site}) |
| Fields used | \`item_id\` → \`code\`, \`name\` → \`name\`, \`short_description\`/\`long_description\` → \`description\` |
| Licence | **${SOURCE.licence}** — ${SOURCE.licenceName} (<${SOURCE.licenceUrl}>) |
| Game content | ${SOURCE.note} |

\`code\` is the game's item template id: it is what \`dune.items.template_id\` stores and what the GM command
\`AddItemToInventory.ItemName\` takes. \`name\` is the player-facing display name.

### Licence caveat — read before shipping this file

CC BY-NC-SA 4.0 is **non-commercial** and **share-alike**. That is unproblematic for an operator generating this
catalogue on their own server. It is **not** obviously fine for a redistributed connector build, and that decision
belongs to the release lane, not to this script:

* \`data/items.json\` and \`data/entities.json\` are **regenerated artefacts**, not hand-maintained source. They are
  currently tracked in git so that a clean checkout and CI can build an image with a working \`listItems\`, and
  \`npm run catalogue\` refreshes them.
* Attribution travels with the data: the generated JSON carries \`source\`, \`sourceUrl\`, \`license\`,
  \`licenseUrl\` and \`generatedAt\`, this file is written beside it, and the sidecar echoes \`itemsSource\` on
  \`/health\`.
* **Open decision for the release lane:** whether the published tarball/image may bundle the dataset at all. If it
  may not, untrack these two files and have the installer run the generator (it needs outbound HTTPS to
  \`api.awakening.wiki\`, about a dozen requests).
* The connector's test suite deliberately does **not** read these files — it uses committed fixtures under
  \`src/testing/fixtures/\` — so nothing in the build depends on who last ran the generator.

There is no second source for these names: the item template ids do not appear in the shipped Linux server ELF (they
live in the cooked content), and the server's \`Config/Tags/ItemTags.ini\` carries gameplay tags, not template ids or
display names. So the wiki is the only place a human name for a code can come from today, and no code in this file
has been confirmed against a live \`AddItemToInventory\` yet.

## entities.json (${entityCount} rows)

Deliberately tiny. \`SpawnVehicleAt\` takes a \`ClassName\`/\`TemplateName\` pair that lives in the cooked
\`DT_VehicleTemplates\` datatable — absent from the Linux server binary and from the wiki API (whose \`/vehicles\`
endpoint lists vehicle *modules*, which are ordinary items and already in \`items.json\`). The only entity classes
recoverable from the server ELF are C++ RTTI names such as \`ASandwormPawn\` and \`ADuneNpcCharacter\`, and a C++
class name is not a display name. Everything that could not be given a proper human name was left out — see the
report in \`research/2026-09-21-catalogue.md\`.
`;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

function parseArgs(argv) {
  const args = { out: DEFAULT_OUT, input: null, dryRun: false, licence: false };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--out') args.out = path.resolve(argv[++i] ?? '');
    else if (arg === '--input') args.input = path.resolve(argv[++i] ?? '');
    else if (arg === '--dry-run') args.dryRun = true;
    else if (arg === '--licence' || arg === '--license') args.licence = true;
    else if (arg === '--help' || arg === '-h') args.help = true;
    else throw new Error(`Unknown argument '${arg}'`);
  }
  return args;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help) {
    process.stdout.write(fs.readFileSync(fileURLToPath(import.meta.url), 'utf8').split('*/')[0].replace(/^\/\*\*?/, ''));
    return;
  }
  if (args.licence) {
    process.stdout.write(`${SOURCE.name} <${SOURCE.site}> — ${SOURCE.licenceName} (${SOURCE.licence}, ${SOURCE.licenceUrl})\n${SOURCE.note}\n`);
    return;
  }

  let raw;
  if (args.input) {
    process.stderr.write(`Reading ${args.input}\n`);
    const parsed = JSON.parse(fs.readFileSync(args.input, 'utf8'));
    raw = Array.isArray(parsed) ? parsed : (parsed.list ?? []);
  } else {
    process.stderr.write(`Fetching ${SOURCE.url}\n`);
    raw = await fetchAllItems();
  }

  const generatedAt = new Date().toISOString();
  const { rows: wikiItems, dropped, droppedExamples } = buildItems(raw);
  // Codes that really appear in `dune.items` but the wiki does not list, with hand-written human names.
  const { rows: items, added: supplemented } = withSupplement(wikiItems);
  const entities = buildEntities();

  const problems = [...validate(items, 'items'), ...validate(entities, 'entities')];
  if (problems.length) {
    process.stderr.write(`\nREFUSING TO WRITE — ${problems.length} problem(s):\n${problems.slice(0, 20).map((p) => `  ${p}`).join('\n')}\n`);
    process.exitCode = 1;
    return;
  }

  process.stderr.write(
    `\n${raw.length} API row(s) → ${items.length} item(s) (${supplemented} hand-written supplement row(s)), ` +
      `${entities.length} entity/entities.\n` +
      `Dropped: ${dropped.noCode} without a code, ${dropped.noName} without a name, ${dropped.devName} with a dev-style name, ${dropped.duplicate} duplicate.\n`,
  );
  if (droppedExamples.length) {
    process.stderr.write(`Dropped examples:\n${droppedExamples.map((d) => `  ${d.code} → '${d.name}' (${d.why})`).join('\n')}\n`);
  }
  process.stderr.write(`\nSample:\n${items.slice(0, 10).map((r) => `  ${r.code.padEnd(40)} ${r.name}`).join('\n')}\n`);

  if (args.dryRun) {
    process.stderr.write('\n--dry-run: nothing written.\n');
    return;
  }

  fs.mkdirSync(args.out, { recursive: true });
  const meta = { source: SOURCE.name, sourceUrl: SOURCE.url, license: SOURCE.licence, licenseUrl: SOURCE.licenceUrl, generatedAt };
  fs.writeFileSync(path.join(args.out, 'items.json'), `${JSON.stringify({ ...meta, rows: items }, null, 1)}\n`);
  fs.writeFileSync(
    path.join(args.out, 'entities.json'),
    `${JSON.stringify({ ...meta, source: 'server ELF RTTI (hand-named)', sourceUrl: '', rows: entities }, null, 1)}\n`,
  );
  fs.writeFileSync(path.join(args.out, 'SOURCES.md'), sourcesMarkdown(items.length, entities.length, generatedAt));
  process.stderr.write(`\nWrote ${path.join(args.out, 'items.json')}, entities.json and SOURCES.md\n`);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  main().catch((err) => {
    process.stderr.write(`${err.message}\n`);
    process.exitCode = 1;
  });
}
