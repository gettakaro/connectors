# Minecraft icon fixtures

Everything here is synthetic. No Mojang model, texture, registry text or other game data is committed: the item names are
`minecraft:fixture_*` (plus `minecraft:air`, which the registry has and the pack excludes), the models are hand-written
boxes and flat items in the shape the mapper reads, and the textures are flat colours.

| Path | Origin |
|---|---|
| `public/client.jar` | A synthetic client jar: item definitions, models and textures for every rule of the mapper. |
| `public/server.jar` | Opaque synthetic bytes. The decoder stand-in never executes it. |
| `public/version.json` | A synthetic version manifest: `id` 26.3 and the sha1 and size of the two synthetic jars. |
| `decoder/index.json`, `decoder/json/registries.json` | The recorded answer of the data-generator stand-in: a synthetic item and block registry. |

`make_fixture.py` generates all of it from one Python description (`python make_fixture.py` rewrites the committed files;
`test_cli_icons_minecraft.py` regenerates into a temporary directory and requires byte equality). PNGs are written with
`compress_level=0` and the jars are `ZIP_STORED` with sorted entries and fixed timestamps, so the bytes are reproducible.

What the description models:

- A leather-like flat item (a dye-tinted base and an untinted overlay layer) and a potion-like one (a potion tint).
- A full-cube block with six distinct face colours reached through a three-level parent chain, a wall-like block (a post and
  two arms, faces with `cullface` and without `uv`) and a block tinted through the grass colormap (a base element and a
  coplanar overlay element), a constant-tint block, an element with a rescaled rotation, a face rotation, a
  `shade_direction_override`, `gui_light: front`, a cutout and a translucent texture, and an animated block texture.
- A flat item with an animated texture whose `frames` start at index 2.
- Item definitions of every resolution rule: `condition`, `select` on the display context and on another property,
  `range_dispatch` without a fallback, `composite`, `special` and `empty`.
- Unmatched cases: a missing texture, a missing model, a parent cycle, an unknown tint, an unknown element key, an
  animation with a declared width, and a registry item without an item definition. An item definition for an item that is
  not in the registry is present and must be ignored.
- Two items with identical art, which share one object.
