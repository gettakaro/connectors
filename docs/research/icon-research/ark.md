# ARK: Survival Evolved icon route (research)

Refs gettakaro/connectors#813 (epic: icon packs, spec #809). Research only: this ships a findings
doc, the pinned network-less tooling that produced the evidence and a small sample mapping. It builds
no pack, publishes nothing and does not depend on the `icons` package of #742.

## Decision

**Go, as "Go B with Go C": the item to icon mapping is read from the server depot, the pixels come from
the client install, and the decoder is a small in-repo Texture2D reader (UModel and CUE4Parse are not
used).**

| Question | Result | Outcome |
| --- | --- | --- |
| Q1 Item code format | confirmed from connector source and fixtures, **not live-confirmed** (#811 skipped) | done |
| Q2 Can the item class to icon texture mapping be resolved from data? | yes, from the server depot: 621 `MasterItemList` codes, 571 (92.0%) resolve to an `ItemIcon` texture that exists in the depot, 50 (8.0%) are material-only icons | go |
| Q3 Do the server depot textures hold pixels? | **no**: every Texture2D is cooked with its pixel data stripped (the file holds `ImportedSize` only) | server-only (Go A) is out |
| Q4 Do the client textures hold pixels, and are they decodable? | yes: 30 of 30 sample icons decode to correct, recognisable 256x256 images | go |
| Q5 Does UModel (UEViewer) read these packages? | **no**, for every game tag tried (see the failure table) | UModel is out |
| Q6 Licence of UEViewer | MIT: building it into a local, non-distributed image is allowed | not a blocker (moot, it does not work) |
| Q7 Does a minimal own reader work (Go C)? | yes: `ark_uasset.py textures`, about 150 lines for the Texture2D part, stdlib only | go |

The route therefore is: mapping from server depot 376031 (a few MB of `.uasset` headers and blueprints,
no download of the 22.8 GB depot), pixels from the ARK SE client (app 346110) with only the icon
texture packages copied, decoded by an own reader that writes a DDS file which Pillow converts to PNG.
This is the same client-sourced shape as the Conan pilot (#743).

Confidence: mapping **high** for the base game data (data-derived, 33 sample rows checked, the code
format matches the connector fixtures), pixels **high** for DXT5 icons (30 of 30 by eye), **medium** for
coverage of all 571 textures (only the sample was copied, other pixel formats may exist), DLC **low**
(see Findings, DLC).

## What was tested

| | |
| --- | --- |
| Date | 2026-11-06 |
| Server target | `catalog/ark/targets/linux-21241282.json`: Steam app 376030, depot 376031, manifest `6366771435093287465` (22,831,022,295 bytes, 145,670 files, manifest date 2025-12-18), build **21241282**, anonymous |
| Client | ARK: Survival Evolved (app 346110) on `gamer`, Steam build **21291776** (`appmanifest_346110.acf`). Client and server builds differ by app and by 50,494 build numbers, so this is a note, not a failure: the 30 sample textures have identical `ImportedSize` (256x256) on both sides |
| Package format | UE4 file version **405**, licensee version **10**, loose `.uasset` only (no `.uexp`, `.ubulk`, `.pak`) |
| UModel | gildor2/UEViewer commit `a0bfb468d42be831b126632fd8a0ae6b3614f981` (2024-02-23), tarball sha256 `3980aca68ffa11ac787910bed87be355e94613172bdf53cbde37863777899edd`, built from source with its `build.sh` |
| UModel image | `ark-spike-umodel:0`, image id `sha256:c1fb3fdb0231aea5f4466bdd7d32c894127d37a40e6072991dde10ba3e7a1db9` (local build, never pushed), base `debian:bookworm@sha256:268fb24c39bca92d326dd83ea3abebe4a3f7751c804ffef23bb6d857dd343ff8`, apt from `snapshot.debian.org` at `20261104T212339Z` |
| Python image | `ark-spike-py:0`, image id `sha256:6d816b0ff2f6aa7ff0f018371609f5ac59c916616c8f52ae9104db54e9745e92`, base `python:3.12-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df`, Pillow 11.3.0 pinned by wheel hash |
| Reader | `ark_uasset.py` in this folder, stdlib only, run with `python -I` in the plain digest-pinned python image |
| DepotDownloader | the repo's pinned wrapper (`takaro_maint.steam.depotdownloader`, tool 3.4.0), anonymous |
| Bytes fetched | 4,854 server files, 19.9 MB of `.uasset` (61 MB on disk), 30 client files, 2.7 MB. The depot was never downloaded in full |

Timings (wall clock): manifest listing 20 s; pass 1 download (4,853 files) 19 s; UModel image build
about 6 minutes in total (two failed attempts: wrong snapshot date, missing `libpng-dev`); the client
copy from `gamer` 2 s; mapping of 621 codes 1.4 s; decoding the 30 textures a few seconds.

## Method

1. **Listing.** `manifest_only(376030, "376031", "public", manifest="6366771435093287465", os_="linux",
   arch="64", ...)` from a throwaway `uv run --frozen --project maintenance python` script (not saved in
   the repo), then the listing was filtered by path.
2. **Pass 1 download.** `download(..., filelist=[...], validate=False)` of every non-mod
   `PrimalGameData*.uasset`, every `PrimalItem*.uasset` (2,647 files, 14.5 MB) and every `.uasset` below an
   `Icons/` directory (2,246 files, 1.9 MB): 4,853 files. Pass 2 fetched the one sample texture that is
   not below an `Icons/` directory (`RexSaddle_Icon`).
3. **Client copy.** Only the 30 sample texture packages, streamed with the Windows `tar.exe` over
   `ssh gamer` into the cache (`scp` could not handle the spaces in `Program Files (x86)`). Steam and the
   install were not touched.
4. **Decode and parse** only through `run.sh`: `docker run --rm --network none --read-only --tmpfs /tmp
   --cap-drop ALL --security-opt no-new-privileges --user "$(id -u):$(id -g)"`, game files mounted
   `:ro` at `/in`, only `/out` writable, `--in` or `--out` inside the git worktree refused.
5. **Cache layout** (outside the repo): `~/.cache/takaro-maint/icons-spike/ark/{server,client,listing,
   umodel-out,map-out,tex-out,logs}`.

Reproduce:

```sh
docs/research/icon-research/ark/run.sh build
docs/research/icon-research/ark/run.sh reader --in "$CACHE" --out "$CACHE/map-out" -- \
  map --root /in/server --listing /in/listing/entries.json --out /out/mapping.json \
  ShooterGame/Content/PrimalEarth/CoreBlueprints/BASE_PrimalGameData_BP.uasset
docs/research/icon-research/ark/run.sh reader --in "$CACHE/client" --out "$CACHE/tex-out/client" -- \
  textures --root /in --out /out /Game/PrimalEarth/CoreBlueprints/Resources/Icons/Wood_Icon
docs/research/icon-research/ark/run.sh sheet --in "$CACHE/tex-out/client" --out "$CACHE/tex-out/client/sheet"
```

`$CACHE` is `~/.cache/takaro-maint/icons-spike/ark`. `listing/entries.json` is the `ManifestInfo.entries`
mapping of the manifest listing, saved as JSON.

## Findings

### 1. Item code format (confirmed from source, not live)

Live confirmation is not possible this run because #811 (live code export) is skipped.

- `listItems` returns `{code, name}` rows (`games/ark/mod/src/takaro_adapter.hpp`, `catalog()`, about
  line 548). `code` is `UClass::GetPathName()` of each class in the live
  `PrimalGameData::MasterItemList` (`games/ark/mod/src/catalog_bindings.hpp`, `page()`: array at
  `PrimalGameData+0x910`, native `get_class_path` at `0x1293230`). Null slots and the base `PrimalItem`
  class are skipped. `name` is the last path segment (the class name), not a localised label.
- Inventory rows use the same `GetPathName` code (`games/ark/mod/src/inventory_bindings.hpp`, line 24).
- `giveItem` accepts the same string, optionally wrapped as `Blueprint'...'`. `gate::valid_item_code`
  (`games/ark/mod/src/gate_state.hpp`, line 51) requires a `/Game/` prefix, an `_C` suffix and only
  `[A-Za-z0-9/_.]`.
- Shape: `/Game/<dir>/<Asset>.<Asset>_C`. On disk `/Game/X/Y` is `ShooterGame/Content/X/Y.uasset`, so a
  code maps 1:1 to a blueprint package plus `<Asset>_C`, with no name guessing.
- The pack game identifier is `ark` (`games/ark/mod/src/game_profile.hpp:18`, `kGameSlug`).
- **Cross-check against data:** the two codes in the connector test fixture
  (`games/ark/mod/tests/takaro_bridge_test.cpp:203-204`, `PrimalItemConsumable_CookedMeat` and
  `PrimalItemResource_Stone`) exist, with exactly those paths, among the 621 codes derived from the
  game data. All 621 codes pass the `valid_item_code` character rules; the longest is 168 characters
  (the longest sample code is 150), under #742's 256-character limit.
- Caveat: `MasterItemList` is read from the **live** `PrimalGameData`, which can depend on the map and
  its DLC data. The code set a server reports can differ per map. Mod items (`/Game/Mods/...`) are out of
  scope.

### 2. Server depot: what is there

| | Count | Size |
| --- | --- | --- |
| files in depot 376031 | 135,596 with content | 22.8 GB |
| `.uasset` | 131,660 | 6.6 GB |
| `.umap` | 2,276 | 12.8 GB |
| `.uexp`, `.ubulk`, `.pak` | 0 | 0 |
| `PrimalItem*.uasset` (non-mod) | 2,647 | 14.5 MB |
| `.uasset` below an `Icons/` directory (non-mod) | 2,246 | 1.9 MB (largest 14.7 KB) |
| `PrimalGameData*` packages | 4 | see below |

The game-data packages are `PrimalGameData_BP` (2,986 bytes, a thin blueprint), `BASE_PrimalGameData_BP`
(244,826 bytes, holds `MasterItemList`), `COREMEDIA_PrimalGameData_BP` (3.4 MB) and
`CORE_PrimalGameData` (2,702 bytes). The inheritance chain is `PrimalGameData_BP` to
`BASE_PrimalGameData_BP` to `COREMEDIA_PrimalGameData_BP` to the native `CORE_PrimalGameData`. Only
`BASE_PrimalGameData_BP` carries `MasterItemList`. There is **no** per-DLC game-data package in the depot.

### 3. Server textures have no pixels (the critical question)

`Wood_Icon.uasset` is 597 bytes on the server and 88,353 bytes on the client. The server export holds the
property `ImportedSize` (256x256) and then strip flags `3,0,3,0` with a pixel format of `None`: the
cooker kept the object and dropped the platform data. All 30 sample textures behave this way (18,256
bytes on the server against 2,650,936 on the client), and no file below an `Icons/` directory is larger
than 14.7 KB, so the whole icon set is stripped. **Go A (server-only) is therefore not possible.**
`ImportedSize` does match the client dimensions for all 30 samples.

### 4. Mapping chain, read from data (`ark_uasset.py map`)

`BASE_PrimalGameData_BP` CDO, property `MasterItemList` (array of 623 class references, 2 null) gives 621
distinct codes; each item class is a `BlueprintGeneratedClass` export whose CDO (`Default__<Asset>_C`)
holds `ItemIcon` (an `ObjectProperty` to a `Texture2D` import, whose outer package is the texture
package). If `ItemIcon` is absent the reader follows the class's `SuperIndex` to the parent blueprint
package (any downloaded `PrimalItem*` package) until it is found or a native `/Script/` class is
reached.

| Outcome (base game data) | Codes | Share |
| --- | --- | --- |
| resolved to an icon texture that exists in the depot listing | 571 | 92.0% |
| texture package missing from the depot | 0 | 0% |
| material-only icon (`ItemIconMaterialParent`, no `ItemIcon`) | 50 | 8.0% |
| **total** | **621** | |

- 44 of the 571 resolved icons are inherited from a parent blueprint (for example `WaterskinRefill`,
  `Seed_*_SpeedHack`, `RecipeNote_*`).
- The 50 material-only items are 26 dyes (`DyeIconColorized`, one generic parent), 18 dino eggs
  (`HUD_DinoEgg_MIC_Inst_*` material instances) and 6 skins. They need a tint or composite op, or an
  "approximate" note, in a pack.
- 111 resolved items also set `ItemIconMaterialParent`, and 89 set `bUseItemColors` (colourised armour,
  saddles and similar). The texture is still the item's `ItemIcon`; a pack can use it as is or apply a
  tint later.
- 28 of the 621 codes live under `/Game/PrimalEarth/Test/`; at least some use development placeholders
  (for example `PrimalItemHelmet2` points at an `OldAssets` texture called `Falloff`). A pack should
  decide whether to publish icons for such items.
- By top-level directory: PrimalEarth 497, Abyss 122, Genesis 2.

This is a coverage estimate against **game-data-derived codes**, not the live code list.

**DLC.** The depot holds item blueprints for ScorchedEarth (82), Aberration (71), Extinction (86), Genesis
(92), Genesis2 (121), Fjordur (16), LostIsland (16) and Valguero (2) that are not in the base
`MasterItemList` (Abyss has 520 `PrimalItem*` blueprints, 122 of them in the list). No DLC game-data package exists as a loose file,
and `COREMEDIA_PrimalGameData_BP` imports a `BPMergeModGameData` class and has `ExtraResources` and
`GenesisSeasonPassItems`, so DLC items are most likely merged into the live list at runtime. That
mechanism was **not** investigated in this spike. Until it is, DLC coverage is unknown, and the live list
of a DLC map may contain items this mapping does not reach.

### 5. UModel fails on every route tried

UModel built and runs; it has an ARK tag (`ark`, "Ark: Survival Evolved", `GAME_Ark`) with a special
case for ARK's package summary. It still cannot read these packages. Test package:
`PrimalEarth/CoreBlueprints/Resources/Icons/Wood_Icon.uasset` from the client.

| Input | `-game=` | Result |
| --- | --- | --- |
| whole `.uasset` | `ark` | exit 1: `wrong name index -1641380927` in `FPropertyTag<<` at `pos=8` |
| whole `.uasset` | `ue4.4`, `ue4.5`, `ue4.6`, `ue4.10`, `ue4.14`, `ue4.20`, `ue4.27` | exit 139 (segmentation fault) |
| split into header `.uasset` plus `.uexp` | `ark` | exit 1: `assertion failed: (BufferPos >> 32) == 0` |
| split into header `.uasset` plus `.uexp` | `ue4.x` tags above | exit 139 (segmentation fault) |

The first failure has a clear cause: `-1641380927` is `0x9E2A83C1`, the package tag, so UModel read the
first property of the export from file offset 0. It treats the export offset as relative to a `.uexp`
(it subtracts the header size, 537 for this file) although ARK's packages are single files with the
export data inline. After splitting each package into a header file and a `.uexp` by hand, UModel gets
past that and then fails an assertion in the buffered reader, most likely while reading the mip bulk
data (64-bit offsets, or the ARK layout described below). Both are structural: a fix means patching
UModel, which is out of scope. UModel's exit codes and messages are in the cache logs
(`logs/umodel-export-client.log`, `logs/umodel-matrix.log`).

**CUE4Parse was not run.** The plan's third fallback was not needed once the own reader worked. The
upstream failure that #747 saw (`Invalid bool value`) is consistent with one difference this spike
found, but that is a hypothesis and was not tested: in this layout a property tag has **no
`HasPropertyGuid` byte** after the type-specific extras. A reader that expects that byte (as later UE4
builds have it) shifts every following value by one byte, which turns a bool or size field into garbage.

### 6. The Go C reader (what the byte layout is)

`ark_uasset.py` reads these packages (all offsets verified on real files):

- **Summary:** tag `0x9E2A83C1`, legacy version `-3`, UE3 version 864, UE4 version **405**, licensee version
  **10**, custom-version count 0, then folder name, package flags and the name, export and import
  tables, which sit right after the package flags. The reader does not parse anything after the
  `DependsOffset` field.
- **Name table:** `FString` entries with **no** trailing hash words.
- **Imports:** 28 bytes each. **Exports:** 76 bytes each (`SerialSize` and `SerialOffset` are 64-bit),
  with `SerialOffset` absolute in the single `.uasset` file.
- **Property tag:** `FName` name (index and number), `FName` type, `int32` size, `int32` array index, then
  the type extras (struct name, bool value, enum name, array inner type), then the value. There is
  no property-guid byte. Every property is skipped by its size except the few that are read, so the
  reader is tolerant of properties it does not understand.
- **Texture2D, after the tagged properties:** `int32` guid flag; four strip-flag bytes (UTexture,
  UTexture2D); `int32` `bCooked`; `FName` pixel format (`None` when stripped); `int64` skip offset;
  `int32` sizeX, sizeY, slots; `FString` pixel format; `int32` first mip; `int32` mip count; then per mip
  `int32` cooked, `int32` bulk flags, `int32` element count, `int32` size on disk, `int64` offset, an
  inline payload when the payload is not at the end of the file, and `int32` sizeX, sizeY. For
  `Wood_Icon`: 9 mips, mips 0 and 1 (256x256 = 65,536 bytes, 128x128 = 16,384 bytes) are stored at the end of
  the file (flags `0x1`, offsets 0 and 65,536 relative to the end of the last export) and mips 2 to 8
  are inline (flags `0x40`). The file ends with a 4-byte package tag trailer.
- **Decode:** mip 0 is wrapped in a DDS header (DXT1, DXT3, DXT5 and B8G8R8A8 are mapped) and
  `sheet.py` opens it with Pillow's DDS decoder (BC1/BC3) and saves a PNG. All 30 samples are
  `PF_DXT5`, 256x256.

The `Invalid bool value` path (a bool byte other than 0 or 1) is checked in `read_tag` and raises with the
offset, so a layout drift on another build fails loudly instead of producing garbage.

### 7. Icons exported and checked by eye

30 texture packages decoded from the client (sizes and prefixes of the file hashes are in the
appendix). A contact sheet with the package names as labels was built in the Pillow container and every
tile was looked at: resources (wood log, stone, flint, hide), food and consumables (cooked meat, Mejoberry,
narcotic and stimulant jars, waterskin, a recipe note), weapons (assault rifle, bow, pistol, hatchet,
torch, pick, arrow, bullet), armour and saddles (shirt, boots, Rex and Parasaur saddles), structures
(campfire, thatch door, fence foundation) and DLC items (Genesis shoulder cannon and Tek space whale
saddle, Abyss fish jerky and Atlan broth). All are the right object, upright, with alpha. Per PNG
statistics (`png-stats.json`) show no icon with an all-zero alpha channel and at least 1,527 distinct
colours. PNG hashes are in `sample-mapping.json`. The decoded pixels, the DDS files and the sheet stay in
the cache and are not committed.

## Sample mapping

`sample-mapping.json` has 33 rows covering a resource, consumables, food, weapons, tools, ammo, armour,
saddles, structures, dyes, icons inherited from a parent blueprint, a material-only egg and DLC
items (Genesis and Abyss). Pixels are from the client (`source: "client"`); rows with no `ItemIcon` have
`iconTexture: null` and `pngSha256: null`.

| Code (shortened) | Icon texture | Notes |
| --- | --- | --- |
| `.../Resources/PrimalItemResource_Wood.PrimalItemResource_Wood_C` | `.../Resources/Icons/Wood_Icon` | resource |
| `.../Armor/Saddles/PrimalItemArmor_RexSaddle.PrimalItemArmor_RexSaddle_C` | `.../Armor/Saddles/RexSaddle_Icon` | saddle, has `ItemIconMaterialParent` |
| `.../Consumables/PrimalItemConsumable_WaterskinRefill...` | `.../Consumables/Icons/FilledWaterskin_Icon` | inherits the icon from `PrimalItemConsumable_WaterskinCraftable` |
| `.../Items/PrimalItemDye_Red.PrimalItemDye_Red_C` | none | material-only (`DyeIconColorized`), needs a tint op |
| `/Game/Abyss/.../PrimalItemConsumable_CookedMeat_Fish_Jerky...` | `/Game/Abyss/Icons/Items/Resources/Fish_Jerky_Icon` | Abyss content |

## Recommendation

**Go.** Build ARK as a client-pixel pack, in the same family as Conan Exiles:

- The mapping from item code to icon texture is derived from data in the server depot and covers 92.0% of the
  base game data's 621 codes outright; the rest are material-only icons, a known and countable op.
- Server textures never hold pixels, so the pixels must come from the ARK SE client (`--client DIR`
  with only the icon texture packages).
- UModel cannot read these packages, so the decoder is the own reader (Go C), which already produces correct
  images for 30 of 30 sample icons.

### Follow-up ticket outline: "ARK: Survival Evolved Icon Pack"

Blocked by #742, #810, #744, and #743 only if it reuses the decoder fake pattern.

- a catalog `icons` block for `ark` (game `ark`; mapping from server depot 376031, pixels from client app
  346110);
- an `unreal-legacy` decoder container (the own reader from this spike, stdlib only, no UModel) with the same
  contract as the #743 decoder (raw PNG plus raw JSON, network-less, read-only mounts) and a decoder
  fake for tests;
- an `ark` mapping module (`MasterItemList` with `ItemIcon` and inheritance) with a small recorded
  fixture (JSON from the reader, not game files);
- the ops that are needed: alpha-bbox trim; tint or composite for the 26 dyes, 18 eggs and 6 skins (or
  an "approximate" note), and a decision about `/Game/PrimalEarth/Test/` placeholder items;
- the two-pass acquisition: server filelist for `BASE_PrimalGameData_BP` plus all `PrimalItem*` and
  `Icons/` packages (measured here: 4,853 files, 19.9 MB, about 20 seconds), then the referenced
  textures from the client (about 88 KB each, so about 50 MB for 571 icons);
- a first task: copy all 571 referenced textures from the client and check which pixel formats occur
  (this spike only saw `PF_DXT5`); add the missing formats to `dds_header` (a stripped or unknown format is
  reported, not guessed);
- a DLC task: find how DLC items are merged into the live `MasterItemList`, and measure DLC coverage once
  #811 (the live code export) is available.

## Limits and what is not proven

- Pixel formats other than `PF_DXT5` were not seen (only 30 textures were copied, as the plan requires).
- The mapping is measured against codes from game data, not the live list. DLC coverage is unknown.
- Client build 21291776 and server build 21241282 differ. Texture pixels were not compared across
  builds; only dimensions were.
- The `HasPropertyGuid` explanation for the CUE4Parse failure is a hypothesis and was not tested.
- The mip payload base (end of the last export) was verified on the sample files, where the payload plus
  the 4-byte trailer exactly fills the rest of the file, not against the summary's own field.

## Appendix: files used

Game-data packages (server depot 376031, manifest `6366771435093287465`):

| File | Bytes | sha256 |
| --- | --- | --- |
| `PrimalEarth/CoreBlueprints/BASE_PrimalGameData_BP.uasset` | 244,826 | `82f918b8c6c6f039d3095168ca85f02249b1a5e2d67dbde20a67a58460c67eaf` |
| `PrimalEarth/CoreBlueprints/COREMEDIA_PrimalGameData_BP.uasset` | 3,470,533 | `72da1a9942dbfb4c49caf3318a4eab6b60ec308440b26722b953e7aa3023d4e0` |
| `PrimalEarth/CoreBlueprints/CORE_PrimalGameData.uasset` | 2,702 | `fe2f96b761fa08976120deb83032d1bd2ab97a5b6369a16afa0e4f491ab74f45` |
| `PrimalEarth/CoreBlueprints/PrimalGameData_BP.uasset` | 2,986 | `0e47c14132e4655a5ed170ac2cac51b9aa95970399ff3b0282787ad0ad662a5c` |

The other 4,849 pass 1 packages (item blueprints and `Icons/` textures) are identified by the manifest
and the listing. Sample texture packages, under `ShooterGame/Content/` (sha256 shown as the first 16 hex
digits; the server copies are the stripped ones):

| Package | Server bytes | Server sha256 | Client bytes | Client sha256 |
|---|---|---|---|---|
| `Abyss/Icons/Items/Resources/Broth_of_Atlan_Recipe_Icon` | 616 | `403bdad7bf161d45` | 88372 | `0942bfd7d766cc74` |
| `Abyss/Icons/Items/Resources/Fish_Jerky_Icon` | 594 | `5a468cf8ea2f23ff` | 88350 | `061cb63ea000cb3a` |
| `Genesis/UI/Icons/HUD_SpaceWhaleSaddle_Icon` | 603 | `866680b9bed2e208` | 88359 | `2fbe1eff20005b80` |
| `Genesis/UI/Icons/HUD_TEKShoulderCannon_Icon` | 605 | `6d95f927680475b2` | 88361 | `409a2f8b606bb5b3` |
| `PrimalEarth/CoreBlueprints/Items/Armor/Icons/BurlapShirt_Icon` | 613 | `c18c7637a4772cde` | 88369 | `7c37407492bafc4f` |
| `PrimalEarth/CoreBlueprints/Items/Armor/Leather/Icons/HideBoots_Icon` | 617 | `d97a25da83fbf891` | 88373 | `e163fad899f80150` |
| `PrimalEarth/CoreBlueprints/Items/Armor/Saddles/Icons/ParasaurSaddle_Icon` | 627 | `e593195b5419cea9` | 88383 | `7316c4407521db4a` |
| `PrimalEarth/CoreBlueprints/Items/Armor/Saddles/RexSaddle_Icon` | 611 | `8d39a11439b9b07f` | 88367 | `23d5f689bdad87c4` |
| `PrimalEarth/CoreBlueprints/Items/Consumables/Icons/AmarberrySeed_Icon` | 623 | `f20a9373cd3bc68f` | 88379 | `953be6438733aaf9` |
| `PrimalEarth/CoreBlueprints/Items/Consumables/Icons/CookedMeat_Icon` | 617 | `3612c49be16f4fda` | 88373 | `ddafc91458dda0f9` |
| `PrimalEarth/CoreBlueprints/Items/Consumables/Icons/FilledWaterskin_Icon` | 627 | `3927e9f5fd3fc4fa` | 88383 | `a6e29357e86c57e8` |
| `PrimalEarth/CoreBlueprints/Items/Consumables/Icons/MejoBerry_Icon` | 615 | `3cf19d2e3a2b943b` | 88371 | `cb867d0d6f6efa28` |
| `PrimalEarth/CoreBlueprints/Items/Consumables/Icons/Narcotic_Icon` | 613 | `9b1a8080b7c212cc` | 88369 | `6faa96a1a53799ce` |
| `PrimalEarth/CoreBlueprints/Items/Consumables/Icons/Stimulant_Icon` | 615 | `3fa7f972e17b7e26` | 88371 | `936983aefe409dec` |
| `PrimalEarth/CoreBlueprints/Items/Notes/Icons/Recipe_Icon` | 603 | `15059dc283f704e9` | 88359 | `0321f8444c4c9c92` |
| `PrimalEarth/CoreBlueprints/Items/Structures/Icons/Campfire_Icon` | 612 | `f1b7d1c316629f4e` | 88368 | `58e3239d2fdf9277` |
| `PrimalEarth/CoreBlueprints/Items/Structures/Icons/FenceFoundation` | 616 | `c202d6ce7b5b5cee` | 88372 | `4353855904a51818` |
| `PrimalEarth/CoreBlueprints/Items/Structures/Icons/Thatch/ThatchDoor_Icon` | 623 | `3d09b7ae8dbd478e` | 88379 | `4e34991f2c849014` |
| `PrimalEarth/CoreBlueprints/Resources/Icons/Flint_Icon` | 599 | `3697d2ca214cf4a5` | 88355 | `8dbbb35671651e97` |
| `PrimalEarth/CoreBlueprints/Resources/Icons/Hide_Icon` | 597 | `1b04ff986fdca373` | 88353 | `e8528f268ce2b737` |
| `PrimalEarth/CoreBlueprints/Resources/Icons/Stone_Icon` | 599 | `e67a3f66d74e4b4d` | 88355 | `e12073b0a2c5d2dc` |
| `PrimalEarth/CoreBlueprints/Resources/Icons/Wood_Icon` | 597 | `a49c1140b5b11e1c` | 88353 | `08a2158fe1e61eb4` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/Arrow_Icon` | 597 | `30497d7b3e82ec20` | 88353 | `1e18a5848ae711bc` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/AssaultRifle_Icon` | 611 | `8b4727e4da1cdb20` | 88367 | `507cc340dbd54669` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/Bow_Icon` | 593 | `87fe2f07aafcca1f` | 88349 | `70b2ed3fb458e8ae` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/Bullet4_Icon` | 601 | `9daf0e286e2d5421` | 88357 | `5294a0dfbb54b768` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/MetalPick_Icon` | 605 | `a4c53bfad5963b96` | 88361 | `5d33bd8cf5a96c21` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/Pistol_Icon` | 599 | `5bd96802e9059851` | 88355 | `b9b05826b02fe81d` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/StoneHatchet_Icon` | 611 | `118e0781eb7ec3fc` | 88367 | `874415e852ce50de` |
| `PrimalEarth/CoreBlueprints/Weapons/Icons/Torch_Icon` | 597 | `6fa5487db9ee3cfe` | 88353 | `e5b11d594137b17a` |
