# Terraria icon fixtures

Everything here is synthetic. No game file, game art, game name or game bytes are committed, and no recorded decoder
output or decompiled text of the real game: the files have the *shape* the decoders return, with invented names and
hand-written C#. Only the numeric code `5644` is a real item id, because a rule of the mapper names it.

| Path | Origin |
|---|---|
| `connector/itemCatalog.ts` | A synthetic item catalogue in the shape of the connector's `itemCatalog.ts` (14 invented items). |
| (built in the test) `client/steamapps/appmanifest_105600.acf` | A synthetic appmanifest: build id 24893155, no account id. |
| (built in the test) `client/steamapps/common/Terraria/Terraria.exe` | Synthetic bytes. The decoder stand-in never parses it. |
| (built in the test) `.../Content/Images/Item_<N>.xnb` | Synthetic bytes, one per item with art, plus one id the catalogue does not list (`99`). |
| (built in the test) `.../Content/Images/{Item_7.png,Projectile_1.xnb,sub/Item_3.xnb,Item_x.xnb,Item_8.xnb.bak}` | Decoys the `regex:` selector must leave out. |
| `decoder/index.json`, `decoder/json/*.json` | The recorded answers of the decoder stand-ins: `assembly-info`, `callers` and `decompile-type` (with synthetic C# snippets in the decompiled style). |
| `decoder/png/*.png` | The decoded textures, written with `compress_level=0`. |

`make_fixture.py` generates all of it from one Python description, so the JSON and the pictures cannot drift apart
(`python make_fixture.py` rewrites the committed files; `test_cli_icons_terraria.py` regenerates into a temporary
directory and requires byte equality). The client tree is written by `make_fixture.write_client` into a temporary
directory by each test run and is never committed, so no `.xnb` or `.exe` file is in the repository.

What the description models:

- A plain item, a 4-frame registered strip (`Synth Strip`), a food item whose 3 frames come from the `IsFood` loop and
  the 9-frame Scrying Orb (`5644`, registered as a different animation type, so the documented override applies).
  Strips are 6 px wide with frames of 5 distinct coloured rows and 2 transparent gap rows.
- `Alpha Chest` with a texture and `Fake Alpha Chest` without one (fallback to the real chest); `Fake Beta Chest` with
  its own texture (no fallback); `Fake Chest` and `Fake newchest1`, which stay unmatched.
- A texture the decoder reports as `unsupported-format`, two items with identical pixels (one shared object) and a
  texture with one translucent pixel whose straight RGBA must survive unchanged.
