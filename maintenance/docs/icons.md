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
- `ops` is a list applied to each layer in order, before the layers are composited. `{"op": "trim"}`
  crops to the alpha bounding box (a fully transparent image is exit 6). `{"op": "tint-multiply",
  "color": "#rrggbb"}` (lowercase, always six digits) multiplies the colour channels by the colour: straight,
  non-premultiplied RGB, `out = c * t // 255` per channel (floored, as Pillow's `ImageChops.multiply` does), alpha unchanged. That is a sprite
  coloured by a tint, texture times colour with a colour alpha of 1 (grey 128 under `#80ff40` is
  64, 128, 32). Mappers normalize game tint syntaxes into this form; the op never parses game text. A
  missing or malformed `color`, an extra key on either op, or any other op name is exit 2.
  `{"op": "atlas-cut", "rect": [x, y, w, h], "rotation": "none", "canvas": [cw, ch], "offset": [ox, oy]}` cuts
  one Unity sprite from its atlas (the layer image, `W x H`, top row first). `rect` is the sprite's `textureRect` in
  Unity texels with the **origin at the bottom-left** (four numbers >= 0, `w` and `h` > 0); edges are
  `l = round(x)`, `r = round(x + w)`, `b = round(y)`, `t = round(y + h)` with Python's `round` (half to even, which is
  what Pillow's float crop does inside UnityPy), a rect outside the atlas is exit 6, and the cut is
  `crop((l, H - t, r, H - b))`. `rotation` (required) undoes the sprite's packing rotation with a transpose table
  that is UnityPy's `SpriteHelper` conjugated by its final vertical flip: `none` nothing, `flip-horizontal`
  `FLIP_LEFT_RIGHT`, `flip-vertical` `FLIP_TOP_BOTTOM`, `rotate-180` `ROTATE_180`, `rotate-90` `ROTATE_90` (UnityPy's
  `ROTATE_270`, taken in bottom-up space). `canvas` (the sprite's full `rect` size) and `offset` (its
  `textureRectOffset`, origin bottom-left) come together or not at all: the cut is pasted on a transparent canvas of
  `(round(cw), round(ch))` at `left = round(ox)`, `top = round(ch) - round(oy) - cut height`, the frame the game
  draws a trimmed sprite in (a cut that does not fit is exit 6). The op never resamples.
  `{"op": "frame-crop", "frames": n, "frame": 0, "gap": 2}` takes one frame of a vertical animation strip.
  `frames` is an integer from 1 to 64, `frame` an integer >= 0 (default 0) and `gap` an integer from 0 to 16 (default
  0); any other key, type or range is exit 2. For an image `W x H`, `fh = H // frames` is the frame pitch and
  `h = fh - gap` the visible height; the result is `crop((0, frame * fh, W, frame * fh + h))`, the full width, never
  resampled. It is XNA's `Texture2D.Frame(1, frames, 0, frame, 0, -gap)`. `fh < 1`, `h < 1` or `frame >= frames` is
  exit 6 naming the recipe. `measuredAgainst` can only be `game-data`.
- A layer is `{"image": ..., "ops": [...]}` or `{"model": "<path to a block-model document>", "ops": [...]}`. A `model`
  layer is rendered by the block-model renderer (see "The block-model renderer" below) to a 128x128 RGBA image and its
  `ops` apply to that image like any other layer's. A model file or a texture file it names that is missing is exit 4; an
  invalid model document is exit 2 naming the recipe.
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
- `pixels` comes in two shapes. With `source: "server-depot"` the pixels are files of the same server
  depot as the data: `files` are DepotDownloader selectors with the semantics of `data.files`, fetched in
  the one filtered download together with the data files (the cache key hashes both lists), and the
  shape takes nothing else (`app`, `depot`, `access` and `clientBuilds` are schema errors; an optional
  `decoder: "unity"` names the container that decodes the pixel files, and the `icons-server-pixels-decoder` check
  requires it to equal `data.decoder`, because both roles are decoded from the one mounted tree). A
  route of this shape builds without a client: `icons build` refuses `--client` (exit 2, before any
  download), runs no client check, and records no `client` block, no client build and no client warning.
  With `source: "client"`, `app` and `depot` are the Steam client app and depot, used for the
  appmanifest lookup. `files` are exact client-relative paths in **precedence order**: a texture name
  held by several packs comes from the first one listed. Globs are not allowed, so a backup or
  temporary pack next to the real ones can never be picked up. `clientBuilds` maps a target revision to
  the client build ids of the same release.
- `secrets` names environment variables the build reads, never their values. `icons inputs` reports
  whether each is set.
- `data` has a second shape, `{"source": "connector", "files": ["games/<game>/..."]}`: the item codes come from
  files of this repository (the connector's own item list), which are hashed and copied; nothing is downloaded and
  no `input` is allowed. The `icons-data-input` check requires every file to exist in the repository.
- `code` (optional) is `{"source": "client", "decoder": "clr", "files": ["Terraria.exe"]}`: game code the mapper reads
  facts from, taken from the same installed client as the pixels. It requires `pixels.source: "client"`
  (`icons-code-role`).
- Client `pixels.files` entries may also be `regex:<pattern>` (case-insensitive, matched against client-relative
  paths, like `data.files`) as well as exact paths; each must compile and a regex is not allowed with
  `access: "iostore-ranges"` (`icons-client-regex`). `icons build` refuses a client where an exact path is missing or
  a regex matches nothing (exit 4, naming it); a regex only considers regular files and does not follow symlinks (exact paths are taken as they are).
- `xnb` (`{"alpha": "straight"}`) holds the settings of the `xnb` decoder and is required exactly when
  `pixels.decoder` is `xnb` (`icons-xnb-settings`).
- `data` has a third shape, `{"source": "public-download", "input": "game", "artifact": "server", "decoder":
  "minecraft-datagen", "reports": ["registries.json"]}`, and `pixels` a matching one, `{"source": "public-download",
  "artifact": "client", "downloads": {"<revision>": {"source", "path", "sha1", "size"}}}`. `input` names the target's
  `mojang-version` input, whose version manifest and server jar pins are the data pins; `downloads` pins the client jar per
  target revision (it is not part of the target, so the target fingerprint does not move). The checks
  `icons-public-download-roles` (data and pixels are `public-download` together), `icons-public-download-revisions` (each
  key is a revision of some target), `icons-public-download-pins` (each `source` is a source of the game record and each
  `path` is `/v1/objects/<sha1>/client.jar` for its own sha1) and `icons-data-input` (every pinned target has the
  `mojang-version` input) need no network. A target whose revision has no pin does not build (exit 2).

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
count and `treeSha256`, a sha256 over the sorted `<path>\t<sha256>` lines; for a server-depot pixel route
the selectors include the pixel selectors and the hashes of the pixel files are in `treeSha256` too);
`pixels` (the decoder and,
per pack, `sha256`, `size`, `layout`, `pages`, `entries`, `codesSupplied`, `shadowedNames`; for server
pixels `source: "server-depot"`, `selectors`, `files` (how many the depot served) and `used` (how many the
mapping referenced)); `client`
(app, depot, `buildid`, `manifest`, `status`, `expected`; absent for server pixels); `mapper` (name,
version, notes and the mapper's own counts); `tools`; and `warnings`. It holds no timestamp, so two identical builds write identical records.

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

### 7 Days to Die

The pack id is `7d2d`. Everything comes from the anonymous dedicated-server depot (app 294420, depot 294422), with
no game client and no decoder container: `Data/ItemIcons/*.png` are the pixels, `Data/Config/items.xml`,
`item_modifiers.xml` and `blocks.xml` the mapping, and `Data/Config/Localization.csv` the code filter. One filtered
download (about 123 MB for 5,265 files) serves all of them; the data and pixel selectors share the cache key.
`--client` is refused for this game (exit 2).

**Codes.** A code is the entry's `name` attribute. Entries are read in the order `items.xml` (`<item>`),
`item_modifiers.xml` (`<item_modifier>`, kind `item`), `blocks.xml` (`<block>`, kind `block`); the first
occurrence of a name wins, and a repeat (also across files) is counted in the mapper notes. An entry is left out
of the code list when `Localization.csv` has no row for its name, an empty or blank `english` value, or an
`english` value equal to the key. The file is read with `csv` and `utf-8-sig`, columns found by header name
(`Key` and `english`, case-insensitive). The XML files start with a UTF-8 BOM and are parsed from bytes.
A `<!DOCTYPE` or `<!ENTITY` is exit 6, as is malformed XML or a header without those two columns; a file
the depot did not serve is exit 4 and named.

**Icon resolution.** Only an entry's direct `<property name= value=>` children without a `class` attribute count
(nested `Action` blocks are ignored; the last occurrence wins). For `CustomIcon` and `CustomIconTint`
independently: the entry's own value, else the value its `Extends` parent resolves to unless that hop's `param1`
(comma-split, trimmed) lists the property, else nothing. The icon falls back to the entry's own name, never the
parent's. `Extends` resolves inside the entry's own file; an unknown parent or a cycle ends inheritance and is
counted in the notes. The PNG is found by exact file stem, else by a casefold-only match flagged `case-mismatch`;
no PNG is `missing-texture` (detail `<icon>.png`).

**Tint.** `CustomIconTint` is `rrggbb` hex with an optional `#`, or `r,g,b` integers 0 to 255; two more hex digits
or a fourth integer are alpha and ignored. Anything else is unmatched with reason `invalid-tint` and the raw
value as detail. The value becomes `{"op": "tint-multiply", "color": "#rrggbb"}`; white (`#ffffff`) is the
identity and emits no op. **Rules** are `own-icon`, `inherited-icon` or `name`, with `-tinted` appended when a tint op
is present.

**What the game does** (decompiled from `Assembly-CSharp.dll` of build 26310339 with ILSpy; the pinned reference
assemblies come from `steam references`):

- `ItemClassesFromXml.parseItem` and `ItemModificationsFromXml` resolve `Extends` against the registered item
  class, copy the parent's properties minus the `param1` list (plus `CreativeMode`), then overlay the entry's own
  properties: transitive, per property, and the parent's merged properties are what is inherited.
  `BlocksFromXml.CreateProperties` does the same for blocks against another block.
- `ItemClass.GetIconName` is `CustomIcon` when non-empty, else `Name`; `Block.GetIconName` is `CustomIcon ?? name`.
  `ItemClassBlock.Init` copies the block's `CustomIcon` and `CustomIconTint`, so a block's item uses the block's icon.
- The tint is `StringParsers.ParseHexColor`: a comma anywhere means `ParseColor32` (3 or 4 integers, the fourth is
  alpha), otherwise `#`-optional hex read two digits at a time (shorter than 6 characters is transparent black).
  No tint is white.
- The localized name is `Localization.Get(name)` for items, modifiers and blocks: an exact, case-sensitive key
  lookup that returns the key when there is none. Auto-shaped blocks compose their name at runtime from the material
  and shape names, so those variants are not in the XML.
- `ItemClass.itemNames` is filled in the `ItemClass` constructor, so item modifiers are in it (they are in scope)
  and so are the block items.
- The depot's localization file is `Data/Config/Localization.csv`; there is no `Localization.txt`.

**Live run** (staging only; build 26310339, manifest 5022117301453546978, target `linux-3.4.0.b10`): 1,434 items,
107 item modifiers and 6,643 blocks in the config; 477 items, 1 modifier and 208 blocks have no English name (the
items are internals such as zombie hands and projectiles). 7,498 codes, 7,403 matched (98.73%): items 1,063 of
1,063, blocks 6,340 of 6,435. All 95 unmatched are `missing-texture` blocks. 2,937 of 5,261 PNGs used; 6,055
objects; one `case-mismatch`.

Known limits: coverage is against codes derived from game data (#811 skipped), not the live code list, and the connector
reports far fewer blocks than the XML lists (shape variants are generated at runtime), so the pack carries extra block
codes that are harmless. Item `Extends` can name a block in the game (one registry); this mapper resolves inside the
file, which is exact for this build: no entry has an unresolved parent or a cycle.

## Unreal games: the decoder container, IoStore ranges and Conan Exiles

### The decoder-container contract

A decoder turns a game's own formats into PNG and JSON. It lives in `maintenance/decoders/<name>/` (Dockerfile, tool
source, README) and is used through `icons/decoders/container.py`:

- **Image pin.** The image is built locally, never pushed (a registry needs credentials, and an image holding Oodle
  would redistribute it). Its tag is `takaro-icons-decoder-<name>:<context16>`, where the context hash is the sha256
  over the sorted `<path>\t<sha256>` lines of every source file (`bin/` and `obj/` excluded). A missing or stale image
  is built with `--label org.takaro.icons.decoder.context=<hash>`; the label must equal the hash afterwards. Every
  `FROM` is pinned `@sha256:`, packages are restored `--locked-mode`, and the container runs by image **Id**, which
  is what the build record stores.
- **Run.** `docker run --rm --network none --read-only --tmpfs /tmp --cap-drop ALL --security-opt no-new-privileges
  --user UID:GID`, every game mount `:ro`, only `/out` writable (under the cache work directory). A secret is passed as
  `-e NAME` (value only in the docker client's environment, never in argv, logs or errors). A non-zero exit is exit 6
  with the last 40 stderr lines, redacted. `TAKARO_MAINT_ICONS_DECODER` (else `TAKARO_MAINT_DOCKER`) replaces the
  docker CLI; a missing one is exit 2 before any download.
- **Request and manifest.** `{schemaVersion:1, decoder, settings, root:"/game", exports:[{id, kind, ...}]}` in,
  `/out/manifest.json` with one result per id (`ok` + `file` + `meta`, or `ok:false` + `errorKind`
  `unsupported-format|not-found|failed`) out. The host validates it with jsonschema, requires every id exactly once,
  `safe_relative` file names built from ids (never from game paths), PNGs opened as PNG and capped at 4096x4096, JSON
  capped at 256 MB. Any violation is exit 6.
- **Decode cache.** `<cache>/icons/decoded/<sha256(image id + canonical request + input tree hash)>/`, with a marker of
  per-file hashes; a hit is re-hashed and a mismatch decodes once more. Decoded pictures stay in the cache.

### The Unreal decoder

`maintenance/decoders/unreal` is a .NET 10 tool on CUE4Parse 1.2.2.202611 with a managed BCn decoder
(BCnEncoder.Net) and a small fixed-setting PNG writer (no native texture libraries, no ImageSharp). Export kinds:
`asset-registry {classes}`, `json {package}` (CUE4Parse's standard JSON), `texture {package, object}` (first mip with
data; PF_DXT1, PF_DXT5, PF_BC4, PF_BC5, PF_BC7, PF_B8G8R8A8 and PF_G8, anything else is `unsupported-format`) and
`list-files {prefix}` (bring-up aid). Settings: `engine` (an `EGame` value), `oodle` (the mounted library path) and
`aesKeyEnv`. Licences: CUE4Parse MIT, BCnEncoder.Net MIT.

Oodle is `liboo2corelinux64.so.9` (2.9.16) from the unofficial `WorkingRobot/OodleUE` mirror at a pinned commit,
recorded in `tools.lock.json` (`tools.oodle`: url, size, sha256, hashOrigin `self-recorded`), fetched through the
content-addressed cache (mismatch exit 5, unreachable exit 4) and mounted read-only. A licensed source is a follow-up.

### IoStore byte ranges

`icons/iostore.py` (stdlib) reads `.utoc` files (TOC versions 2 to 9, directory index required) and plans the `.ucas`
byte ranges that hold named packages: the `.uasset`/`.umap`, `.ubulk` and `.uptnl` chunks of each package, plus each
container's container header (Unreal reads it when it mounts), merged when less than 64 KiB apart. An encrypted
directory index is exit 2 ("not supported by the range planner yet (#747)"); an unknown version or a missing directory
index is exit 6. `icons/client_ranges.py` checks every `.utoc` and implied `.ucas` (including `_s<N>.ucas` partitions)
under `--client` before any network (exit 4 with `missing`), then builds a sparse mirror at
`<cache>/icons/clients/<pack game>/<app>/<client build | unknown>/<plan16>/`: `.utoc` files and `global.ucas` whole,
every other `.ucas` truncated to its source size, and each planned range copied (8 MiB chunks, hashed) with the marker
`.takaro/complete.json` replaced atomically. A hit re-hashes the ranges; a mismatch warns and re-reads once, a second
mismatch or a short source read is exit 5. `--client` may be a local install or a read-only mount of one:

    rclone mount ":sftp:/D:/Program Files (x86)/Steam/steamapps" "$MNT" --sftp-ssh "ssh gamer" \
      --read-only --vfs-cache-mode off --daemon
    takaro-maint icons build --game conan-exiles --client "$MNT/common/Conan Exiles" --out "$OUT"
    fusermount3 -u "$MNT"

`OUT/client-ranges.json` lists the plan (`file, offset, length, sha256`); it is provenance and is not published.

### Composite, flags and candidates (recipes)

A recipe takes 1 to 8 layers and optional `composite: "over"`. Each layer is loaded and its ops applied without
downscaling; the canvas is layer 0, a layer of another size is resized onto it (LANCZOS on premultiplied pixels),
layers are `alpha_composite`d in order and the result is downscaled once. One layer gives the same bytes as before.
`flags` (up to 4 `{flag, detail?}`) reach `coverage.json` as `flagsByName` and `flaggedCodes`. A recipe with
`candidateOf: "<primary code>"` (the primary must be another non-candidate recipe) goes into the index pointing at the
primary's object and is left out of `total`, `matched`, `byRule`, `byKind` and the contact sheet; coverage lists it
under `candidates`.

### Conan Exiles

`games/conan_exiles/icons.py` (mapper `conan-exiles`, pack `conan-exiles`). Item codes follow the connector's
`Catalogue::BuildItems`: a template id's code is its first row name in
`/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToTemplateID`, otherwise the id, and rows for which
`IsInternalName` holds (on the trimmed `Name` or the code) are not listed. Item rows come from every DataTable whose
`RowStructure` is `/Script/ConanSandbox.ItemTableRow` (base `/Game/Items/ItemTable`, then other plain tables, then
composites; the first table wins and disagreements are counted in the build record). Icons: `Icon` if set (rule `icon`),
else `IconLayers` bottom to top (rule `icon-layers`), else unmatched `no-icon-in-game`. DLC items (template ids not in
the base `ItemTable`) also get a candidate code, the other of name or id; a candidate colliding with any code is dropped.
Single-channel textures (PF_BC4, PF_G8) get a `mask-texture` flag.

### Conan Exiles bring-up findings

Observed with the real decoder image against server build 25792439 (depot 443032) and client build 25791535
(depot 440901), 2026-11-07.

- **G1 engine.** CUE4Parse has `GAME_ConanExilesEnhanced`; it reads both the Linux server containers and the Windows
  client containers, so `GAME_UE5_8` was not needed. Recorded as `unreal.engine`.
- **G2 unversioned properties.** The DataTables decode without a `.usmap` mappings file.
- **G3 Oodle.** `liboo2corelinux64.so.9` 2.9.16 decompresses both sets of containers.
- **G4 tables.** The AssetRegistry (`ConanSandbox/AssetRegistry.bin`, inside `pakchunk0-LinuxServer.pak`) lists 22
  `ItemTableRow` tables: `/Game/Items/ItemTable` (a plain DataTable, 5,821 rows), the empty `Example_ItemTable`, 19 DLC
  tables under `/Game/DLC/` and `Special_ItemTable_Composite`, a `CompositeDataTable` whose `ParentTables` are
  `Special_ItemTable`, `_GiantKings`, `_Shemite` and `_Cimmerian` (its 2,094 rows are their union). 13 tables use
  `ItemNameToTemplateIDStruct` as name tables; `ItemNameToHarvestXPValue` shares that struct but maps names to harvest
  XP, so it is excluded explicitly. `StoreItems` ends in `ItemTableRow` too but is `StoreItemTableRow`, so the row
  struct is matched exactly. DLC is decided per template id: not a row of the base `ItemTable`. In the live data every
  DLC id is missing from both the base item table and the base name table, so DLC items get a numeric primary code
  and, when a DLC name table names them, that name as the candidate (3,433 listed DLC items, 1,449 candidates).
- **G5 field shapes.** `Icon` is `{AssetPathName, SubPathString}`; `IconLayers` is an array of the same soft-path
  objects (one or two layers, no tint or other per-layer field); `Name` is an FText with an inline `SourceString` (no
  string tables); the name-table int member is `ID_XX_6_7ED8A4BD4E9D07B8E7077B82BED9F5D7`, found as the struct's
  only integer. Layer 0 is the bottom: epic items list the item art and then `epic_icon_border`.
- **G6 server selectors.** The AssetRegistry, `global.*`, `pakchunk0`, `pakchunk1050`, `pakchunk1100` and
  `pakchunk5000` to `pakchunk5150` hold every table: about 1.3 GB instead of 2.7 GB.
- **G7 client containers.** All 5,350 distinct icon textures live in `pakchunk1100`, `pakchunk1110` and `pakchunk5000`
  to `pakchunk5150` (TOC version 8, compressed, indexed, not encrypted, one partition each); `global.utoc` is read
  whole. No client `.pak` is needed. The range plan is 944 ranges, 126.0 MB.
- **G8 package naming.** Every texture is `/Game/...`, which maps to `ConanSandbox/Content/...`; there are no plugin
  mount points. A package needs its `.uasset` chunk and `.ubulk`/`.uptnl` chunks when present, plus each container's
  container header chunk (keyed by the container id), which CUE4Parse reads when it mounts. All textures were found, so
  the DLC depot 577840 is not needed.

### Known limitations

- Whether the live connector reports DLC items by name or by id is unsettled (#811 skipped); both are published.
- `PF_BC4` mask textures are published as grayscale art and flagged.
- Layer tints are not supported (no tint field was observed in the data; an item with one would be unmatched).
- Encrypted containers (VEIN) need the planner to read encrypted directory indexes (#747).

## Unity games: the Unity decoder and Valheim

### The Unity decoder

`maintenance/decoders/unity/` (`Dockerfile`, `requirements.in`, `requirements.lock`, `export.py`) follows the
decoder-container contract above unchanged. The context hash now also leaves out `__pycache__`.

- **Pins.** `FROM python:3.12-slim-bookworm@sha256:...` (digest resolved 2026-11-07). `requirements.in` is exactly
  `UnityPy==1.25.2`; `requirements.lock` is `uv pip compile requirements.in --generate-hashes --python-version 3.12
  --python-platform x86_64-manylinux_2_28 --only-binary :all:` and the image installs it with `pip install
  --no-deps --require-hashes --only-binary=:all:`, so no sdist and no build step ever runs. UnityPy 1.25.3 and later
  depend on `tpk-ar`, which PyPI ships only as an sdist, so 1.25.2 (2026-07-09) is the newest release that installs
  from wheels alone; it reads the Unity 6000.0.75f1 bundles of the current Valheim build (see the findings).
  `python -m compileall` runs at build time and the container sets `PYTHONDONTWRITEBYTECODE=1`, so nothing is written
  under `--read-only`. The user is `10001:10001`. The `ttgen` extra (a type-tree generator) is not installed: it
  would be a new unpinned tool and Valheim's bundles carry their type trees.
- **Licences** (from each wheel's metadata): UnityPy, texture2ddecoder, etcpak, astc-encoder-py, fmod_toolkit,
  pyfmodex and brotli are MIT; attrs MIT; Pillow MIT-CMU (HPND); lz4 BSD; fsspec BSD-3-Clause; archspec Apache-2.0 or
  MIT. Nothing forbids local use. `fmod_toolkit` and `pyfmodex` are only imported (the FMOD library is not
  downloaded and audio is never decoded), and the container has no network.
- **Request.** `{"schemaVersion": 1, "decoder": "unity", "settings": {"cabIndex": {<cab>: <bundle path>}} | {},
  "root": "/game", "exports": [...]}`. Every `files` entry must stay inside `/game` (absolute, `..` and links out are
  `failed`). An object reference is `{"cab": <lower-cased serialized-file name>, "pathId": <int>}` or `null`
  (`fileID 0` is the object's own file, `fileID n` is `externals[n-1]`, `fileID 0` with `pathID 0` is `null`).
- **Export kinds** (generic Unity facts, no game logic; output is sorted by `(bundle, pathId)`):
  `bundle-index {files}` lists each bundle's Unity version and serialized files; `monobehaviours {files, script
  {class, namespace, assembly}, fields}` dumps the listed dotted field paths of every MonoBehaviour whose script
  matches (a missing path is `failed`, naming the path); `sprite {ref}` gives name, `rect`, `textureRect`,
  `textureRectOffset`, packing, texture, alpha texture and atlas references, reading the render data from the
  SpriteAtlas entry when the sprite has one, like UnityPy's `SpriteHelper`; `texture {ref}` is a PNG of mip 0 in image
  orientation (a format UnityPy cannot decode is `unsupported-format`, a picture over 4096 px is `failed`);
  `sprite-image {ref}` is UnityPy's own cut, the reference the `atlas-cut` op is checked against.
- **Memory.** Bundles are walked one at a time in sorted order, the environment dropped and collected before the next;
  scans take two passes (MonoScripts, then the matching MonoBehaviours) because the scripts live in a shared bundle.
  Reference kinds keep at most four bundles open. No worker pool. Progress goes to stderr only; the manifest holds no
  timings or memory figures, so the build record stays deterministic.
- **Not covered by CI.** The real image is built and run by the bring-up and the live run, not in CI; CI proves the
  adapter, the session and the mapper against `fake_icons_decoder.py` and synthetic fixtures.

### Valheim

`games/valheim/icons.py` (mapper `valheim`, pack `valheim`). The pack is built from the anonymous server depot
(app 896660, depot 896661) alone: the item data and the art are in the roughly 800 asset bundles under
`valheim_server_Data/StreamingAssets/SoftRef/Bundles/`, selected by one regex that serves both the data and the
pixels roles. No game client is used.

- **Codes** follow the connector's `ListItemsAsync`: the `prefab.name` of every `ZNetScene.m_prefabs` entry whose root
  GameObject carries an `ItemDrop`, the first of a repeated name winning. A prefab reference is matched to an
  `ItemDrop` by the GameObject reference (the root component, as `GetComponent` does), never by name; `ItemDrop`
  copies in other bundles that no `m_prefabs` entry points at are ignored.
- **Icon** is `m_itemData.m_shared.m_icons[0]`. An empty list or a `null` first entry is `no-icon-in-game` (rule
  `none`); more than one entry is rule `first-of-variants`, otherwise `icon`. The sprite and its atlas texture are
  described by the decoder; each atlas PNG is copied once into the build's textures root and every recipe is one
  `atlas-cut` layer (with `canvas` and `offset` only for a sprite whose `textureRect` differs from its `rect`, or has
  an offset). A tight-packed sprite, one with a separate alpha texture, or an undecodable atlas format is unmatched
  (`unsupported-sprite`, `unsupported-texture`); a reference into no bundle is `unresolved-reference`.
- **Reference check on every build.** For every sprite with a packing rotation, the first 16 codes by sorted code and
  every trimmed sprite (capped at 64 sprites) the op's plain cut (no canvas) is compared pixel for pixel with
  UnityPy's own `sprite-image`. A difference adds the recipe flag `cut-differs-from-reference` and a note;
  `details.referenceCheck` is `{sampled, equal, rotated}`. The canvas and offset placement is not covered by it
  (UnityPy ignores the offset); it is checked on the contact sheet.
- **Exit codes.** No bundles in the depot is 4; no `ZNetScene`, `ZNetScene`s with different `m_prefabs`, one
  serialized file in two bundles, a decoder failure or an `atlas-cut` outside its atlas or canvas is 6; a missing
  docker CLI is 2, before any download; `--client` is 2.

### Valheim bring-up findings

Observed with the real decoder image (UnityPy 1.25.2) against server build 26266951 (depot 896661, manifest
6965848522742639928), 2026-11-07.

- **Bundles.** 798 files under `SoftRef/Bundles/`, all Unity 6000.0.75f1, 801 serialized files, none in two bundles.
- **Scripts.** Every MonoScript (454) is in one shared bundle, so the scan needs both passes. `ZNetScene` and
  `ItemDrop` are `assembly_valheim` with an empty namespace. Three bundles hold a `ZNetScene` MonoBehaviour; all three
  list the same 4,644 prefabs (after normalizing their external file ids) in one serialized file, none null, none
  outside the bundles. 1,710 `ItemDrop` components, one per GameObject.
- **Type trees.** Present for both behaviours; `m_itemData.m_shared.m_icons` resolves; no type-tree generator needed.
- **Codes.** 1,164 item codes, 3,480 prefabs without an `ItemDrop`, no repeated names, 17 prefabs with several icons.
  994 codes have an icon, 170 have none (creature attacks, boss weapons, `Charred_*`, hair), which is what the earlier
  spike found.
- **Sprites.** 911 distinct first-icon sprites, all in the one atlas serialized file, all through a SpriteAtlas
  render-data entry, all rectangle-packed, none with a separate alpha texture. Every packing rotation is `none`: the
  rotation table is verified synthetically only. 59 sprites have a non-zero `textureRectOffset` and 64 have a
  `textureRect` size that differs from `rect`; their rects and offsets are fractional.
- **Atlas.** One texture, 4096x4096, format BC7, decoded by UnityPy (exactly the 4096 px limit).
- **Hardening.** The container ran with `--network none --read-only` and no write outside `/tmp` and `/out`.

### Known limitations

- Coverage is measured against codes derived from game data, not against a live code list from a staging Valheim
  server (#811 skipped); a human review of the pack stays open.
- No sprite in the current build is rotated, so the rotation table is proven by synthetic tests and by UnityPy's own
  `SpriteHelper` source, not by live data.
- Tight-packed sprites and sprites with a separate alpha texture are not supported (none exist in this build).

## Terraria: XNB textures and game-code frame counts

The `terraria` pack (mapper `terraria`, `games/terraria/icons.py`) has three inputs and no server depot at all:

- **data**: the item codes are the 6,147 numeric codes of the connector's own static catalogue,
  `games/terraria/bridge/src/terraria/itemCatalog.ts` (`data.source: "connector"`). `listItems` returns that array
  unchanged, so the codes in the file are the codes the connector reports. The file is parsed as the JSON between
  `TERRARIA_ITEM_CATALOG: readonly TerrariaItemCatalogEntry[] = ` and `] as const;`; a missing array, a non-numeric
  code, an empty name or a repeated code is exit 6 ("connector catalogue format changed"). `codesFrom` names the file
  and its sha256 and says the codes were not measured against a live code list (#811 skipped).
- **code**: `Terraria.exe` from the Windows client, read by the `clr` container.
- **pixels**: `Content/Images/Item_<N>.xnb` from the same client (Steam app 105600, depot 105601), decoded by the
  `xnb` container. The TShock server zip has no `Content` folder and there is no anonymous server depot for it.

### The client mirror

Both containers read one read-only mirror of the selected client files (`Terraria.exe` and every file matching
`regex:^Content/Images/Item_[0-9]+\.xnb$`), never `--client` itself. `icons build` copies them once to
`<cache>/icons/client-files/<pack>/<app>/<buildid|unknown>/<key16>/`, where `key16` is the first 16 characters of the
sha256 over the sorted `path<TAB>sha256` lines, and writes `.takaro/complete.json` last through an atomic rename. A
second build hashes the source again, finds the directory, re-hashes the mirror, and copies nothing; a mirror that no
longer matches is warned about, removed and copied again once (a second mismatch is exit 5). Decoys next to the
selected files (`Item_7.png`, `Projectile_1.xnb`, `sub/Item_3.xnb`, `Item_x.xnb`) are not selected and not mirrored.

The gamer machine has no Docker, so the maintainer copies only what a build needs. The mirror is about 29 MB and stays
outside the repository:

```bash
M=~/.cache/takaro-maint/icons/clients/terraria/105600; mkdir -p "$M"
# on gamer (PowerShell, via -EncodedCommand): list Content\Images\Item_*.xnb plus Terraria.exe into a file, then
#   tar -cf - -C "D:\Program Files (x86)\Steam\steamapps\common\Terraria" -T <list>
# and unpack it here with tar -xf - -C "$M". The appmanifest goes next to it:
#   tar -cf - -C "D:\Program Files (x86)\Steam\steamapps" appmanifest_105600.acf | tar -xf - -C "$M"
```

Check 6,134 `Item_*.xnb` files totalling 2,546,532 bytes and that `sha256sum Terraria.exe` equals `Get-FileHash` on gamer.
`--client "$M"` is then the directory that holds `Terraria.exe`, `Content/` and the appmanifest.

### The XNB decoder

`maintenance/decoders/xnb/` (`Dockerfile`, `package.json`, `package-lock.json`, `export.mjs`, `png.mjs`).

- **Pins.** `FROM node:22.23.2-bookworm-slim@sha256:48e4b67d...`, the digest the catalog already pins for the Terraria
  bridge runtime. `package.json` has one dependency, `"xnb": "1.3.0-a"` (exact; npm `latest`, published 2024-03-26, no
  runtime dependencies), and `package-lock.json` is lockfile version 3 generated once with `npm install
  --package-lock-only --ignore-scripts` inside the pinned image. The image installs with `npm ci --omit=dev
  --ignore-scripts --no-audit --no-fund`, which enforces the lock's `sha512` integrity value
  (`sha512-N6oz2xqk...OSU5og==`). `1.3.0-a` imports and runs under Node 22, so the plain `1.3.0` is not needed.
- **Licence.** `xnb` is LGPL-3.0-or-later, used unmodified and installed only into a locally built image that is
  never pushed. It bundles png.js and LZ4-js (MIT). Nothing forbids local use.
- **Request.** `{"schemaVersion": 1, "decoder": "xnb", "settings": {}, "root": "/game", "exports": [{"id": "it5644",
  "kind": "texture", "file": "Content/Images/Item_5644.xnb"}]}`. A `file` that is absolute, has a `..` part, resolves
  outside `/game` or is not a regular file (`lstat`) is `failed`. The bytes are passed to the library as an exact
  `ArrayBuffer` slice, never as a pooled Node `Buffer` (its backing store is shared). Content other than `Texture2D` or a
  surface format other than `Color` is `unsupported-format`; an edge above 4096 is `failed`.
- **Output.** `png/<id>.png`, RGBA8, written by the container's own encoder (colour type 6, one IDAT from
  `zlib.deflateSync(level 9)`, filter 0, no ancillary chunks), so the bytes only depend on the pixels and the pinned
  Node. Pixels are written as decoded; alpha is the recipe's job. `meta` is `width`, `height`, `format`, `xnbVersion`,
  `platform` and `compressed`. Files are read one at a time and the library's progress chatter goes to stderr.

### The CLR decoder

`maintenance/decoders/clr/` (`Dockerfile`, `TakaroClrExport.csproj`, `nuget.config`, `packages.lock.json`,
`Program.cs`). It exists because "read the frame counts from the game code" needs an assembly reader, and
`Terraria.exe` is untrusted game data, which spec #809 says only runs in pinned, network-less containers.

- **Pins.** The same two .NET 10 digests as the Unreal image, `dotnet restore --locked-mode`, and one package,
  `ICSharpCode.Decompiler` 11.2.0.10050 (MIT, netstandard2.0, the current stable release when this was written,
  2026-11-07). The assembly is only read, never loaded or executed.
- **Export kinds** (generic, no game logic): `assembly-info {file}` gives `name`, `version` and `mvid`; `callers {file,
  member: "Ns.Type::Method"}` walks every method body's IL and lists `{type, method, count}` for each `call`, `callvirt`,
  `newobj` and `ldftn` that targets the member; `decompile-type {file, type}` gives `{type, source}` (C#) for `Ns.Type`
  or `Ns.Outer+Inner`. An unknown type or member is `not-found`. Decompiled text lives only in the decode cache.

### Mapping rules

1. Item `N` is drawn from `Content/Images/Item_N.xnb`, rule `item`.
2. **Frame counts come from the game code.** The `callers` of `Terraria.Main::RegisterItemAnimation` and
   `Terraria.ID.ItemID+Sets` are decompiled and read with bounded regular expressions:
   - `RegisterItemAnimation(<id>, new DrawAnimationVertical(<ticks>, <frames>[, <pingpong>]))` gives that item
     `frames` frames (rule `animation-frame`);
   - the loop `if (ItemID.Sets.IsFood[j]) { RegisterItemAnimation(j, new DrawAnimationVertical(int.MaxValue, 3)); }`
     gives every id of `IsFood = Factory.CreateBoolSet(...)` 3 frames (rule `food-frame`);
   - every other call of `RegisterItemAnimation` is `unparsed`, and an id with two different counts is a
     `conflict`. Either is exit 6, so a new game build that registers animations differently stops the build instead
     of mis-cropping items.
3. **The Scrying Orb (5644).** The game registers it as `new DrawAnimationScryingOrb { TicksPerFrame = 7, FrameCount = 9 }`,
   a state machine that picks its frame, whose `GetFrame` is `texture.Frame(1, FrameCount, 0, frameY, 0, -2)`: a
   vertical strip of 9 frames less two rows, and `frameY` 0 for its centred state. The mapper therefore has one
   documented override, `FRAME_OVERRIDES = {"5644": 9}` (rule `frame-override`), applied when the registration table
   has no entry for the id and only when the `DrawAnimationScryingOrb` registration with that `FrameCount` is found in
   the game code (otherwise exit 6); it is recorded in `details.animations.overrides`.
4. **The gap is 2.** `DrawAnimationVertical.GetFrame` is `texture.Frame(1, FrameCount, 0, frame)` followed by
   `Height -= 2`; the recipe is `{"op": "frame-crop", "frames": n, "frame": 0, "gap": 2}`. Nothing is trimmed, so the
   game's own framing is kept.
5. **Alpha is straight.** XNA content is often stored premultiplied, so the plan allowed for an `unpremultiply` op. The
   decoded textures refute it: 25,771 pixels are translucent and 18,036 of them (in 96 of the 125 textures that have
   translucent pixels) have a colour channel above their alpha, which a premultiplied pixel cannot have, and every
   fully transparent pixel is `(0, 0, 0, 0)`. The block therefore says `"xnb": {"alpha": "straight"}` and there is no
   `unpremultiply` op. A future build that breaks this shows up in `details.alpha`.
6. **Fake chests.** `Fake <X> Chest` items with no `Item_N.xnb` take the art of the one catalogue entry named
   `<X> Chest` when that entry has a texture (rule `fake-chest-fallback`, flag `fallback-art` naming the code); the
   fallback applies only when the file is missing. A fake chest with its own file keeps it.
7. Otherwise unmatched: `missing-texture` (no file), `unsupported-texture` (a surface format the container cannot
   decode) or `fallback-unresolved`.

`details` in the build record carries the evidence: the catalogue size and sha256, the assembly, the animation table
(`registered`, `food`, `overrides`, `unparsed`, `conflicts`, `callers`), `frames.nonDivisible`, `gapNotTransparent`
(frame-0 gap rows that are not fully transparent), `alpha`, `clientOnlyIds` and `fallbacks`.

### Exit codes of this route

Catalogue format changes, unparsed or conflicting registrations, a missing food loop or no callers, a decoder failure
and an out-of-bounds `frame-crop` are exit 6; a client missing `Terraria.exe` or with no file matching the `Item_*.xnb`
selector is exit 4; a missing `--client` or missing Docker is exit 2 (before any image is built or file copied).

### Terraria bring-up findings (client build 24893155, Terraria 1.4.5.8, 2026-11-07)

- The installed client of the maintainer's gamer machine is Steam's current public build 24893155 (depot 105601
  manifest 2046220459945595868), which is also what the only target, `tshock-v6.2.1` ("TShock 6.2.1 for Terraria
  1.4.5.8"), records in `clientBuilds`. `Terraria.exe` is 26,597,888 bytes, assembly version 1.4.5.8.
- 6,134 `Item_*.xnb` files, 2,546,532 bytes, ids 0 to 6195. All are XNB v5 for the Windows target, LZX-compressed,
  `Texture2D` with surface format `Color`; the largest edge is 234 px. The container decodes all of them with 0
  failures in about 13 s, peaking at about 81 MiB.
- `callers` finds one registering method, `Terraria.Main::InitializeItemAnimations` (17 calls): 15 constant
  `DrawAnimationVertical` registrations, the `IsFood` loop (86 ids) and the Scrying Orb: 102 items, `unparsed` 0,
  `conflicts` 0. Decompiling `Terraria.Main` takes about 25 s and peaks at about 270 MiB in the container.
- 6,085 of the 6,147 catalogue codes have a texture; 62 do not: 59 `Fake X Chest` items (all of which fall back), `3665`
  (`Fake Chest`), `3705` and `3706` (`Fake newchest1` and `2`).

### Known limitations

- Coverage is measured against codes derived from game data, not against a live code list from a staging server (#811
  skipped); a human review of the pack stays open.
- 49 client textures are not in the connector catalogue: `Item_0` (the "no item" placeholder) and `Item_6148` to
  `Item_6195`, which the connector's catalogue does not list. They are reported (`clientOnlyIds`), not added; whether
  the catalogue lags 1.4.5.8 is a connector question.
- `3665` ("Fake Chest") stays unmatched, as specified. It is the trapped plain Chest and could take the art of item
  48; that is a reviewer decision, not implemented.
- The fake-chest fallback is name matching, which spec #809 discourages; the ticket prescribes it.
- tModLoader items are out of scope.
- The real containers are not built in CI. They are proven by the bring-up and the live run; CI proves the adapters,
  sessions and mapper against `fake_icons_decoder.py` and synthetic fixtures.

## Minecraft: public downloads, data reports and the block-model renderer

The `minecraft` pack needs no Steam account and no game machine. Everything is a public Mojang download pinned by hash,
and the pack id is the Takaro game id, `minecraft`.

### Inputs and pins

- **Data:** the item registry in the data reports (`--reports`, `registries.json`) of the dedicated `server.jar`. The
  server jar and the version manifest that names it are the target's own pins (`inputs.game`, kind `mojang-version`).
- **Pixels:** `client.jar`, which holds the item definitions (`assets/minecraft/items/*.json`), the models and the
  textures. It is pinned in the `icons` block per target revision (`icons.pixels.downloads`, `source`, `path`, `sha1`,
  `size`), not in the target, so the target fingerprint does not move.
- `icons build --game minecraft --target fabric-26.3` builds the latest public release (26.3). Without `--target` the
  default target is `fabric-26.2`, which has no client pin, and the build is exit 2 before any download.
- **Adding a revision:** take `downloads.client.{sha1,size}` from the pinned version JSON of the new target, add
  `"<revision>": {"source": "mojang-data", "path": "/v1/objects/<sha1>/client.jar", "sha1", "size"}` and run
  `catalog validate`. The build fetches the manifest by its sha1 and requires its `id`, `downloads.server` and
  `downloads.client` to equal the pins (exit 5 otherwise).
- Downloads are cached by sha1 under `<cache>/icons/downloads/sha1/<sha1>/` with a completion marker; a hit is re-hashed,
  and a mismatch warns and downloads once more. The `server.jar` directory holds only that jar and its marker, and it is
  what the decoder container mounts read-only.

### The `minecraft-datagen` container

Reading the item registry means running Mojang's server jar, so the jar runs in a pinned decoder container
(`maintenance/decoders/minecraft-datagen/`) and nowhere else: `--network none`, a read-only root file system, `/tmp`
as the only scratch space, the jar mounted read-only at `/game`, `-Xmx2g`. The image is the Python base of the Unity
decoder plus the JDK copied from the Temurin 25 image of the Minecraft targets, both digests already pinned in the
repository, and nothing is downloaded at build time. It runs the bundler's data generator once per jar and returns the
requested reports as `/out/json/<id>.json` (request `kind: "datagen-report"`, `file: "server.jar"`, `report:
"registries.json"`). Mojang's jar is run locally for its registry facts and is never redistributed. The real image is
not built in CI; the tests use `fake_icons_decoder.py` (`datagen-report:<report>`).

### Mapping rules

Codes are the sorted item keys of the registry minus `minecraft:air`, which the connector's `listItems` skips; `kind` is
`block` when the key is also a block key. Everything is read from the jar by exact entry name, each entry capped at
8 MiB (over is exit 6); a resource location must match `^([a-z0-9_.-]+:)?[a-z0-9_./-]+$` without a `..` segment.

1. **The default model of `items/<name>.json`.** `model`: its model and `tints`. `condition`: `on_false`. `select`:
   when the property is `display_context` and a case's `when` holds `gui`, that case (flag `gui-case`), else
   `fallback` (none is unmatched `no-default-model`). `range_dispatch`: `fallback`, or with none the entry with the lowest
   `threshold` (flag `lowest-threshold`). `composite`: its first model. `special`: unmatched `entity-rendered`.
   `empty`: unmatched `no-icon-in-game`. Other top-level keys (`swap_animation_scale`, ...) are ignored and counted.
2. **The parent chain** is flattened (depth at most 32; a cycle is `model-cycle`, an absent model `missing-model`):
   `textures` and `display` per key with the child winning, `elements` from the nearest model that has them,
   `gui_light` from the child (default `side`). A chain rooted at `builtin/generated` is **flat**; `builtin/entity` is
   `entity-rendered`. A texture is `#key` (through the textures, at most 32 hops), a location, or an object whose
   `sprite` is the location. An unresolved one or an absent PNG is `missing-texture`. A face texture that is a bare
   key without `#` is read as a location, as the game reads it, and so is missing too.
3. **Animated textures.** `<texture>.png.mcmeta` with an `animation` object makes the texture a vertical strip of
   `W x W` frames (`frames = H // W`, 1 to 64, `H % W` must be 0) and the first listed `frames` entry (else 0) is used:
   `{"op": "frame-crop", "frames": n, "frame": i, "gap": 0}`. A declared `width` or `height` is
   unmatched `unsupported-animation`.
4. **Tints** (`tints[i]` colours tint index `i`; values are signed ARGB, `value & 0xFFFFFF`): `constant` uses `value`;
   `dye`, `potion`, `firework`, `map_color`, `team` and `custom_model_data` use `default`; `grass` reads
   `textures/colormap/grass.png` at `x = int((1 - t) * 255)`, `y = int((1 - d * t) * 255)` for the clamped temperature `t`
   and downfall `d`. Any other type is unmatched `unsupported-tint`. White emits no op; a non-opaque alpha is counted.
5. **Flat items** stack `layer0..layer7` (consecutive from 0): each is `{"image", "ops": [frame-crop?, tint-multiply?]}`
   (rule `flat`, or `flat-tinted`). A `display.gui` on a generated model is ignored and counted. The leather overlay
   (`layer1`, untinted) therefore comes out. Flat icons keep their native size (usually 16 px) because the builder
   never upscales.
6. **Box models** are written as block-model documents (`models/<ns>/<item>.json`, canonical JSON) and drawn by the
   renderer (rule `rendered` or `rendered-tinted`). `cullface` is dropped, because an inventory slot draws every quad;
   with default UVs this is what makes walls complete. Element keys `name`, `__comment` and `light_emission` are
   ignored; `shade_direction_override` becomes `shadeDirection`; any other element or face key, the `{origin, x, y, z}`
   rotation form and a `rescale` of 90 degrees or more are unmatched `unsupported-model`. A model that reaches beyond the
   canvas is shrunk and centred to fit (`fit_to_canvas`, flag `render-fitted`), because a slot lets it overflow and an icon
   cannot.

The build record's `mapper` block carries the evidence: `gameVersion`, `registry`, `definitions`, `models`, `tints`,
`animated`, `textures`, `renderFitted`, `ignoredKeys` and `itemDefinitionsWithoutCode`.

### The block-model renderer

`icons/block_model.py` draws a model document in pure Python and Pillow (no numpy). The document
(`schemaVersion` 1; every object closed) is `{"textures": {key: {"image", "ops"?}}, "elements": [...], "gui": {"rotation",
"translation", "scale"}?, "light": "side" | "front"?}`. An element is `{"from", "to", "rotation": {"origin", "axis",
"angle", "rescale"?}?, "shade"?, "shadeDirection"?, "faces": {<face>: {"texture", "uv"?, "rotation"?, "tint"?}}}`.
Limits: 1 to 512 elements, 1 to 64 textures, coordinates in [-80, 96], `uv` in [0, 16], an angle in [-180, 180], a face
rotation in {0, 90, 180, 270}, `rescale` only below 90 degrees. Units are 1/16 block.

1. **Face geometry.** For a box `from = (x1, y1, z1)`, `to = (x2, y2, z2)` each face has a corner `O` (the texture's top
   left seen from outside), an edge `U` (u increasing) and an edge `V` (v increasing); the outward normal is `V x U`:

   | Face | O | U | V |
   |---|---|---|---|
   | north | (x2, y2, z1) | (x1-x2, 0, 0) | (0, y1-y2, 0) |
   | south | (x1, y2, z2) | (x2-x1, 0, 0) | (0, y1-y2, 0) |
   | west | (x1, y2, z1) | (0, 0, z2-z1) | (0, y1-y2, 0) |
   | east | (x2, y2, z2) | (0, 0, z1-z2) | (0, y1-y2, 0) |
   | up | (x1, y2, z1) | (x2-x1, 0, 0) | (0, 0, z2-z1) |
   | down | (x1, y1, z2) | (x2-x1, 0, 0) | (0, 0, z1-z2) |

2. **Default `uv`:** down `[x1, 16-z2, x2, 16-z1]`, up `[x1, z1, x2, z2]`, north `[16-x2, 16-y2, 16-x1, 16-y1]`,
   south `[x1, 16-y2, x2, 16-y1]`, west `[z1, 16-y2, z2, 16-y1]`, east `[16-z2, 16-y2, 16-z1, 16-y1]`.
3. **Face rotation** (clockwise), face parameters `(s, t)` in [0, 1)^2 to `(s', t')`: 0 `(s, t)`; 90 `(t, 1-s)`;
   180 `(1-s, 1-t)`; 270 `(1-t, s)`. Then `u = u1 + s'(u2-u1)`, `v = v1 + t'(v2-v1)` and the texel is
   `(floor(u*W/16), floor(v*H/16))` clamped, where `W x H` is the texture after its ops.
4. **Element rotation** about `origin` and `axis` by `angle` degrees (right-handed): `p = o + k * R(p - o)`, with `k` 1
   on the axis and, on the other two axes, `1/cos(angle)` when `rescale` and 1 otherwise.
5. **GUI transform.** With `b = p/16`: `q = T + Rx(rx) Ry(ry) Rz(rz) (S * (b - 0.5))`, `T = clamp(translation, -80, 80)/16`,
   `S = clamp(scale, -4, 4)`; this is the game's item transform followed by its `translate(-0.5)`. The identity
   transform shows a block from the south; the inventory transform `[30, 225, 0]` at scale 0.625 shows the top, east on
   the left and north on the right.
6. **Screen:** the canvas is 128 px, `X = 64 + 128 q.x`, `Y = 64 - 128 q.y`, `depth = q.z` (larger is nearer).
7. **Culling:** a face is drawn only if `(V' x U').z > 0` for the transformed edges.
8. **Rasterization:** every pixel centre `(i + 0.5, j + 0.5)` in the face's bounds solves `O' + s U' + t V'` in 2D and is
   accepted for `0 <= s < 1` and `0 <= t < 1` (a face with `|det| < 1e-9` is skipped); a texel with alpha 0 is discarded.
9. **Colour per fragment:** a tint multiplies `c * t // 255` per channel; with `light: "side"` and the element's `shade`
   true, the colour is scaled by the factor of the face (or of `shadeDirection`): up 1.0, down 0.5, north and south 0.8,
   east and west 0.6, as `int(c * f + 0.5)`; `light: "front"` is unshaded. Alpha is unchanged.
10. **Compositing:** the fragments of a pixel are sorted by `(depth, element index, face order)` ascending (face order
    down, up, north, south, west, east, so a later coplanar element, like a grass overlay, lands on top) and combined with
    Porter-Duff "over" in straight alpha with floats, rounded half up.
11. **Fit:** `screen_bounds(model)` projects the element corners; `fit_to_canvas` scales the gui scale by the factor that
    leaves a one pixel margin and moves the bounds' centre to the canvas centre.

### Known limitations

- Items drawn by an entity renderer (`minecraft:special`: chests, banners, shulker boxes, heads, shields, ...) stay
  unmatched, as the game draws them from code, not from a model.
- A `composite` definition (beds) takes only its first model, as the mapping rule says, so a bed icon is its head part.
- Shading uses fixed per-direction constants; it approximates the game's GUI lighting (checked visually).
- Flat icons are 16 px; whether Takaro scales pixel art with nearest-neighbour is a question for Takaro, not the pack.
- Only revisions with a client pin build. Coverage is measured against codes derived from game data (#811 is skipped).

### Minecraft bring-up findings (release 26.3, 2026-11-07)

- `latest.release` of the version manifest was 26.3; the pinned manifest is served and names the pinned jars: `client.jar`
  sha1 `e877b6a07acd633fb3bb475002175cec036e7b87` (41,483,720 bytes), `server.jar` sha1
  `33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c` (62,294,556 bytes), Java 25.
- The data-generator container runs Java 25.0.4 (Temurin) on the Python base. `--reports` succeeds under the hardened run
  (no network, read-only root, the default `/tmp` tmpfs) in about 9 s. `registries.json` is 536,272 bytes with sha256
  `51f62d56f8bd0e9134ee88dd9226b64e9e56cf6a32377540d44291bf47ecdac6` and is byte-identical across every fresh run
  (two by hand and the live builds). It has 1,658 item entries (including `minecraft:air`) and 1,286 block entries.
- 1,658 item definitions, one per registry item, none missing. Top-level types: `model`, `select` (71), `special` (51),
  `composite` (17), `condition` (7) and `range_dispatch` (2); tint types: `constant`, `dye`, `firework`, `grass`,
  `potion`. Nothing beyond what the mapping covers.
- Things the plan's census did not show and the mapper handles: a model texture may be an object
  (`{"sprite": ..., "force_translucent": true}`, 17 glass items); `minecraft:heavy_core` names a face texture without
  `#`, which the game reads as a location (`minecraft:all`) and so lacks, so it is the one unmatched `missing-texture`.
- Under the default resolution only one flat layer is animated (`sculk_vein`); 24 box models use animated textures
  (one frame each).
- Coverage: 1,594 of 1,657 codes (96.2%): 761 flat (744 plain, 17 tinted), 833 rendered (825 plain, 8 tinted). The 63
  unmatched are 62 `entity-rendered` and 1 `missing-texture`. The ticket's 63 `special` items become 62 because the
  trident takes its `gui` case (a flat inventory icon); 26 `select` definitions take a `gui` case; `compass`,
  `recovery_compass` and `clock` take their lowest threshold. 4 models reach beyond the canvas and are fitted
  (`calibrated_sculk_sensor`, `lectern`, `spore_blossom`, `straw_bed`). The coverage is measured against codes derived
  from game data (#811 is skipped).
- The inventory transform in the order `Rx Ry Rz` needed no swap: the furnace front is on the right (north), the east
  face on the left, the side brightness order is top, north and south, then east and west. Walls show the post and the
  arm; leather armour shows its overlay; the potion, grass block, leaves and vines are tinted; glass is transparent;
  compass and clock show their default frame; sea lantern and magma block show one frame. The renders were checked on
  the contact sheet and enlarged review sheets, not against the wiki's inventory images.
- Timings on the maintainer machine: the data generator takes about 9 s; the render of 1,594 icons takes most of a build, which is 1 minute 40 seconds with the
  downloads and the decode cached and 2 minutes 20 seconds from an empty cache (about 104 MB of downloads); the build's own peak memory is about 190 MB and the generator is capped at
  `-Xmx2g`. A fresh build from an empty cache produces a byte-identical `pack/` and `build-record.json`.

## Keeping packs current

`takaro-maint scan` adds an Icon Pack rebuild line to the acceptance checklist of every new-build maintenance
issue of a game whose catalog record has an `icons` block. Games without the block are unchanged. The line names
three commands, and each takes a different id:

- `icons build --game <catalog game id> --target <new target> [--client <client install>] --out <out>`;
- `icons publish --env staging --pack <out>/pack/<pack id>`;
- `icons promote --game <pack id>`, which is local and needs production credentials.

The pack id is `icons.game`, which can differ from the catalog id. A new release usually needs a
`pixels.clientBuilds` entry for its revision first. The rebuild can be skipped when the new `build-record.json`
input hashes (`data.treeSha256` and the per-pack `pixels` sha256) match the previous build.

The line is a reminder, not a gate: the lifecycle does not track it, and like every line of the owned block it is
regenerated on a rescan.

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
   taken from the live index: `enabled: false` stays, a new pack is enabled, and a code removed by
   `takedown` comes back when the published pack contains it. The merged index is validated.
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

No takedown record exists yet, so every later build, publish and promote re-adds a taken-down code
that the game data still produces. Apply the takedown again each time. `takedown` prints a warning
saying so. `promote` never removes codes: when production holds codes that staging lacks, it warns
with the first ten of them and the count, and a staging takedown needs
`icons takedown --env production`.

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
| 2 | usage or configuration: a bad flag or recipe document, `--env` other than `staging` (`publish`) or other than `staging`/`production` (the others), a `_store/` or otherwise invalid game or key, a missing `STG_R2_*` / `PROD_R2_*` variable, a non-https endpoint, staging and production resolving to the same bucket, a non-empty `--out`, an unsupported op or multi-layer recipe, a game without an `icons` block, `--recipes` and `--game` both or neither, `--client` without `--game`, a missing `--client` for a client-sourced build, `--client` on a route whose pixels come from the server depot or a public download, a target revision without a client jar pin, an invalid block-model document, an unknown code or object, a malformed `--object`, `--code` and `--object` both or neither, `--enabled` other than `true`/`false` |
| 3 | a catalog build or `inputs` for a target that does not exist |
| 4 | a recipe image file is missing, or there is no index for the game in the environment being read; a catalog build whose `--client` is not a directory or lacks a listed pack, or whose pinned server manifest Steam will not serve; a data file a mapper requires that the depot did not serve (7d2d: the XML and localization files, named in the error); Valheim: no asset bundle under `SoftRef/Bundles/`; Minecraft: a Mojang download that is unreachable or 404, a model or texture file a model layer names that is missing |
| 5 | a filtered depot cache that still does not match its marker after one fresh download; Minecraft: a download that does not match its pin, or a version manifest that names another version, server jar or client jar than the catalog pins; the index read back after upload differs from what was sent; a copied object does not hash to its name; an object is still present after a delete |
| 6 | a malformed texture pack, a `trim` of a fully transparent image; 7d2d: an XML file with a DOCTYPE or ENTITY, malformed XML, or a localization file without a `Key` or `english` column; Valheim: no or conflicting `ZNetScene`, one serialized file in two bundles, a Unity decoder failure, an `atlas-cut` outside its atlas or canvas; Minecraft: a data-generator failure, a registry report of another shape, a jar entry over 8 MiB, a texture that is not a PNG |
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
