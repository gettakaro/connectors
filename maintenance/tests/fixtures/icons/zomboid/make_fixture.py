#!/usr/bin/env python3
"""Regenerate the synthetic Project Zomboid texture packs under ``client/media/texturepacks``.

Run by hand (``python make_fixture.py``); a test regenerates the packs into a temporary directory and
requires them to match the committed bytes, so the fixture can always be reproduced. Nothing here is
game art: every texture is a solid, uniquely coloured rectangle, so a test can tell which pack and
entry an icon came from by its colour.
"""

from __future__ import annotations

import io
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).parent
V1_TERMINATOR = b"\xef\xbe\xad\xde"


@dataclass(frozen=True)
class Texture:
    """An entry: ``(w, h)`` is the rectangle cut from the page, ``(ox, oy)`` its place in the ``fw x fh`` frame.

    ``inset`` leaves a transparent margin of that many pixels inside the rectangle, around the coloured shape.
    """

    name: str
    colour: tuple[int, int, int]
    x: int
    y: int
    w: int
    h: int
    ox: int
    oy: int
    fw: int
    fh: int
    inset: tuple[int, int] = (0, 0)


@dataclass(frozen=True)
class PackPage:
    name: str
    size: tuple[int, int]
    textures: list[Texture] = field(default_factory=list)
    mask: int = 0


def page_png(page: PackPage) -> bytes:
    image = Image.new("RGBA", page.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    for texture in page.textures:
        left, top = texture.x + texture.inset[0], texture.y + texture.inset[1]
        right = texture.x + texture.w - 1 - texture.inset[0]
        bottom = texture.y + texture.h - 1 - texture.inset[1]
        draw.rectangle((left, top, right, bottom), fill=(*texture.colour, 255))
    buffer = io.BytesIO()
    # Stored (uncompressed) deflate blocks: the bytes do not depend on the zlib build.
    image.save(buffer, format="PNG", compress_level=0, optimize=False)
    return buffer.getvalue()


def _string(value: str) -> bytes:
    raw = value.encode("latin-1")
    return struct.pack("<i", len(raw)) + raw


def _header(page: PackPage) -> bytes:
    out = _string(page.name) + struct.pack("<ii", len(page.textures), page.mask)
    for t in page.textures:
        out += _string(t.name) + struct.pack("<8i", t.x, t.y, t.w, t.h, t.ox, t.oy, t.fw, t.fh)
    return out


def write_pack(path: Path, layout: str, pages: list[PackPage], *, v2_version: int = 1) -> None:
    """A v1 pack (PNG then 0xDEADBEEF) or a v2 pack (``PZPK``, version, PNG preceded by its length)."""
    out = b"PZPK" + struct.pack("<i", v2_version) if layout == "v2" else b""
    out += struct.pack("<i", len(pages))
    for page in pages:
        png = page_png(page)
        out += _header(page)
        out += struct.pack("<i", len(png)) + png if layout == "v2" else png + V1_TERMINATOR
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)


RED, YELLOW, BLUE, GREEN, CYAN = (220, 30, 30), (240, 220, 20), (30, 60, 230), (20, 200, 60), (20, 200, 200)
MAGENTA, ORANGE, GREY, PURPLE, BROWN = (255, 0, 255), (255, 128, 0), (120, 120, 120), (128, 0, 255), (120, 70, 20)
PINK = (255, 150, 180)


def item(name: str, colour: tuple[int, int, int], x: int, y: int) -> Texture:
    """A 20x24 crop sitting at (6, 4) inside a 32x32 frame."""
    return Texture(name, colour, x, y, 20, 24, 6, 4, 32, 32)


PACKS: dict[str, tuple[str, list[PackPage]]] = {
    "UI2.pack": (
        "v2",
        [
            PackPage(
                "UI20",
                (64, 64),
                [
                    item("Item_Axe", RED, 0, 0),
                    item("Item_GlassesBlack", BLUE, 24, 0),
                    item("Item_GlassesBlue", GREEN, 0, 28),
                ],
                mask=1,
            ),
            PackPage("UI21", (64, 64), [item("Item_Wineskin", CYAN, 0, 0)], mask=1),
        ],
    ),
    "UI.pack": (
        "v1",
        [
            PackPage("itemThuz0", (64, 64), [item("Item_Axe", YELLOW, 0, 0), item("Item_Bandage", MAGENTA, 24, 0)]),
            # Item_Bandage repeats: the first entry of a pack wins.
            PackPage("itemThuz1", (64, 64), [item("Item_Bandage", PINK, 0, 0), item("Item_Hammer", GREY, 24, 0)]),
        ],
    ),
    "IconsMoveables.pack": (
        "v1",
        [PackPage("IconsMovables0", (32, 32), [Texture("Item_Moveable_floorrug", BROWN, 0, 0, 24, 12, 0, 6, 24, 24)])],
    ),
    "Tiles2x.pack": (
        "v2",
        [
            PackPage(
                "tiles2x0",
                (128, 64),
                [
                    # A 40x50 cut with a transparent margin: its alpha bounding box is 24x30, inside a 64x128 frame.
                    Texture("crafting_ore_01_9", ORANGE, 0, 0, 40, 50, 12, 60, 64, 128, inset=(8, 10)),
                    Texture("location_shop_accessories_01_8", PURPLE, 44, 0, 30, 40, 5, 70, 64, 128, inset=(3, 4)),
                ],
            ),
        ],
    ),
    "Tiles2x.floor.pack": (
        "v2",
        [PackPage("floors0", (64, 64), [Texture("floors_fixture_01_0", GREEN, 0, 0, 64, 32, 0, 0, 64, 32)])],
    ),
}


def generate(directory: Path) -> None:
    for name, (layout, pages) in PACKS.items():
        write_pack(directory / name, layout, pages)


if __name__ == "__main__":
    generate(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "client" / "media" / "texturepacks")
