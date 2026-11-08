#!/usr/bin/env python3
"""Generate the synthetic Valheim fixtures: the depot tree and the decoder stand-in's recorded outputs.

Nothing here is game data: the bundle bytes are a few synthetic bytes, the CAB, prefab and sprite names are
invented, and the atlas is a 64x48 grid of uniquely coloured 8x8 cells with one asymmetric marker pixel each.
``python make_fixture.py`` rewrites the committed files; ``test_cli_icons_valheim.py`` regenerates them into a
temporary directory and requires byte equality, so the JSON and the pictures can only change together.

The expected pixels of a sprite are computed by ``expected_image`` from UnityPy's own bottom-up formulation
(crop in Unity texel space, transpose, flip last). It deliberately shares no code with ``ops.atlas_cut``.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from PIL import Image

HERE = Path(__file__).parent
BUNDLES_DIR = "valheim_server_Data/StreamingAssets/SoftRef/Bundles"
MANIFEST = "7440000000000000746"
DEPOT = "896661"

BUNDLES = {
    "aaaa0001": "cab-a",
    "aaaa0002": "cab-b",
    "aaaa0003": "cab-c",
    "aaaa0004": "cab-d",
    "aaaa0005": "cab-e",
}
DECOYS = {
    "valheim_server_Data/StreamingAssets/SoftRef/manifest": b"fixture decoy manifest\n",
    f"{BUNDLES_DIR}/sub/0abc": b"fixture decoy nested bundle\n",
    f"{BUNDLES_DIR}/README.txt": b"fixture decoy readme\n",
    "valheim_server_Data/resources.assets": b"fixture decoy assets\n",
}

ATLAS = ("cab-d", 400)
ATLAS_SIZE = (64, 48)
CELL = 8
ATLAS_ALPHA = ("cab-d", 401)
BAD_TEXTURE = ("cab-d", 402)
SCENE_OBJECT = ("cab-a", 1)
SCENE_GAMEOBJECT = ("cab-a", 2)
SPRITE_ATLAS = ("cab-e", 50)


def bundle_path(name: str) -> str:
    return f"{BUNDLES_DIR}/{name}"


def cell_color(column: int, row: int) -> tuple[int, int, int, int]:
    return (16 + 30 * column, 16 + 36 * row, 16 + ((column * 5 + row * 11) % 8) * 28, 255)


def atlas_image() -> Image.Image:
    """Top row first. Every cell is one colour except a white marker at local (1, 2)."""
    image = Image.new("RGBA", ATLAS_SIZE)
    for row in range(ATLAS_SIZE[1] // CELL):
        for column in range(ATLAS_SIZE[0] // CELL):
            for y in range(CELL):
                for x in range(CELL):
                    colour = (255, 255, 255, 255) if (x, y) == (1, 2) else cell_color(column, row)
                    image.putpixel((column * CELL + x, row * CELL + y), colour)
    return image


def unity_cut(atlas: Image.Image, rect: list[float], rotation: str) -> Image.Image:
    """UnityPy's ``SpriteHelper`` for a rectangle-packed sprite: bottom-up crop, transpose, flip last."""
    bottom_up = atlas.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    x, y, width, height = rect
    cut = bottom_up.crop((round(x), round(y), round(x + width), round(y + height)))
    if rotation == "flip-horizontal":
        cut = cut.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    elif rotation == "flip-vertical":
        cut = cut.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    elif rotation == "rotate-180":
        cut = cut.transpose(Image.Transpose.ROTATE_180)
    elif rotation == "rotate-90":
        cut = cut.transpose(Image.Transpose.ROTATE_270)
    return cut


def expected_image(sprite: dict[str, Any]) -> Image.Image:
    """The icon the game draws for a sprite: UnityPy's cut, in the sprite's frame when it is trimmed."""
    cut = unity_cut(atlas_image(), sprite["textureRect"], sprite["rotation"])
    width, height = (round(v) for v in sprite["rect"][2:])
    if [cut.width, cut.height] == [width, height] and not any(sprite["textureRectOffset"]):
        return cut.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    frame = Image.new("RGBA", (width, height))
    frame.paste(cut, (round(sprite["textureRectOffset"][0]), round(sprite["textureRectOffset"][1])))
    return frame.transpose(Image.Transpose.FLIP_TOP_BOTTOM)


def _sprite(
    name: str,
    texture_rect: list[float],
    *,
    rotation: str = "none",
    rect: list[float] | None = None,
    offset: list[float] | None = None,
    mode: str = "rectangle",
    alpha: tuple[str, int] | None = None,
    atlas: tuple[str, int] | None = None,
    texture: tuple[str, int] = ATLAS,
) -> dict[str, Any]:
    return {
        "name": name,
        "rect": rect or [0, 0, texture_rect[2], texture_rect[3]],
        "textureRect": texture_rect,
        "textureRectOffset": offset or [0, 0],
        "packed": True,
        "packingMode": mode,
        "rotation": rotation,
        "texture": texture,
        "alpha": alpha,
        "atlas": atlas,
    }


# Sprites live in cab-c at these path ids. Geometry is in Unity texels, origin bottom-left of the 64x48 atlas.
SPRITES: dict[int, dict[str, Any]] = {
    101: _sprite("plain", [16, 8, 8, 8]),
    102: _sprite("variant-a", [0, 40, 8, 8]),
    103: _sprite("variant-b", [8, 40, 8, 8]),
    104: _sprite("variant-c", [16, 40, 8, 8]),
    105: _sprite("copy-one", [24, 24, 8, 8]),
    106: _sprite("copy-two", [32, 24, 8, 8]),
    107: _sprite("ghost", [40, 24, 8, 8]),
    108: _sprite("shared", [48, 16, 8, 8]),
    109: _sprite("round-down", [8.25, 8.25, 8, 8]),
    110: _sprite("round-up", [7.75, 7.75, 8, 8]),
    111: _sprite("half-even-low", [8.5, 8.5, 8, 8]),
    112: _sprite("half-even-high", [9.5, 9.5, 8, 8]),
    113: _sprite("flip-h", [0, 16, 16, 8], rotation="flip-horizontal"),
    114: _sprite("flip-v", [16, 16, 16, 8], rotation="flip-vertical"),
    115: _sprite("turn-180", [32, 0, 8, 16], rotation="rotate-180"),
    116: _sprite("turn-90", [24, 8, 16, 8], rotation="rotate-90", rect=[0, 0, 8, 16]),
    117: _sprite("trimmed", [40, 0, 8, 8], rect=[0, 0, 16, 16], offset=[4, 2]),
    118: _sprite("via-atlas", [48, 32, 8, 8], atlas=SPRITE_ATLAS),
    119: _sprite("tight", [0, 0, 8, 8], mode="tight"),
    120: _sprite("alpha", [8, 0, 8, 8], alpha=ATLAS_ALPHA),
    121: _sprite("bad-format", [16, 0, 8, 8], texture=BAD_TEXTURE),
}
LOST_SPRITE = ("cab-x", 1)
UNRESOLVED_PREFAB = ("cab-y", 5)

# (prefab name, gameobject ref, itemdrop ref, [icon sprite refs]); None for the itemdrop means a non-item prefab.
ITEMS: list[tuple[str, tuple[str, int], tuple[str, int] | None, list[tuple[str, int] | None]]] = [
    ("SynthPlain", ("cab-b", 10), ("cab-b", 510), [("cab-c", 101)]),
    ("SynthTri", ("cab-b", 11), ("cab-b", 511), [("cab-c", 102), ("cab-c", 103), ("cab-c", 104)]),
    ("SynthDup", ("cab-b", 12), ("cab-b", 512), [("cab-c", 105)]),
    ("SynthRock", ("cab-b", 13), None, []),
    ("SynthTwinA", ("cab-b", 14), ("cab-b", 514), [("cab-c", 108)]),
    ("SynthTwinB", ("cab-b", 15), ("cab-b", 515), [("cab-c", 108)]),
    ("SynthRoundDown", ("cab-b", 16), ("cab-b", 516), [("cab-c", 109)]),
    ("SynthRoundUp", ("cab-b", 17), ("cab-b", 517), [("cab-c", 110)]),
    ("SynthHalfEvenLow", ("cab-b", 18), ("cab-b", 518), [("cab-c", 111)]),
    ("SynthHalfEvenHigh", ("cab-b", 19), ("cab-b", 519), [("cab-c", 112)]),
    ("SynthFlipH", ("cab-b", 20), ("cab-b", 520), [("cab-c", 113)]),
    ("SynthFlipV", ("cab-b", 21), ("cab-b", 521), [("cab-c", 114)]),
    ("SynthTurn180", ("cab-b", 22), ("cab-b", 522), [("cab-c", 115)]),
    ("SynthTurn90", ("cab-b", 23), ("cab-b", 523), [("cab-c", 116)]),
    ("SynthTrimmed", ("cab-b", 24), ("cab-b", 524), [("cab-c", 117)]),
    ("SynthViaAtlas", ("cab-b", 25), ("cab-b", 525), [("cab-c", 118)]),
    ("SynthTight", ("cab-b", 26), ("cab-b", 526), [("cab-c", 119)]),
    ("SynthAlpha", ("cab-b", 27), ("cab-b", 527), [("cab-c", 120)]),
    ("SynthBadFormat", ("cab-b", 28), ("cab-b", 528), [("cab-c", 121)]),
    ("SynthLost", ("cab-b", 29), ("cab-b", 529), [LOST_SPRITE]),
    ("SynthBlank", ("cab-b", 30), ("cab-b", 530), []),
    ("SynthHollow", ("cab-b", 31), ("cab-b", 531), [None]),
]
# The second prefab copy named SynthDup (another bundle, listed later in ZNetScene) and an unreferenced namesake.
DUP_COPY = ("SynthDup", ("cab-c", 900), ("cab-c", 901), [("cab-c", 106)])
UNREFERENCED_NAMESAKE = ("SynthPlain", ("cab-d", 300), ("cab-d", 301), [("cab-c", 107)])


def ref(value: tuple[str, int] | None) -> dict[str, Any] | None:
    return None if value is None else {"cab": value[0], "pathId": value[1]}


def _bundle_of(cab: str) -> str:
    return next(bundle_path(name) for name, held in BUNDLES.items() if held == cab)


def znetscene_prefabs() -> list[dict[str, Any] | None]:
    refs: list[tuple[str, int] | None] = [prefab[1] for prefab in ITEMS[:4]]
    refs.append(None)
    refs += [prefab[1] for prefab in ITEMS[4:]]
    refs.append(DUP_COPY[1])
    refs.append(UNRESOLVED_PREFAB)
    return [ref(value) for value in refs]


def item_drop_dump() -> list[dict[str, Any]]:
    dump = []
    for name, game_object, drop, icons in [*ITEMS, DUP_COPY, UNREFERENCED_NAMESAKE]:
        if drop is None:
            continue
        dump.append(
            {
                "bundle": _bundle_of(drop[0]),
                "object": ref(drop),
                "gameObject": {**(ref(game_object) or {}), "name": name},
                "fields": {"m_itemData.m_shared.m_icons": [ref(icon) for icon in icons]},
            }
        )
    return sorted(dump, key=lambda entry: (entry["bundle"], entry["object"]["pathId"]))


def znetscene_dump() -> list[dict[str, Any]]:
    return [
        {
            "bundle": _bundle_of(SCENE_OBJECT[0]),
            "object": ref(SCENE_OBJECT),
            "gameObject": {**(ref(SCENE_GAMEOBJECT) or {}), "name": "SynthScene"},
            "fields": {"m_prefabs": znetscene_prefabs()},
        }
    ]


def bundle_index() -> list[dict[str, Any]]:
    return [
        {"file": bundle_path(name), "unityVersion": "0000.0.0f0", "cabs": [cab]}
        for name, cab in sorted(BUNDLES.items())
    ]


def sprite_meta(path_id: int) -> dict[str, Any]:
    sprite = SPRITES[path_id]
    return {
        "name": sprite["name"],
        "rect": sprite["rect"],
        "textureRect": sprite["textureRect"],
        "textureRectOffset": sprite["textureRectOffset"],
        "packed": sprite["packed"],
        "packingMode": sprite["packingMode"],
        "packingRotation": sprite["rotation"],
        "texture": ref(sprite["texture"]),
        "alphaTexture": ref(sprite["alpha"]),
        "atlas": ref(sprite["atlas"]),
    }


def _json(value: Any) -> bytes:
    return (json.dumps(value, indent=2) + "\n").encode("utf-8")


def _png(image: Image.Image, path: Path) -> bytes:
    image.save(path, format="PNG", compress_level=0)
    return path.read_bytes()


def decoder_files(scratch: Path) -> dict[str, bytes]:
    """Every file under ``decoder/`` by relative path; ``scratch`` is where PNG bytes are rendered."""
    scratch.mkdir(parents=True, exist_ok=True)
    files: dict[str, bytes] = {}
    index: dict[str, Any] = {}

    def add_json(key: str, name: str, value: Any, meta: dict[str, Any] | None = None) -> None:
        files[f"json/{name}.json"] = _json(value)
        index[key] = {"file": f"json/{name}.json", **({"meta": meta} if meta else {})}

    add_json("bundle-index", "bundle-index", bundle_index())
    add_json("monobehaviours:ZNetScene", "znetscene", znetscene_dump())
    add_json("monobehaviours:ItemDrop", "itemdrop", item_drop_dump())
    atlas = atlas_image()
    for path_id in SPRITES:
        add_json(f"sprite:cab-c:{path_id}", f"sprite-{path_id}", sprite_meta(path_id))
    files["png/atlas.png"] = _png(atlas, scratch / "atlas.png")
    index["texture:cab-d:400"] = {
        "file": "png/atlas.png",
        "meta": {"width": ATLAS_SIZE[0], "height": ATLAS_SIZE[1], "format": "RGBA32", "name": "SynthAtlas"},
    }
    index["texture:cab-d:402"] = {
        "error": "texture format SYNTH_UNSUPPORTED is not decodable",
        "errorKind": "unsupported-format",
    }
    for path_id, sprite in SPRITES.items():
        if sprite["packingMode"] != "rectangle" or sprite["alpha"] or sprite["texture"] != ATLAS:
            continue
        cut = unity_cut(atlas, sprite["textureRect"], sprite["rotation"]).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        name = f"reference-{path_id}"
        files[f"png/{name}.png"] = _png(cut, scratch / f"{name}.png")
        index[f"sprite-image:cab-c:{path_id}"] = {
            "file": f"png/{name}.png",
            "meta": {"width": cut.width, "height": cut.height},
        }
    files["index.json"] = _json(index)
    return files


def depot_files() -> dict[str, bytes]:
    root = f"depots/{DEPOT}/{MANIFEST}/tree"
    files = {f"{root}/{bundle_path(name)}": f"fixture bundle {name}\n".encode() for name in BUNDLES}
    files.update({f"{root}/{name}": payload for name, payload in DECOYS.items()})
    files["depots/head.json"] = _json({DEPOT: MANIFEST})
    return files


def all_files(scratch: Path) -> dict[str, bytes]:
    files = {f"decoder/{name}": payload for name, payload in decoder_files(scratch).items()}
    files.update(depot_files())
    return files


def write(root: Path, scratch: Path) -> None:
    for name, payload in all_files(scratch).items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)


if __name__ == "__main__":
    import tempfile

    with tempfile.TemporaryDirectory() as temporary:
        write(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE, Path(temporary))
