"""Convert the reader's DDS mip 0 files to PNG, inspect them and build a labelled contact sheet
(spike #813; the output is never committed).

    python -I sheet.py --dds-dir /in/dds --out /out/sheet.png --stats /out/png-stats.json

The PNGs are written next to the sheet, under png/. Statistics per PNG: size, sha256 of the file, whether the alpha channel is entirely zero, and the
number of distinct colours (a blank or placeholder texture has one). The sheet labels every tile
with the PNG's file name so each icon can be checked by eye against its item code.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw

TILE = 128
LABEL_HEIGHT = 28
COLUMNS = 8


def stats(path: Path, root: Path) -> dict:
    image = Image.open(path).convert("RGBA")
    alpha_extrema = image.getchannel("A").getextrema()
    colours = image.getcolors(maxcolors=image.width * image.height) or []
    return {
        "file": path.relative_to(root).as_posix(),
        "width": image.width,
        "height": image.height,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "alphaAllZero": alpha_extrema == (0, 0),
        "distinctColours": len(colours),
    }


def contact_sheet(paths: list[Path], out: Path) -> None:
    rows = (len(paths) + COLUMNS - 1) // COLUMNS
    sheet = Image.new(
        "RGBA", (COLUMNS * TILE, rows * (TILE + LABEL_HEIGHT)), (48, 48, 56, 255)
    )
    draw = ImageDraw.Draw(sheet)
    for index, path in enumerate(paths):
        image = Image.open(path).convert("RGBA")
        image.thumbnail((TILE, TILE))
        x, y = (index % COLUMNS) * TILE, (index // COLUMNS) * (TILE + LABEL_HEIGHT)
        sheet.alpha_composite(
            image, (x + (TILE - image.width) // 2, y + (TILE - image.height) // 2)
        )
        draw.text((x + 2, y + TILE + 2), path.stem[:20], fill=(255, 255, 255, 255))
    sheet.save(out)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dds-dir", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--stats", required=True)
    args = parser.parse_args()
    png_dir = Path(args.out).parent / "png"
    paths = []
    for dds in sorted(Path(args.dds_dir).rglob("*.dds")):
        target = png_dir / dds.relative_to(args.dds_dir).with_suffix(".png")
        target.parent.mkdir(parents=True, exist_ok=True)
        Image.open(dds).convert("RGBA").save(target)
        paths.append(target)
    Path(args.stats).write_text(
        json.dumps([stats(path, png_dir) for path in paths], indent=1)
    )
    contact_sheet(paths, Path(args.out))


if __name__ == "__main__":
    main()
