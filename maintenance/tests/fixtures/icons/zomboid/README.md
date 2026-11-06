# Project Zomboid icon fixture

What `takaro-maint icons build --game zomboid` reads, shrunk to a few kilobytes. The tests
(`tests/test_cli_icons_catalog.py`) run the real command against it, with the DepotDownloader
stand-in (`tests/fake_depotdownloader.py`) serving `depots/`.

## What is real and what is synthetic

| Path | Origin |
|---|---|
| `depots/380871/7440000000000000001/tree/media/scripts/generated/items/fixture_nested.txt` | Real excerpts: the item blocks `Axe`, `Bandage`, `Glasses_Normal` (`IconsForTexture`), `Wound_Chest_Bite_Male` (no icon), `Bag_LeatherWaterBag` (a nested `component FluidContainer` block), `Hematite` and `Mov_SodaMachine` (`Icon = default` moveables), copied verbatim from the server depot's `media/scripts/generated/items/` (app 380870, depot 380871, manifest 5337933334995336130, build 25485538, Project Zomboid 42.21.0). Text game data, a small slice. |
| `depots/380871/7440000000000000001/tree/media/scripts/items_fixture.txt` | Synthetic items, each commented in the file (a commented-out item, a lowercase `icon` key, obsolete, disabled and debug-only items, a missing texture, moveables with a missing or absent sprite, a repeated `Base.Axe`), plus a real `entity` excerpt (`entity_wood_stake.txt`) whose `inputs` block holds the line `item 1 [Base.Plank]`. |
| `.../media/scripts/notes.md`, `.../media/other/items.txt` | Synthetic decoys: a non-`.txt` file under `media/scripts` and a `.txt` outside it. The file filter must leave both out. |
| `depots/head.json` | The fake DepotDownloader's branch head. Tests re-pin the catalog to manifest `7440000000000000001`. |
| `client/appmanifest_108600.acf` | A trimmed copy of the real client manifest (build 25485521, depot 108604). The `LastOwner` field is not carried over. |
| `client/media/texturepacks/*.pack` | Synthetic, produced by `make_fixture.py`. **No game art.** Every texture is a solid, uniquely coloured rectangle, so a test tells where an icon came from by its colour. |

The mock manifest id is not a real Steam manifest: the stand-in only needs a directory name.

## The texture-pack layout

All integers are little-endian int32; a string is an int32 byte length and then that many bytes (latin-1).

```
v2 ("PZPK"):                          v1 (no magic):
  magic   4 bytes = "PZPK"              numPages int32
  version int32   = 1                   page * numPages:
  numPages int32                          (same page header and entries as v2)
  page * numPages:                        PNG bytes, up to and including the IEND chunk,
    name        string                    then the 4 bytes EF BE AD DE (0xDEADBEEF)
    numEntries  int32
    mask        int32
    entry * numEntries:
      name   string
      x, y, w, h, ox, oy, fw, fh   int32 * 8
    pngLength   int32
    PNG bytes   (pngLength)
```

An entry is the rectangle `(x, y, w, h)` of the page PNG. The packer trimmed it from an original
`fw x fh` frame and `(ox, oy)` is where the rectangle sat in that frame.

## The packs

| Pack | Layout | Holds |
|---|---|---|
| `UI2.pack` | v2, 2 pages | `Item_Axe` (red), `Item_GlassesBlack` (blue), `Item_GlassesBlue` (green), `Item_Wineskin` (cyan) |
| `UI.pack` | v1, 2 pages | `Item_Axe` (yellow, shadowed by UI2), `Item_Bandage` (magenta, and a second pink entry that must lose), `Item_Hammer` (grey) |
| `IconsMoveables.pack` | v1, 1 page | `Item_Moveable_floorrug`, not referenced by any item |
| `Tiles2x.pack` | v2, 1 page | the sprites `crafting_ore_01_9` (orange) and `location_shop_accessories_01_8` (purple): 64x128 frames, a non-zero offset and a transparent margin round the shape |
| `Tiles2x.floor.pack` | v2, 1 page | `floors_fixture_01_0`, not referenced by any item |

Item icons are 20x24 crops at offset (6, 4) inside a 32x32 frame.

## Regenerating

```bash
uv run --project maintenance python maintenance/tests/fixtures/icons/zomboid/make_fixture.py
```

PNG pages are written with `compress_level=0` (stored deflate blocks), so the bytes do not depend on
the zlib build. A test regenerates the packs and requires them to equal the committed files.
