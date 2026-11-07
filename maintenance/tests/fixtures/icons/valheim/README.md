# Valheim icon fixtures

Everything here is synthetic. No game file, game art, game name or game bytes are committed, and no recorded decoder
output of the real game: the files have the *shape* the Unity decoder returns, with invented names.

| Path | Origin |
|---|---|
| `depots/head.json`, `depots/896661/<manifest>/tree/**` | Five bundle stand-ins (`aaaa0001` to `aaaa0005`, a few synthetic bytes each) that the catalog selector must pick up, plus four decoys it must leave out: `SoftRef/manifest`, `Bundles/sub/0abc`, `Bundles/README.txt` and `resources.assets`. The decoder stand-in never parses them. |
| `decoder/index.json`, `decoder/json/*.json` | The recorded answers of the decoder stand-in: `bundle-index`, the `ZNetScene` and `ItemDrop` MonoBehaviour dumps and one `sprite` description per sprite. |
| `decoder/png/*.png` | The atlas (`texture`) and, per rectangle-packed sprite, UnityPy's own cut (`sprite-image`), written with `compress_level=0`. |

`make_fixture.py` generates all of it from one Python description, so the JSON and the pictures cannot drift apart
(`python make_fixture.py` rewrites the committed files; `test_cli_icons_valheim.py` regenerates into a temporary
directory and requires byte equality).

What the description models:

- Five CABs `cab-a` to `cab-e`, one per bundle. The cross-bundle chain is `ZNetScene` (`cab-a`) to a prefab GameObject
  and its root `ItemDrop` (`cab-b`) to a Sprite (`cab-c`) to the atlas Texture2D (`cab-d`); one sprite goes through a
  SpriteAtlas render-data entry (`cab-e`).
- A prefab with three variant icons (the first wins), a prefab name with two copies in different bundles (the first
  `m_prefabs` entry wins, the later copy counts as a repeated name), an unreferenced `ItemDrop` with a used name, a
  prefab without `ItemDrop`, a null `m_prefabs` entry and one reference into no bundle.
- `ItemDrop`s with an empty `m_icons` and with `[null]`, a sprite in no bundle, a texture recorded as
  `unsupported-format`, a tight-packed sprite and one with an alpha texture.
- One 64x48 atlas (not square, so a width/height swap shows) of uniquely coloured 8x8 cells with an asymmetric white
  marker pixel in each. Sprites: a plain rect away from the origin (catches a vertical flip), fractional rects that hit
  both rounding directions including exact `.5` cases, each of the four packing rotations (with non-square cuts, so a
  quarter turn changes the size), and a trimmed sprite with a canvas and an offset.
- The expected pixels are computed by `expected_image` from UnityPy's bottom-up formulation (crop in Unity texel space,
  transpose, flip last), independently of `ops.atlas_cut`.

Deviation from the Conan fixtures, which trim real decoded JSON: the run rules forbid committing verbatim game data, so
no real Valheim decoder text is recorded here.
