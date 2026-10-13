# Enshrouded map image and tile pack

Takaro protocol 1 (the default): the connector declares the world as Dimension `embervale` (x right,
z up, 0..10240 m) with no image of its own, because the dedicated server ships no map art. The
operator uploads one image in the dashboard. This tool draws that image from the map data of the
operator's own game client:

    python3 render_tiles.py <raw dir> --image embervale.jpg     # 8192 x 8192, north up, edge to edge

Upload it on the game server's map page and leave the bounds empty. `<raw dir>` comes from step 1 below.

Protocol 0 (older Takaro): the native connector answers `getMapInfo` / `getMapTile` from a tile pack
in `<server>\takaro\map\` (override with `TAKARO_MAP_DIR` or `mapDir` in `takaro\plugin.json`); see
`src/native/maptiles.h`.
Each render is its own folder (`pack-<UTC time>/`), and the one-line file `current` names the folder
being served. Only `current` switches packs, so a half-copied pack is never served and tiles from two
renders never mix.
The dedicated server ships no map art, so the pack is rendered offline from the game client:

1. Export the raw map textures from a complete client install with the EML CLI (kfc-parser `emm`):
   copy `eml-mod/` to `<client>/mods/takaro_map_export/`, then
   `emm run -e -g <client dir> --export-directory <raw dir>`.
2. Render: `python3 render_tiles.py <raw dir> <out dir>` (numpy, pillow; about 10 minutes, 8525 tiles,
   ~110 MB). It writes a new `pack-…` folder under `<out dir>` and points `<out dir>/current` at it
   only when the folder is complete. When `<out dir>` is the server's `takaro/map/`, you are done.
3. Otherwise install it while the server runs: `python3 pack.py install <out dir> <server>/takaro/map/`.
   It copies the pack into a hidden folder, renames it into place, then switches `current`. No restart
   needed. By hand: copy the `pack-…` folder first, then copy `current` last. Never copy tiles into a
   pack folder that `current` already names.

Both commands keep the live pack and the one before it, and delete older `pack-…` folders.

World frame (build 1024233 server / 1076226 client): origin (0,0), 10240 x 10240 m (x east, z north),
1 m per pixel at zoom 4, 128 px tiles; tile y grows north. This matches takaro-player-map's tiling.

The pack is derived from Keen Games' client assets (height data and colour ramp). Use it on servers
you run; do not publish or redistribute it without permission.
