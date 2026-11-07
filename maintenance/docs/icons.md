# Icon Packs

An **Icon Pack** is the set of small item icons for one game, served to Takaro from an icon bucket.
`takaro-maint icons` builds a pack from a recipe document, or from the catalog, and publishes it. Game binaries and raw
game files are still never uploaded or committed; only the small derived icons (WebP, at most 128px)
go to the icon buckets, and they are not kept in git (see [support-policy.md](support-policy.md)).

## Commands

| Command | What it does |
|---|---|
| `icons inputs --game G [--target ID] [--platform P]` | List every input a catalog-driven build needs. No network. |
| `icons build --recipes FILE --out DIR` | Render a recipe document into `DIR/pack/<game>/`, plus a coverage report, a contact sheet and a build record outside the pack. `DIR` must be absent or empty. |
| `icons build --game G [--target ID] [--platform P] [--client DIR] --out DIR` | The same, from the catalog: fetch the game data, read the client files, map them to recipes and render. Excludes `--recipes`. See [Catalog-driven builds](#catalog-driven-builds). |
| `icons publish --env staging --pack DIR [--dry-run]` | Merge the pack into the live index and upload it to the staging bucket. `DIR` is `…/pack/<game>`. |
| `icons promote --game G [--dry-run]` | Copy a game's pack from staging to production with a rolling merge. |
| `icons set-enabled --env E --game G --enabled true\|false [--dry-run]` | Rewrite only the index's `enabled` flag. |
| `icons takedown --env E --game G (--code C… \| --object O…) [--dry-run]` | Remove exactly the named codes or objects. |

Not built: the live item-code list with `--codes` (#811, skipped for now, so coverage is measured
against codes derived from game data and the report says so).

## The recipe document

```json
{
  "schemaVersion": 1,
  "game": "fixture-846",
  "measuredAgainst": "game-data",
  "recipes": [
    {"code": "fixture-circle", "kind": "item", "layers": [{"image": "images/circle-512.png", "ops": []}], "composite": "over"}
  ],
  "unmatched": [
    {"code": "fixture-ghost", "kind": "item", "reason": "no-icon-in-game"}
  ]
}
```

- `game` is a valid game identifier (below). Codes are non-empty, at most 256 characters, without
  control characters, and unique across `recipes` and `unmatched` together.
- `kind` is optional: `item` or `block`. `reason` matches `^[a-z][a-z0-9-]*$` and is an open set.
- `image` is a PNG path relative to the recipe file, checked by `paths.safe_relative`. A missing
  file is exit 4.
- A recipe takes exactly one layer. `ops` may hold `{"op": "trim"}` (crop to the alpha bounding box;
  a fully transparent image is exit 6); other image operations and layer compositing arrive with the
  games that need them. `measuredAgainst` can only be `game-data`.
- Optional: `rule` on a recipe or unmatched entry (`^[a-z][a-z0-9-]*$`, how it was mapped), `detail` on
  an unmatched entry (at most 256 characters, no control characters) and `codesFrom` on the document
  (where the codes come from, at most 1000 characters).

The synthetic fixture is `maintenance/tests/fixtures/icons/fixture-846/`.

## Catalog-driven builds

`icons build --game G` builds a game's pack from the catalog, so a maintainer does not write recipes
by hand: the item data comes from the game's server depot, the pixels from an installed game client,
and a per-game mapping module joins them into the recipe document the builder already takes. Project
Zomboid (`zomboid`) is the first game on this route.

```bash
maintenance/bin/takaro-maint icons inputs --game zomboid           # what the build needs; no network
maintenance/bin/takaro-maint icons build --game zomboid --client DIR --out OUT
```

`--game` (with `--target`, `--platform` and `--client`) and `--recipes` are mutually exclusive. `--target`
and `--platform` select the target as everywhere else; the default target is used when neither is given.

### The `icons` block

An optional block of `catalog/<game>/game.json`, validated by `catalog/schema/v1/icons.schema.json` and
by the `icons-*` checks of `catalog validate` (none of them needs the network). It is not part of the
target fingerprint.

```json
"icons": {
  "game": "zomboid",
  "mapper": "zomboid",
  "data": {"source": "server-depot", "input": "server", "depots": ["380871"], "files": ["regex:^media/scripts/.*\\.txt$"]},
  "pixels": {
    "source": "client", "app": 108600, "depot": 108604, "decoder": "pz-texturepack",
    "files": ["media/texturepacks/UI2.pack", "media/texturepacks/UI.pack", "..."],
    "clientBuilds": {"42.21.0": [25485521]}
  },
  "secrets": []
}
```

- `game` is the pack directory and the identifier Takaro looks the pack up by. It is what the connector
  reports as `payload.game`, not necessarily the catalog id. It must be a valid game identifier and not
  `_store`.
- `mapper` names a registered mapping module (`icons/mappers.py`).
- `data` is the server-side input. `input` is the target's `steam-depots` input, `depots` an optional
  subset of its depots, `files` DepotDownloader file-list selectors (an exact path, or `regex:` with a
  case-insensitive pattern: the semantics of `steam/selectors.py`).
- `pixels` is the client-side input. `app` and `depot` are the Steam client app and depot, used for the
  appmanifest lookup. `files` are exact client-relative paths in **precedence order**: a texture name
  held by several packs comes from the first one listed. Globs are not allowed, so a backup or
  temporary pack next to the real ones can never be picked up. `clientBuilds` maps a target revision to
  the client build ids of the same release.
- `secrets` names environment variables the build reads, never their values. `icons inputs` reports
  whether each is set.

### Acquisition and its cache

The server data is fetched through the pinned DepotDownloader at the target's pinned manifests with a
file list, so a build downloads megabytes, not the 7 GB depot. The filtered files live in a cache of
their own:

```
$TAKARO_MAINT_CACHE/icons/steam/<app>/<depot>/<manifest>/<sha256(selectors)[:16]>/   # files + .takaro/complete.json
$TAKARO_MAINT_CACHE/icons/work/build-*/                                              # removed after every run
$TAKARO_MAINT_CACHE/icons/clients/<game>/<app>/                                       # suggested client mirror
```

It is deliberately not `steam/<app>/<depot>/<manifest>`: that directory means "the whole depot" and the
installer trusts its marker. The marker records every file's sha256. A hit is re-hashed; a mismatch is
a warning and one fresh download, and a mismatch after that is exit 5. A manifest Steam no longer
serves (anonymous access is refused about two weeks after a build is superseded) is exit 4 with the
instruction to re-pin the target to the current public build
(`takaro-maint steam pin --game G --target ID --write`) and rebuild. There is no fallback to the branch
head. Decoded textures are written under `icons/work`, never into `--out`.

### Client mirror runbook (Project Zomboid)

The client is not downloaded. Point `--client` at a directory that holds the listed pack files (and,
for the drift warning below, the client's `appmanifest_<app>.acf`, either in that directory or two
levels up as in a Steam library: `steamapps/common/<install>`). From a Windows machine that has the
game, over ssh (Windows' bundled `tar` streams through the default shell):

```bash
CACHE="$(maintenance/bin/takaro-maint icons inputs --game zomboid | jq -r '.inputs[]|select(.role=="pixels").suggestedMirror')"
FILES="$(maintenance/bin/takaro-maint icons inputs --game zomboid | jq -r '.inputs[]|select(.role=="pixels").files[]' | tr '\n' ' ')"
mkdir -p "$CACHE"
ssh HOST "tar -cf - -C \"D:\\Program Files (x86)\\Steam\\steamapps\\common\\ProjectZomboid\" $FILES" | tar -xf - -C "$CACHE"
ssh HOST 'tar -cf - -C "D:\Program Files (x86)\Steam\steamapps" appmanifest_108600.acf' | tar -xf - -C "$CACHE"
```

Compare `sha256sum` of each file with `Get-FileHash -Algorithm SHA256` on the machine. Keep the mirror
outside the repository; never commit game files. Only `buildid` and `InstalledDepots/<depot>/manifest`
are read from the appmanifest; the file also names the Steam account that owns the install.

### Project Zomboid: the texture packs

`pz-texturepack` (`icons/decoders/pz_texturepack.py`, in-process, no container) reads v1 and v2 packs:

```
v2 ("PZPK"):  magic "PZPK", int32 version (=1), int32 numPages, then per page:
              name, int32 numEntries, int32 mask, entries, int32 pngLength, PNG bytes
v1:           int32 numPages, then per page the same header and entries,
              then the PNG up to and including IEND, then 0xDEADBEEF
entry:        name, x, y, w, h, ox, oy, fw, fh   (int32 x 8)
```

Integers are little-endian int32; a string is an int32 length and then that many latin-1 bytes. A v1
PNG is found by walking its chunks to `IEND` (the terminator is not searched for, because its bytes
can occur inside compressed data). An entry is the rectangle `(x, y, w, h)` of the page, cut from an
original `fw x fh` frame at `(ox, oy)`; a texture is rebuilt as a transparent `fw x fh` canvas with
the crop pasted at that offset. A malformed pack (truncated, an unknown version, a missing
terminator, a rectangle or offset that does not fit, an entry name with a path separator) is exit 6.
Pages are decoded only when an entry of theirs is needed. The first occurrence of a name inside a pack
wins, and across packs the order of `pixels.files` decides; the build record counts `shadowedNames`
per pack.

### Project Zomboid: the mapping

The mapper (`games/zomboid/icons.py`) is a ScriptManager equivalent. It reads every `*.txt` under
`media/scripts` (sorted, `utf-8-sig` with a `cp1252` fallback), strips nested `/* */` comments and
finds `item <Name> { ... }` blocks inside `module <Name> { ... }`. Other blocks (`craftRecipe`,
`entity`, `model`, ...) are skipped, which keeps lines such as `item 1 [Base.Plank]` out of the code
list. An item's properties are its own `key = value` pieces split on `,`; nested blocks such as
`component FluidContainer` are skipped, keys are case-insensitive and the last one wins. A code is
`<module>.<item>`; a repeated full name counts once (the repeats in build 25485538 are identical
redefinitions).

An item is left out of the code list when the game would not register it. Read from the game jar
(`ScriptBucket`, build 25485538): `OBSOLETE = true`, `enabled` given as anything but `true`, and
`debugOnly = true` (a dedicated server is not in debug mode). `ScriptManager.getAllItems()` applies no
other filter: items marked `hidden` (for example the wound overlays) are registered, so they stay in
the code list and show up as `no-icon-in-game`.

Each item is mapped by the first rule that applies; `rule` is recorded on every recipe and unmatched
entry, and `detail` names the texture or sprite that was missing:

| Rule | When | Recipe |
|---|---|---|
| `moveable` | `Icon = default` | the `WorldObjectSprite` texture, trimmed to its alpha bounding box (`trim`), then downscaled to 128px. No sprite is `no-world-sprite`, an absent one `missing-sprite`. |
| `icon` | a non-empty `Icon` | texture `Item_<Icon>` at its full frame. Absent is `missing-texture`. |
| `icons-for-texture` | no usable `Icon`, but `IconsForTexture = A;B;C` | texture `Item_<first piece>`. |
| `none` | none of these | unmatched, `no-icon-in-game` (expected for wound, damage and make-up overlays). |

Nothing is guessed from names. Only a texture an item's own properties name is used.

### The build record and client drift

`build-record.json` of a catalog build adds, to the recipe hash and encoder: `source`, `catalogGame`,
`target`, `revision`, `fingerprint`; `data` (app, branch, buildid, os, depot manifests, selectors, file
count and `treeSha256`, a sha256 over the sorted `<path>\t<sha256>` lines); `pixels` (the decoder and,
per pack, `sha256`, `size`, `layout`, `pages`, `entries`, `codesSupplied`, `shadowedNames`); `client`
(app, depot, `buildid`, `manifest`, `status`, `expected`); `mapper` (name, version, notes); `tools`;
and `warnings`. It holds no timestamp, so two identical builds write identical records.

Server and client build ids differ for the same release (25485538 and 25485521 for 42.21.0), so
`pixels.clientBuilds` lists the client builds of each revision. `client.status` is `match` (the build is
listed for the target's revision), `older` or `newer` (it is not, and it is lower or higher than the
listed ones; the warning names another revision that lists it), or `unknown` (no appmanifest, or the
revision lists no client builds). Anything but `match` is a warning in stderr, `warnings` of the
output and the build record, and the exit code stays 0. A new game release needs a `clientBuilds` entry.

### Known limitations

- Coverage is measured against codes derived from game data, not against a live code list from a
  server (#811). `coverage.json` says so in `measuredAgainst`, `note` and `codesFrom`.
- Tinted overlays (`IconColorMask`, `IconFluidMask`) are ignored: the base icon is published.
- An item that names another item's art through anything but `Icon`, `IconsForTexture` or
  `WorldObjectSprite` has no icon here.

## The pack contract

Shared with Takaro, whose reader rejects the whole index when it breaks a rule.

```
<game>/index.json
<game>/<sha256-of-webp-bytes>.webp
```

```json
{"game": "<game>", "enabled": true, "icons": {"<item code>": "<sha256>.webp"}}
```

- Game identifier: `^[a-z0-9][a-z0-9-]*$`, at most 64 characters. `_store/` belongs to Takaro's Icon
  Store (takaro ADR 0051) and is never written, checked or deleted by this tool.
- Objects are WebP, longest edge at most 128px, never upscaled, named by the sha256 of their bytes,
  so identical art is stored once.
- `index.json` has exactly the keys `game`, `enabled`, `icons`; it is serialized by one function
  (`contract.serialize_index`: sorted codes, two-space indent, trailing newline), so build and
  publish agree byte for byte.
- Headers: `.webp` is `image/webp` with `public, max-age=31536000, immutable`; `index.json` is
  `application/json` with `public, max-age=300`.

`contract.check_pack_dir` runs at the end of `icons build` and again at the start of `icons publish`.
It needs the directory to be named for its game, to hold only `index.json` and `<sha256>.webp`
files, every referenced object to exist, every object to be referenced, every hash to match its
name and every image to open as WebP within 128px.

## WebP encoding and the Pillow pin

Rendering is pinned so that the same recipes give the same bytes: open the PNG, convert to RGBA,
copy the pixels into a fresh image (no ICC, EXIF or XMP), downscale a longest edge over 128 with
LANCZOS on premultiplied pixels, then encode lossless (`quality=100, method=6, exact=False`).

The bytes depend on the Pillow wheel and the libwebp it bundles, so `pillow` is pinned exactly in
`pyproject.toml`. Bumping it changes object hashes. That is acceptable, because objects are
content-addressed and the rolling merge repoints codes, but do it deliberately. `build-record.json`
logs both versions.

## Reports

- `coverage.json`: total, matched, unmatched, `coverage` (matched over total), object count,
  `byKind` (only when entries carry a kind), `byRule` (only when they carry a rule),
  `unmatchedByReason`, and the sorted `unmatchedCodes` with their reasons (and `detail`).
  `measuredAgainst` is `game-data`, with a note saying so and, when the document has it, `codesFrom`.
- `contact-sheet.png`: up to 64 matched icons, eight columns, on a checkerboard with the code
  underneath. Beyond 64 it is an evenly spaced sample, or, when recipes carry a `rule`, a stratified
  one: slots are shared out in proportion, at least 8 per rule where it has them, and the sheet is
  sorted by rule and then code, so a rare rule such as moveables is always on it.
- `build-record.json`: the recipe file's sha256, the encoder versions and settings, and the counts;
  a catalog build adds what it was built from (see above).
- `recipes.json` (catalog builds only): the recipe document the mapper produced, for review. Its
  sha256 is the build record's `recipesSha256`.

None of them is inside `pack/`, so none is published.

## Publish protocol

1. Refuse any `--env` other than `staging` (production is written by `icons promote`).
2. Refuse a `_store/` pack, then run the contract check on the local pack.
3. Resolve credentials from the environment. No client exists before this step.
4. `GET <game>/index.json`. A 404 means there is no live index; an invalid one (or one for another
   game) stops the run with exit 7 before anything is uploaded.
5. Merge: every live code survives, a changed code points at the new object, and `enabled` is
   taken from the live index (a takedown is not undone; a new pack is enabled). The merged index is
   validated.
6. `HEAD` every object; existing ones are skipped.
7. In a dry run, stop and print the plan.
8. `PUT` each missing object, then `PUT` `index.json` last, then `GET` it back and compare. An index
   that merges to the live bytes is not uploaded (`unchanged`). A failed object upload stops the
   run with exit 10 and the index is not uploaded; a rerun skips what already landed. Old objects
   are never deleted.

stdout is one JSON document with `objects` (`uploaded` or `wouldUpload`, and `skipped`) and `index`
(`action`, `codes`, `enabled`, `added`, `repointed`, `kept`, `unchanged`). The endpoint, bucket and
keys are never printed.

## Promote

`icons promote --game G [--dry-run]` reads staging and writes production; it has no `--env`. It
never writes staging.

1. Validate the game (`_store` and malformed names exit 2), resolve both buckets, and refuse when
   staging and production resolve to the same endpoint and bucket. No request is made before this.
2. `GET` the staging index (404 is exit 4: publish it to staging first) and the production index
   (404 means none yet). An invalid index of either exits 7.
3. Merge with the same rolling merge as `publish`: the production index keeps every code it has,
   staging codes are added, a code in both points at the staging object, and production's `enabled`
   is kept (a pack new to production takes staging's flag). A disabled result prints a warning.
4. `HEAD` every object staging references in production. Production-only codes' objects are not
   touched.
5. A dry run checks those objects exist in staging and stops.
6. `GET` every missing object from staging and check it (hash equals its name, a WebP of at most
   128px) before any production write.
7. `PUT` the missing objects, then the index last, then `GET` it back. An index that merges to the
   live bytes is not uploaded.

stdout: `from`, `to`, `game`, `dryRun`, `objects` (`copied` or `toCopy`, and `skipped`) and `index`
(as for `publish`).

## Set-enabled

`icons set-enabled --env staging|production --game G --enabled true|false [--dry-run]` rewrites the
index with the same `game` and `icons` and the requested flag, then reads it back. A real flip makes
exactly three requests: `GET` index, `PUT` index, `GET` index. No object is read, written or
deleted. Setting the current value is `unchanged`. stdout: `env`, `game`, `dryRun`,
`enabled: {from, to}` and `index: {action, codes}`.

## Takedown

`icons takedown --env staging|production --game G (--code C… | --object O…) [--dry-run]`, both
flags repeatable. Exactly one of the two kinds must be given; each `--object` is a bare
`<sha256>.webp` name as it appears in the index.

- `--code`: exactly those codes leave the index. Their objects are deleted unless another remaining
  code still points at them; those are retained and reported. An unknown code is exit 2.
- `--object`: those objects are deleted and every code pointing at them leaves the index. An object
  that is not indexed is still deleted when it exists in the bucket (art orphaned by a repointed
  code); one that is neither indexed nor in the bucket is exit 2.

The index (`enabled` kept, `icons` may become empty) is republished and read back first, then each
object is deleted and checked with a `HEAD`. If a delete fails, the exit is 10 with
`pendingObjects`; the index is already republished, so rerun with `--object <pending…>`. stdout:
`env`, `game`, `dryRun`, `removedCodes`, `objects` (`deleted` or `toDelete`, and `retained`) and
`index`.

Deleted objects were served with `immutable, max-age=31536000` and can stay in the Cloudflare edge
cache; the command prints the keys to purge. Purging is manual (no Cloudflare API credentials are
used).

`promote` merges staging into production, so a code taken down only in production comes back on the
next promote while staging still has it. Take down in staging first, then production. `publish` and
`promote` both preserve a live `enabled: false`.

## Production promotion

The CI workflow and the GitHub Environments (`icons-staging`, `icons-production`) for promotion were
not built: the maintainer decided against holding production secrets in a public repository.
`promote` is a local command for whoever holds the production credentials, and it has been exercised
only against the S3 fake, never against production.

## Environment

| Variable | Use |
|---|---|
| `STG_R2_ACCESS_KEY_ID`, `STG_R2_SECRET_ACCESS_KEY`, `STG_R2_ENDPOINT`, `STG_R2_BUCKET` | The staging bucket (Cloudflare R2, S3-compatible, path-style). All four are redacted. The endpoint must be `https://` (loopback `http://` is allowed for the test fake). |
| `STG_ICON_HOST` | Public staging host, for curl verification only. No command reads it. |
| `PROD_R2_ACCESS_KEY_ID`, `PROD_R2_SECRET_ACCESS_KEY`, `PROD_R2_ENDPOINT`, `PROD_R2_BUCKET` | The production bucket, read by `promote` (destination) and by `set-enabled` / `takedown` with `--env production`. Same rules as the staging variables; all four are redacted. `icons publish` never reads them. |
| `PROD_ICON_HOST` | Public production host, for curl verification only. No command reads it. |

## Exit codes

| Code | When |
|---|---|
| 0 | ok, including a dry run and a republish where nothing changed |
| 2 | usage or configuration: a bad flag or recipe document, `--env` other than `staging` (`publish`) or other than `staging`/`production` (the others), a `_store/` or otherwise invalid game or key, a missing `STG_R2_*` / `PROD_R2_*` variable, a non-https endpoint, staging and production resolving to the same bucket, a non-empty `--out`, an unsupported op or multi-layer recipe, a game without an `icons` block, `--recipes` and `--game` both or neither, `--client` without `--game`, a missing `--client` for a client-sourced build, an unknown code or object, a malformed `--object`, `--code` and `--object` both or neither, `--enabled` other than `true`/`false` |
| 3 | a catalog build or `inputs` for a target that does not exist |
| 4 | a recipe image file is missing, or there is no index for the game in the environment being read; a catalog build whose `--client` is not a directory or lacks a listed pack, or whose pinned server manifest Steam will not serve |
| 5 | a filtered depot cache that still does not match its marker after one fresh download; the index read back after upload differs from what was sent; a copied object does not hash to its name; an object is still present after a delete |
| 6 | a malformed texture pack, a `trim` of a fully transparent image |
| 7 | contract violation in the built pack, the local pack, the live index or the merged index; a staging object that is missing or is not a valid WebP of at most 128px |
| 10 | a bucket read, upload or delete failed (S3 error, unreachable endpoint, access denied) |

## Runbook: publish fixture-846 to staging

From the repository root, with the staging variables in a gitignored `.env` (never print them):

```bash
OUT="$(mktemp -d)/fixture-846"
maintenance/bin/takaro-maint icons build \
  --recipes maintenance/tests/fixtures/icons/fixture-846/recipes.json --out "$OUT"
( set -a; . ./.env; set +a
  maintenance/bin/takaro-maint icons publish --env staging --pack "$OUT/pack/fixture-846" --dry-run )
( set -a; . ./.env; set +a
  maintenance/bin/takaro-maint icons publish --env staging --pack "$OUT/pack/fixture-846" )
```

A second publish must skip all five objects and leave the index `unchanged`. Verify through the
public host:

```bash
curl -fsS https://icons.takaro.dev/fixture-846/index.json | jq .
curl -sSI https://icons.takaro.dev/fixture-846/index.json | grep -iE '^(HTTP|content-type|cache-control)'
cmp <(curl -fsS https://icons.takaro.dev/fixture-846/index.json) "$OUT/pack/fixture-846/index.json"
for f in $(curl -fsS https://icons.takaro.dev/fixture-846/index.json | jq -r '.icons[]' | sort -u); do
  curl -sS -o /dev/null -w "%{http_code} %{content_type} %header{cache-control} $f\n" "https://icons.takaro.dev/fixture-846/$f"
done
```

Expect `200 application/json public, max-age=300` for the index and
`200 image/webp public, max-age=31536000, immutable` for every object. A 404 right after the first
publish can be a cached miss at Cloudflare: retry with `?v=$(date +%s)` and again after a few minutes.
