#!/usr/bin/env python3
"""Render the Enshrouded world map into a Takaro tile pack (served by the native connector, maptiles.h).

Input: the raw map textures the game client's map screen is drawn from, as exported by the EML mod in
this folder (src/mod.lua, `emm run -e`):
  _map_shader_parameters_baseHeight.raw      1280x1280 R16_unorm   (whole world, 8 m per pixel)
  _map_shader_parameters_baseGradient.raw    151x1 R8G8B8A8_unorm  (height -> colour ramp)
  DetailHeight_<x>_<y>_.raw                  10x10 tiles of 1024x1024 BC4_unorm (1 m per pixel relief)

World frame (build 1076226): origin (0,0), 10240 x 10240 m in x/z (VoxelWorldResource). Base image
column = x/8 and row = (10240 - z)/8 (north up); DetailHeight(i,j) covers x in [1024 i, 1024 i + 1024)
and z in [1024 j, 1024 j + 1024). Verified against 1031 placed locations (height correlation 0.91).

Output (Takaro / takaro-player-map tiling, 128 px tiles, 1 m per pixel at maxZoom 4): a new pack folder
  <out>/pack-<UTC time>/ with map.json, outside.png and <z>/<x>/<y>.png (x east, y north from the origin),
  switched live through <out>/current only once complete (pack.py); older packs are pruned.

This is our own rendering of the game's height data (ramp colours from the game, hillshade and
contours computed here), not a copy of the in-game map screen. The pack is derived from Keen Games'
assets: fine on a dev rig, do not redistribute it without permission.

Usage: render_tiles.py <raw-dir> [<out-dir>] [--image map.jpg [--image-size 8192]] [--preview preview.png]
  --image writes one north-up image of the whole world for the dashboard's "upload map" (protocol 1,
  Dimension embervale): top-left pixel = x 0, z 10240; bottom-right = x 10240, z 0. Without <out-dir>
  no tile pack is written.
Needs numpy and pillow.
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # pack.py beside this file, also under python -I
from pack import publish  # noqa: E402

WORLD = 10240
TILE = 128
DETAIL_TILES = 10
DETAIL_SIZE = 1024
BASE_SIZE = 1280
HEIGHT_SCALE = 0.0629  # metres per R16 unit (fit against placed locations); offset irrelevant here
CONTOUR_M = 25.0
MAX_ZOOM = 4  # 1 m per pixel; the player-map tracker assumes 128 world units per tile at zoom 4


def bc4_decode(data, w, h):
    blk = np.frombuffer(data, np.uint8).reshape(h // 4, w // 4, 8)
    r0 = blk[..., 0].astype(np.int32)
    r1 = blk[..., 1].astype(np.int32)
    bits = np.zeros(blk.shape[:2], np.uint64)
    for i in range(6):
        bits |= blk[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    pal = np.zeros(blk.shape[:2] + (8,), np.int32)
    pal[..., 0] = r0
    pal[..., 1] = r1
    gt = r0 > r1
    for i in range(1, 7):
        pal[..., 1 + i] = np.where(gt, ((7 - i) * r0 + i * r1) // 7, 0)
    for i in range(1, 5):
        pal[..., 1 + i] = np.where(gt, pal[..., 1 + i], ((5 - i) * r0 + i * r1) // 5)
    pal[..., 6] = np.where(gt, pal[..., 6], 0)
    pal[..., 7] = np.where(gt, pal[..., 7], 255)
    out = np.zeros((h // 4, 4, w // 4, 4), np.uint8)
    for py in range(4):
        for px in range(4):
            idx = ((bits >> np.uint64(3 * (py * 4 + px))) & np.uint64(7)).astype(np.int64)
            out[:, py, :, px] = np.take_along_axis(pal, idx[..., None], axis=2)[..., 0]
    return out.reshape(h, w)


def read(raw_dir, name, size):
    path = os.path.join(raw_dir, name)
    data = open(path, "rb").read()
    if len(data) < size:
        sys.exit(f"{path}: {len(data)} bytes, expected {size}")
    if not any(data[: min(len(data), 1 << 16)]):
        sys.exit(f"{path}: all zero (incomplete client install?)")
    return data[:size]


def load(raw_dir):
    base = np.frombuffer(read(raw_dir, "_map_shader_parameters_baseHeight.raw", BASE_SIZE * BASE_SIZE * 2), "<u2")
    base = base.reshape(BASE_SIZE, BASE_SIZE).astype(np.float32)
    ramp = np.frombuffer(read(raw_dir, "_map_shader_parameters_baseGradient.raw", 151 * 4), np.uint8)
    ramp = ramp.reshape(151, 4)[:, :3].astype(np.float32)
    # detail rows in image order (north first): DetailHeight(i, j) has j growing north
    detail = np.zeros((WORLD, WORLD), np.uint8)
    for i in range(DETAIL_TILES):
        for j in range(DETAIL_TILES):
            raw = read(raw_dir, f"DetailHeight_{i}_{j}_.raw", DETAIL_SIZE * DETAIL_SIZE // 2)
            row = DETAIL_TILES - 1 - j
            detail[row * DETAIL_SIZE:(row + 1) * DETAIL_SIZE, i * DETAIL_SIZE:(i + 1) * DETAIL_SIZE] = bc4_decode(
                raw, DETAIL_SIZE, DETAIL_SIZE)
    return base, ramp, detail


def upsample_base(base, r0, r1):
    """Bilinear base height (metres) for image rows r0..r1 at 1 m per pixel."""
    scale = BASE_SIZE / WORLD
    ys = (np.arange(r0, r1, dtype=np.float32) + 0.5) * scale - 0.5
    xs = (np.arange(WORLD, dtype=np.float32) + 0.5) * scale - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, BASE_SIZE - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, BASE_SIZE - 1)
    y1 = np.clip(y0 + 1, 0, BASE_SIZE - 1)
    x1 = np.clip(x0 + 1, 0, BASE_SIZE - 1)
    fy = np.clip(ys - y0, 0, 1)[:, None]
    fx = np.clip(xs - x0, 0, 1)[None, :]
    top = base[y0][:, x0] * (1 - fx) + base[y0][:, x1] * fx
    bot = base[y1][:, x0] * (1 - fx) + base[y1][:, x1] * fx
    return (top * (1 - fy) + bot * fy) * HEIGHT_SCALE


def render(base, ramp, detail, band=512):
    # stretch the ramp over the land's height range (the sea floor sits at the bottom of the R16 range)
    hmin = float(np.percentile(base, 2)) * HEIGHT_SCALE
    hmax = float(np.percentile(base, 99.9)) * HEIGHT_SCALE
    img = np.zeros((WORLD, WORLD, 3), np.uint8)
    for r0 in range(0, WORLD, band):
        a0, a1 = max(0, r0 - 1), min(WORLD, r0 + band + 1)  # one-row halo for gradients
        h = upsample_base(base, a0, a1)
        d = detail[a0:a1].astype(np.float32) - 128.0
        # colour from the game's own height ramp
        t = np.clip((h - hmin) / (hmax - hmin), 0, 1) ** 0.6 * (len(ramp) - 1)
        i0 = np.floor(t).astype(int)
        i1 = np.minimum(i0 + 1, len(ramp) - 1)
        f = (t - i0)[..., None]
        col = ramp[i0] * (1 - f) + ramp[i1] * f
        # hillshade from the 1 m relief layer (light from the north-west) plus the coarse slope
        surf = h + d * 0.5
        gy, gx = np.gradient(surf)
        shade = np.clip(1.0 + (-gx + gy) * 0.06, 0.55, 1.45)
        col = col * shade[..., None]
        # contour lines every CONTOUR_M metres of the smooth base height
        level = np.floor(h / CONTOUR_M)
        edge = np.zeros_like(h, bool)
        edge[:-1] |= level[:-1] != level[1:]
        edge[:, :-1] |= level[:, :-1] != level[:, 1:]
        major = np.zeros_like(edge)
        major[:-1] |= np.floor(h[:-1] / (CONTOUR_M * 4)) != np.floor(h[1:] / (CONTOUR_M * 4))
        col[edge] *= 0.82
        col[major] *= 0.75
        s0 = r0 - a0
        img[r0:min(WORLD, r0 + band)] = np.clip(col[s0:s0 + min(band, WORLD - r0)], 0, 255).astype(np.uint8)
    return img


def write_tiles(img, out, max_zoom, revision):
    count = 0
    for z in range(max_zoom, -1, -1):
        factor = 2 ** (max_zoom - z)
        size = WORLD // factor
        level = img if factor == 1 else np.asarray(Image.fromarray(img).resize((size, size), Image.BOX))
        n = -(-size // TILE)
        for x in range(n):
            xdir = os.path.join(out, str(z), str(x))
            os.makedirs(xdir, exist_ok=True)
            for y in range(n):
                # tile y grows north; image rows grow south
                rb = size - y * TILE
                ra = max(0, rb - TILE)
                tile = np.zeros((TILE, TILE, 4), np.uint8)
                part = level[ra:rb, x * TILE:min(size, (x + 1) * TILE)]
                tile[TILE - part.shape[0]:, :part.shape[1], :3] = part
                tile[TILE - part.shape[0]:, :part.shape[1], 3] = 255
                Image.fromarray(tile, "RGBA").save(os.path.join(xdir, f"{y}.png"), optimize=True)
                count += 1
    Image.fromarray(np.zeros((TILE, TILE, 4), np.uint8), "RGBA").save(os.path.join(out, "outside.png"))
    manifest = {
        "format": "takaro-map-tiles/1",
        "game": "enshrouded",
        "tileSize": TILE,
        "mapBlockSize": TILE,
        "minZoom": 0,
        "maxZoom": max_zoom,
        "mapSizeX": WORLD,
        "mapSizeY": 2648,
        "mapSizeZ": WORLD,
        "outsideTile": "outside.png",
        "revision": revision,
        "origin": {"x": 0, "z": 0},
        "metresPerPixelAtMaxZoom": 1,
        "source": "Enshrouded client map textures (_map_shader_parameters_baseHeight, baseGradient, DetailHeight), "
                  "rendered by games/enshrouded/mod/tools/map-tiles/render_tiles.py; derived from Keen Games assets, "
                  "not for redistribution",
    }
    with open(os.path.join(out, "map.json"), "w") as f:  # last: a pack without map.json is incomplete
        json.dump(manifest, f, indent=2)
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw_dir")
    ap.add_argument("out_dir", nargs="?", help="tile pack root for the protocol 0 map (optional)")
    ap.add_argument("--image", help="write the whole world as one image for Takaro's map upload (.jpg/.png/.webp)")
    ap.add_argument("--image-size", type=int, default=8192, help="side of --image in pixels (Takaro allows up to 8192)")
    ap.add_argument("--preview", help="also write a 2048 px overview PNG here")
    a = ap.parse_args()
    if not a.out_dir and not a.image and not a.preview:
        ap.error("nothing to write: give <out-dir>, --image or --preview")
    if not 1 <= a.image_size <= 8192:
        ap.error("--image-size must be 1..8192")
    base, ramp, detail = load(a.raw_dir)
    img = render(base, ramp, detail)
    if a.preview:
        Image.fromarray(img).resize((2048, 2048), Image.BOX).save(a.preview)
    if a.image:
        # rows already run north to south and columns west to east: exactly the Dimension's screen frame
        out = Image.fromarray(img).resize((a.image_size, a.image_size), Image.BOX)
        out.save(a.image, **({"quality": 90} if a.image.lower().endswith((".jpg", ".jpeg", ".webp")) else {"optimize": True}))
        print(f"{a.image}: {a.image_size} x {a.image_size} px, {os.path.getsize(a.image)} bytes")
    if not a.out_dir:
        return
    count = []
    name = publish(a.out_dir, lambda d, rev: count.append(write_tiles(img, d, MAX_ZOOM, rev)))
    print(f"{count[0]} tiles written to {os.path.join(a.out_dir, name)}, now live via {a.out_dir}/current")


if __name__ == "__main__":
    main()
