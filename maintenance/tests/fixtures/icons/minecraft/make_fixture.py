#!/usr/bin/env python3
"""Generate the synthetic Minecraft fixtures: a client jar, a server jar, a version manifest and the report answer.

Nothing here is game data. The item names are ``minecraft:fixture_*`` (plus the registry's ``minecraft:air``), the
models are hand-written boxes and flat items in the shape the mapper reads, the textures are flat colours and the jars
are a few synthetic entries (the server jar is opaque bytes that are never executed).
``python make_fixture.py`` rewrites the committed files; ``test_cli_icons_minecraft.py`` regenerates them into a
temporary directory and requires byte equality, so the JSON, the pictures and the expectations change together.
"""

from __future__ import annotations

import hashlib
import io
import json
import sys
import zipfile
from pathlib import Path
from typing import Any

from PIL import Image

HERE = Path(__file__).parent
VERSION = "26.3"
ASSETS = "assets/minecraft"

# name -> (width, height, colour or per-frame colours); the pictures are flat.
SOLID = {
    "item/fixture_base": (200, 100, 50, 255),
    "item/fixture_liquid": (200, 200, 200, 255),
    "item/fixture_a": (10, 20, 30, 255),
    "item/fixture_b": (200, 210, 220, 255),
    "item/fixture_c": (90, 90, 90, 255),
    "block/fixture_up": (255, 0, 0, 255),
    "block/fixture_down": (0, 255, 0, 255),
    "block/fixture_north": (0, 0, 255, 255),
    "block/fixture_south": (255, 255, 0, 255),
    "block/fixture_east": (0, 255, 255, 255),
    "block/fixture_west": (255, 0, 255, 255),
    "block/fixture_stone": (150, 150, 150, 255),
    "block/fixture_dirt": (120, 80, 40, 255),
    "block/fixture_leaves": (220, 220, 220, 255),
    "block/fixture_pane": (0, 128, 255, 128),
}
ANIMATED = {
    "item/fixture_strip": [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)],
    "item/fixture_wide": [(255, 0, 0, 255), (0, 255, 0, 255)],
    "block/fixture_flow": [(10, 10, 10, 255), (250, 250, 250, 255)],
}
MCMETA = {
    "item/fixture_strip": {"animation": {"frames": [2, 0, 1]}},
    "item/fixture_wide": {"animation": {"width": 8}},
    "block/fixture_flow": {"animation": {}},
}
GUI_BLOCK = {"rotation": [30, 225, 0], "translation": [0, 0, 0], "scale": [0.625, 0.625, 0.625]}
GUI_WALL = {"rotation": [30, 135, 0], "translation": [0, 0, 0], "scale": [0.625, 0.625, 0.625]}
LEATHER_DEFAULT = -6265536  # signed ARGB ints, as the item definitions write them
POTION_DEFAULT = -13083194
LEAVES_CONSTANT = -12012264

# registry items: block ones are also in the block registry
BLOCKS = [
    "fixture_overflow",
    "fixture_block",
    "fixture_wall",
    "fixture_grass",
    "fixture_leaves",
    "fixture_glass",
    "fixture_flow_block",
]
ITEMS = [
    "fixture_overflow",
    "fixture_leather",
    "fixture_potion",
    "fixture_block",
    "fixture_wall",
    "fixture_grass",
    "fixture_leaves",
    "fixture_rescale",
    "fixture_facerot",
    "fixture_shadedir",
    "fixture_front",
    "fixture_glass",
    "fixture_strip_item",
    "fixture_flow_block",
    "fixture_condition",
    "fixture_select_gui",
    "fixture_select_trim",
    "fixture_range",
    "fixture_composite",
    "fixture_special",
    "fixture_empty",
    "fixture_missing_texture",
    "fixture_missing_model",
    "fixture_cycle",
    "fixture_tint_unknown",
    "fixture_bad_key",
    "fixture_wide_anim",
    "fixture_no_item_file",
    "fixture_twin_a",
    "fixture_twin_b",
]


def png(width: int, height: int, pixels: Any) -> bytes:
    image = Image.new("RGBA", (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), pixels(x, y))
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", compress_level=0)
    return buffer.getvalue()


def solid(colour: tuple[int, int, int, int]) -> bytes:
    return png(16, 16, lambda x, y: colour)


def strip(colours: list[tuple[int, int, int, int]]) -> bytes:
    return png(16, 16 * len(colours), lambda x, y: colours[y // 16])


def textures() -> dict[str, bytes]:
    found = {f"textures/{name}.png": solid(colour) for name, colour in SOLID.items()}
    found.update({f"textures/{name}.png": strip(colours) for name, colours in ANIMATED.items()})
    found["textures/item/fixture_overlay.png"] = png(
        16, 16, lambda x, y: (10, 200, 10, 255) if x < 8 and y < 8 else (0, 0, 0, 0)
    )
    found["textures/block/fixture_grass_overlay.png"] = png(
        16, 16, lambda x, y: (200, 200, 200, 255) if y < 8 else (0, 0, 0, 0)
    )
    found["textures/block/fixture_cutout.png"] = png(
        16, 16, lambda x, y: (255, 128, 0, 255) if x >= 8 else (0, 0, 0, 0)
    )
    found["textures/colormap/grass.png"] = png(256, 256, lambda x, y: (x, y, 128, 255))
    return found


def cube(faces: dict[str, str] | None = None, *, extra: dict[str, Any] | None = None) -> dict[str, Any]:
    names = faces or {name: f"#{name}" for name in ("down", "up", "north", "south", "west", "east")}
    return {
        "from": [0, 0, 0],
        "to": [16, 16, 16],
        "faces": {name: {"texture": ref} for name, ref in names.items()},
        **(extra or {}),
    }


def models() -> dict[str, dict[str, Any]]:
    found: dict[str, dict[str, Any]] = {
        "item/generated": {
            "parent": "builtin/generated",
            "display": {"gui": {"rotation": [0, 0, 0], "translation": [0, 0, 0], "scale": [1, 1, 1]}},
        },
        "block/fixture_block_base": {"display": {"gui": GUI_BLOCK}, "gui_light": "side"},
        "block/fixture_cube": {"parent": "minecraft:block/fixture_block_base", "elements": [cube()]},
        "block/fixture_block": {
            "parent": "minecraft:block/fixture_cube",
            "textures": {
                "down": "minecraft:block/fixture_down",
                "up": "minecraft:block/fixture_up",
                "north": "minecraft:block/fixture_north",
                "south": "minecraft:block/fixture_south",
                "west": "minecraft:block/fixture_west",
                "east": "minecraft:block/fixture_east",
            },
        },
        "block/fixture_overflow": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"all": "minecraft:block/fixture_stone"},
            "elements": [
                {
                    "from": [-8, 0, 0],
                    "to": [24, 16, 16],
                    "faces": {name: {"texture": "#all"} for name in ("down", "up", "north", "south", "west", "east")},
                }
            ],
        },
        "block/fixture_wall": {
            "display": {"gui": GUI_WALL},
            "textures": {"wall": "minecraft:block/fixture_stone"},
            "elements": [
                {
                    "from": [4, 0, 4],
                    "to": [12, 16, 12],
                    "__comment": "post",
                    "faces": {
                        "down": {"uv": [4, 4, 12, 12], "texture": "#wall", "cullface": "down"},
                        "up": {"uv": [4, 4, 12, 12], "texture": "#wall"},
                        "north": {"uv": [4, 0, 12, 16], "texture": "#wall"},
                        "south": {"texture": "#wall"},
                        "west": {"texture": "#wall"},
                        "east": {"uv": [4, 0, 12, 16], "texture": "#wall"},
                    },
                },
                {
                    "from": [5, 0, 0],
                    "to": [11, 13, 16],
                    "name": "arms",
                    "faces": {
                        "down": {"texture": "#wall", "cullface": "down"},
                        "up": {"texture": "#wall"},
                        "north": {"texture": "#wall", "cullface": "north"},
                        "south": {"texture": "#wall", "cullface": "south"},
                        "west": {"texture": "#wall"},
                        "east": {"texture": "#wall"},
                    },
                },
            ],
        },
        "block/fixture_grass": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"dirt": "minecraft:block/fixture_dirt", "overlay": "minecraft:block/fixture_grass_overlay"},
            "elements": [
                cube({name: "#dirt" for name in ("down", "up", "north", "south", "west", "east")}),
                {
                    "from": [0, 0, 0],
                    "to": [16, 16, 16],
                    "faces": {
                        name: {"texture": "#overlay", "tintindex": 0}
                        for name in ("up", "north", "south", "west", "east")
                    },
                },
            ],
        },
        "block/fixture_leaves": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"all": "minecraft:block/fixture_leaves"},
            "elements": [
                {
                    "from": [0, 0, 0],
                    "to": [16, 16, 16],
                    "faces": {
                        name: {"texture": "#all", "tintindex": 0}
                        for name in ("down", "up", "north", "south", "west", "east")
                    },
                }
            ],
        },
        "block/fixture_rescale": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"all": "minecraft:block/fixture_stone"},
            "elements": [
                {
                    "from": [2, 0, 2],
                    "to": [14, 12, 14],
                    "rotation": {"origin": [8, 8, 8], "axis": "y", "angle": 45, "rescale": True},
                    "faces": {name: {"texture": "#all"} for name in ("down", "up", "north", "south", "west", "east")},
                }
            ],
        },
        "block/fixture_facerot": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"all": "minecraft:item/fixture_overlay"},
            "elements": [
                {
                    "from": [0, 0, 0],
                    "to": [16, 16, 16],
                    "faces": {
                        "up": {"texture": "#all", "rotation": 90},
                        "north": {"texture": "#all", "rotation": 180},
                        "south": {"texture": "#all", "rotation": 270},
                        "west": {"texture": "#all"},
                        "east": {"texture": "#all"},
                        "down": {"texture": "#all"},
                    },
                }
            ],
        },
        "block/fixture_shadedir": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"all": "minecraft:block/fixture_stone"},
            "elements": [
                cube(
                    {name: "#all" for name in ("down", "up", "north", "south", "west", "east")},
                    extra={"shade_direction_override": "up"},
                )
            ],
        },
        "block/fixture_front": {
            "parent": "minecraft:block/fixture_block_base",
            "gui_light": "front",
            "textures": {"all": "minecraft:block/fixture_stone"},
            "elements": [cube({name: "#all" for name in ("down", "up", "north", "south", "west", "east")})],
        },
        "block/fixture_glass": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {
                "stone": "minecraft:block/fixture_stone",
                "front": {"sprite": "minecraft:block/fixture_cutout", "force_translucent": True},
                "pane": "minecraft:block/fixture_pane",
            },
            "elements": [
                cube({name: "#stone" for name in ("down", "up", "north", "south", "west", "east")}),
                {
                    "from": [0, 0, 0],
                    "to": [16, 16, 16],
                    "faces": {"north": {"texture": "#front"}, "east": {"texture": "#pane"}},
                },
            ],
        },
        "block/fixture_flow_block": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"all": "minecraft:block/fixture_flow"},
            "elements": [cube({name: "#all" for name in ("down", "up", "north", "south", "west", "east")})],
        },
        "item/fixture_leather": {
            "parent": "minecraft:item/generated",
            "textures": {"layer0": "minecraft:item/fixture_base", "layer1": "minecraft:item/fixture_overlay"},
        },
        "item/fixture_potion": {
            "parent": "minecraft:item/generated",
            "textures": {"layer0": "minecraft:item/fixture_liquid", "layer1": "minecraft:item/fixture_overlay"},
        },
        "item/fixture_strip_item": {
            "parent": "minecraft:item/generated",
            "textures": {"layer0": "minecraft:item/fixture_strip"},
        },
        "item/fixture_wide_anim": {
            "parent": "minecraft:item/generated",
            "textures": {"layer0": "minecraft:item/fixture_wide"},
        },
        "item/fixture_a": {"parent": "minecraft:item/generated", "textures": {"layer0": "minecraft:item/fixture_a"}},
        "item/fixture_b": {"parent": "minecraft:item/generated", "textures": {"layer0": "minecraft:item/fixture_b"}},
        "item/fixture_c": {"parent": "minecraft:item/generated", "textures": {"layer0": "minecraft:item/fixture_c"}},
        "item/fixture_missing_texture": {
            "parent": "minecraft:item/generated",
            "textures": {"layer0": "minecraft:item/fixture_nowhere"},
        },
        "item/fixture_cycle_a": {"parent": "minecraft:item/fixture_cycle_b"},
        "item/fixture_cycle_b": {"parent": "minecraft:item/fixture_cycle_a"},
        "item/fixture_tint_unknown": {
            "parent": "minecraft:item/generated",
            "textures": {"layer0": "minecraft:item/fixture_a"},
        },
        "block/fixture_bad_key": {
            "parent": "minecraft:block/fixture_block_base",
            "textures": {"all": "minecraft:block/fixture_stone"},
            "elements": [
                cube(
                    {name: "#all" for name in ("down", "up", "north", "south", "west", "east")},
                    extra={"fixture_extra": 1},
                )
            ],
        },
    }
    return found


def model_item(model: str, **extra: Any) -> dict[str, Any]:
    return {"model": {"type": "minecraft:model", "model": model, **extra}}


def items() -> dict[str, dict[str, Any]]:
    flat = lambda name: {"type": "minecraft:model", "model": f"minecraft:item/{name}"}  # noqa: E731
    return {
        "fixture_leather": model_item(
            "minecraft:item/fixture_leather", tints=[{"type": "minecraft:dye", "default": LEATHER_DEFAULT}]
        ),
        "fixture_potion": model_item(
            "minecraft:item/fixture_potion", tints=[{"type": "minecraft:potion", "default": POTION_DEFAULT}]
        ),
        "fixture_overflow": model_item("minecraft:block/fixture_overflow"),
        "fixture_block": model_item("minecraft:block/fixture_block"),
        "fixture_wall": model_item("minecraft:block/fixture_wall"),
        "fixture_grass": model_item(
            "minecraft:block/fixture_grass", tints=[{"type": "minecraft:grass", "temperature": 0.5, "downfall": 1.0}]
        ),
        "fixture_leaves": model_item(
            "minecraft:block/fixture_leaves", tints=[{"type": "minecraft:constant", "value": LEAVES_CONSTANT}]
        ),
        "fixture_rescale": model_item("minecraft:block/fixture_rescale"),
        "fixture_facerot": model_item("minecraft:block/fixture_facerot"),
        "fixture_shadedir": model_item("minecraft:block/fixture_shadedir"),
        "fixture_front": model_item("minecraft:block/fixture_front"),
        "fixture_glass": model_item("minecraft:block/fixture_glass"),
        "fixture_strip_item": model_item("minecraft:item/fixture_strip_item"),
        "fixture_flow_block": model_item("minecraft:block/fixture_flow_block"),
        "fixture_condition": {
            "model": {
                "type": "minecraft:condition",
                "property": "minecraft:using_item",
                "on_true": flat("fixture_b"),
                "on_false": flat("fixture_a"),
            }
        },
        "fixture_select_gui": {
            "model": {
                "type": "minecraft:select",
                "property": "minecraft:display_context",
                "cases": [{"when": ["gui", "ground"], "model": flat("fixture_c")}],
                "fallback": flat("fixture_a"),
            }
        },
        "fixture_select_trim": {
            "model": {
                "type": "minecraft:select",
                "property": "minecraft:trim_material",
                "cases": [{"when": "minecraft:fixture", "model": flat("fixture_b")}],
                "fallback": flat("fixture_c"),
            }
        },
        "fixture_range": {
            "model": {
                "type": "minecraft:range_dispatch",
                "property": "minecraft:time",
                "entries": [
                    {"threshold": 0.5, "model": flat("fixture_b")},
                    {"threshold": 0.0, "model": flat("fixture_a")},
                ],
            }
        },
        "fixture_composite": {
            "model": {"type": "minecraft:composite", "models": [flat("fixture_b"), flat("fixture_a")]}
        },
        "fixture_special": {
            "model": {
                "type": "minecraft:special",
                "base": "minecraft:item/fixture_a",
                "model": {"type": "minecraft:fixture_entity"},
            }
        },
        "fixture_empty": {"model": {"type": "minecraft:empty"}},
        "fixture_missing_texture": model_item("minecraft:item/fixture_missing_texture"),
        "fixture_missing_model": model_item("minecraft:item/fixture_nowhere"),
        "fixture_cycle": model_item("minecraft:item/fixture_cycle_a"),
        "fixture_tint_unknown": model_item(
            "minecraft:item/fixture_tint_unknown", tints=[{"type": "minecraft:fixture_unknown"}]
        ),
        "fixture_bad_key": model_item("minecraft:block/fixture_bad_key"),
        "fixture_wide_anim": model_item("minecraft:item/fixture_wide_anim"),
        "fixture_twin_a": model_item("minecraft:item/fixture_a"),
        "fixture_twin_b": model_item("minecraft:item/fixture_a"),
        "fixture_not_registered": model_item("minecraft:item/fixture_a"),
        "air": model_item("minecraft:item/fixture_a"),
    }


def dumps(document: Any) -> bytes:
    return (json.dumps(document, indent=1, sort_keys=True) + "\n").encode("utf-8")


def jar(entries: dict[str, bytes]) -> bytes:
    """A reproducible ZIP_STORED archive: sorted entries, the DOS epoch and fixed attributes."""
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_STORED) as archive:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = 0o644 << 16
            archive.writestr(info, entries[name])
    return buffer.getvalue()


def client_entries() -> dict[str, bytes]:
    entries = {f"{ASSETS}/{name}": data for name, data in textures().items()}
    for name, meta in MCMETA.items():
        entries[f"{ASSETS}/textures/{name}.png.mcmeta"] = dumps(meta)
    for name, document in models().items():
        entries[f"{ASSETS}/models/{name}.json"] = dumps(document)
    for name, document in items().items():
        entries[f"{ASSETS}/items/{name}.json"] = dumps(document)
    return entries


def registries() -> dict[str, Any]:
    codes = ["minecraft:air", *(f"minecraft:{name}" for name in ITEMS)]
    return {
        "minecraft:item": {
            "default": "minecraft:air",
            "entries": {code: {"protocol_id": number} for number, code in enumerate(codes)},
            "protocol_id": 7,
        },
        "minecraft:block": {
            "default": "minecraft:air",
            "entries": {f"minecraft:{name}": {"protocol_id": number} for number, name in enumerate(["air", *BLOCKS])},
            "protocol_id": 1,
        },
    }


def files() -> dict[str, bytes]:
    client = jar(client_entries())
    server = b"synthetic server jar: never executed\n"
    client_sha1, server_sha1 = hashlib.sha1(client).hexdigest(), hashlib.sha1(server).hexdigest()
    version = {
        "id": VERSION,
        "downloads": {
            "client": {"sha1": client_sha1, "size": len(client)},
            "server": {"sha1": server_sha1, "size": len(server)},
        },
    }
    registry = dumps(registries())
    return {
        "public/client.jar": client,
        "public/server.jar": server,
        "public/version.json": dumps(version),
        "decoder/index.json": dumps({"datagen-report:registries.json": {"file": "json/registries.json"}}),
        "decoder/json/registries.json": registry,
    }


def write(root: Path) -> None:
    for name, data in files().items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


if __name__ == "__main__":
    write(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE)
