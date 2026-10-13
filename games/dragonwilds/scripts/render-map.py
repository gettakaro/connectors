#!/usr/bin/env python3
"""Render the plugin's map survey into a top-down map image for Takaro's map upload.

The Dragonwilds dedicated server ships no map art (its cook strips texture pixels), so the plugin
samples the world's collision from above (`POST /debug/map/survey`, see mod/src/mapsurvey.h) and
this script shades that height field: elevation tint plus a hillshade, water below `--sea-level`.

    render-map.py <survey dir> <out.png> [--sea-level Z] [--max-side 8192]

<survey dir> holds meta.json, heights.f32 and normalz.f32 (copied from <serverdir>/takaro/map/).
Next to <out.png> it writes <out>.settings.json, the `settings` part of Takaro's
`PUT /gameserver/{id}/map/{dimension}/upload`: positions are raw Unreal units (Takaro x = Unreal X,
y = Unreal Y, z = Unreal Z up), so the map plane is x (right) by y (down), which is the unmirrored
view from above for Unreal's left-handed axes.

Needs numpy and Pillow (a developer tool, not shipped with the plugin).
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image

# Elevation tint, (fraction of the land range, RGB); land is tinted between sea level and the
# 99.5th height percentile so a few peaks do not wash the rest out.
LAND = [
    (0.00, (196, 186, 140)),
    (0.04, (128, 160, 96)),
    (0.30, (92, 132, 74)),
    (0.55, (128, 128, 92)),
    (0.80, (150, 136, 120)),
    (1.00, (236, 236, 236)),
]
WATER_SHALLOW = np.array((96, 150, 190), dtype=np.float32)
WATER_DEEP = np.array((40, 76, 120), dtype=np.float32)
NO_DATA = np.array((26, 46, 74), dtype=np.float32)


def load(survey_dir):
    with open(os.path.join(survey_dir, "meta.json")) as f:
        meta = json.load(f)
    w, h = int(meta["w"]), int(meta["h"])
    heights = np.fromfile(os.path.join(survey_dir, "heights.f32"), dtype="<f4")
    if heights.size != w * h:
        sys.exit(f"heights.f32 has {heights.size} samples, meta says {w}x{h}")
    return meta, heights.reshape(h, w)


def tint(frac):
    out = np.zeros(frac.shape + (3,), dtype=np.float32)
    for (f0, c0), (f1, c1) in zip(LAND, LAND[1:]):
        m = (frac >= f0) & (frac <= f1)
        t = ((frac[m] - f0) / (f1 - f0))[:, None]
        out[m] = np.array(c0, np.float32) * (1 - t) + np.array(c1, np.float32) * t
    return out


def hillshade(z, step, azimuth=315.0, altitude=45.0):
    # Row index grows with Unreal Y (south on the rendered map), column with X (east).
    gy, gx = np.gradient(np.nan_to_num(z, nan=float(np.nanmin(z))), step)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gy, gx)
    az, alt = math.radians(360.0 - azimuth + 90.0), math.radians(altitude)
    shade = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(shade, 0, 1)


def render(meta, z, sea_level):
    valid = np.isfinite(z)
    if not valid.any():
        sys.exit("the survey has no hits; was the world loaded (server streaming off)?")
    if sea_level is None:
        sea_level = float(np.nanpercentile(z, 1))
    top = float(np.nanpercentile(z, 99.5))
    land = valid & (z > sea_level)
    frac = np.clip((z - sea_level) / max(top - sea_level, 1.0), 0, 1)
    rgb = tint(np.nan_to_num(frac))
    depth = np.clip((sea_level - np.nan_to_num(z, nan=sea_level)) / 3000.0, 0, 1)[..., None]
    water = WATER_SHALLOW * (1 - depth) + WATER_DEEP * depth
    rgb = np.where(land[..., None], rgb, water)
    shade = hillshade(z, float(meta["step"]))[..., None]
    rgb = np.where(land[..., None], rgb * (0.45 + 0.75 * shade), rgb)
    rgb = np.where(valid[..., None], rgb, NO_DATA)
    return np.clip(rgb, 0, 255).astype(np.uint8), sea_level


def settings(meta):
    step = float(meta["step"])
    min_x, min_y = float(meta["minX"]), float(meta["minY"])
    return {
        "name": "Dragonwilds",
        "axes": {
            "horizontal": {"field": "x", "direction": "right"},
            "vertical": {"field": "y", "direction": "down"},
        },
        "bounds": {
            "horizontal": {"min": min_x, "max": min_x + step * int(meta["w"])},
            "vertical": {"min": min_y, "max": min_y + step * int(meta["h"])},
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("survey_dir")
    ap.add_argument("out_png")
    ap.add_argument("--sea-level", type=float, default=None,
                    help="Unreal Z below which a hit is water (default: 1st height percentile)")
    args = ap.parse_args()
    meta, z = load(args.survey_dir)
    rgb, sea = render(meta, z, args.sea_level)
    Image.fromarray(rgb, "RGB").save(args.out_png, optimize=True)
    out = settings(meta)
    with open(os.path.splitext(args.out_png)[0] + ".settings.json", "w") as f:
        json.dump(out, f, indent=2)
    print(json.dumps({"png": args.out_png, "w": int(meta["w"]), "h": int(meta["h"]), "seaLevel": sea,
                      "hits": int(np.isfinite(z).sum()), "bounds": out["bounds"]}))


if __name__ == "__main__":
    main()
