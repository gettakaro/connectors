"""``icons build --recipes`` with ``model`` layers: the block-model renderer, pixel by pixel, from tiny model documents.

Every document is written by the test next to a few flat-colour PNGs. With the identity ``gui`` transform a block is
seen from the south at 128 pixels per block, so the expected pixels follow from the box's coordinates; with the
inventory transform ``[30, 225, 0]`` at scale 0.625 the top, east (left) and north (right) faces show.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest
from PIL import Image

RED, GREEN, BLUE, YELLOW, WHITE = (
    (255, 0, 0, 255),
    (0, 255, 0, 255),
    (0, 0, 255, 255),
    (255, 255, 0, 255),
    (255, 255, 255, 255),
)
GUI = {"rotation": [30, 225, 0], "translation": [0, 0, 0], "scale": [0.625, 0.625, 0.625]}
FACES = ("down", "up", "north", "south", "west", "east")
CUBE = {"from": [0, 0, 0], "to": [16, 16, 16]}


def cube(texture: str = "t", **extra: Any) -> dict[str, Any]:
    return {**CUBE, "faces": {name: {"texture": texture} for name in FACES}, **extra}


def south(texture: str = "t", *, z: float = 16, box: tuple[float, float] = (0, 16), **face: Any) -> dict[str, Any]:
    """An element showing only its south face."""
    return {
        "from": [box[0], box[0], z - 1],
        "to": [box[1], box[1], z],
        "faces": {"south": {"texture": texture, **face}},
    }


class Workspace:
    def __init__(self, tmp_path: Path, run: Any) -> None:
        self.dir = tmp_path / "recipes"
        self.dir.mkdir()
        self.tmp_path = tmp_path
        self.run = run
        self.builds = 0

    def png(self, name: str, image: Image.Image) -> str:
        path = self.dir / "images" / f"{name}.png"
        path.parent.mkdir(exist_ok=True)
        image.save(path, format="PNG")
        return f"images/{name}.png"

    def solid(self, name: str, colour: tuple[int, int, int, int], size: tuple[int, int] = (16, 16)) -> str:
        return self.png(name, Image.new("RGBA", size, colour))

    def quadrants(self, name: str) -> str:
        image = Image.new("RGBA", (2, 2))
        for position, colour in zip(((0, 0), (1, 0), (0, 1), (1, 1)), (RED, GREEN, BLUE, YELLOW), strict=True):
            image.putpixel(position, colour)
        return self.png(name, image)

    def build(self, model: dict[str, Any], *, ops: list[dict[str, Any]] | None = None) -> tuple[int, Any, Path]:
        (self.dir / "models").mkdir(exist_ok=True)
        (self.dir / "models" / "m.json").write_text(json.dumps({"schemaVersion": 1, **model}))
        layer: dict[str, Any] = {"model": "models/m.json"}
        if ops:
            layer["ops"] = ops
        document = {"schemaVersion": 1, "game": "fixture-model", "recipes": [{"code": "c", "layers": [layer]}]}
        (self.dir / "recipes.json").write_text(json.dumps(document))
        self.builds += 1
        out = self.tmp_path / f"out{self.builds}"
        code, payload, _ = self.run("icons", "build", "--recipes", str(self.dir / "recipes.json"), "--out", str(out))
        return code, payload, out

    def render(self, model: dict[str, Any], *, ops: list[dict[str, Any]] | None = None) -> Image.Image:
        code, payload, out = self.build(model, ops=ops)
        assert code == 0, payload
        pack = out / "pack" / "fixture-model"
        index = json.loads((pack / "index.json").read_text())["icons"]
        with Image.open(pack / index["c"]) as image:
            return image.convert("RGBA")


@pytest.fixture
def ws(tmp_path: Path, run: Any) -> Workspace:
    return Workspace(tmp_path, run)


def model(textures: dict[str, Any], elements: list[dict[str, Any]], **extra: Any) -> dict[str, Any]:
    """A model document; lit from the front (unshaded) unless a test asks for the side lighting."""
    return {
        "textures": {key: {"image": image} for key, image in textures.items()},
        "elements": elements,
        "light": "front",
        **extra,
    }


def shaded(colour: tuple[int, int, int, int], factor: float) -> tuple[int, int, int, int]:
    return (*(int(channel * factor + 0.5) for channel in colour[:3]), colour[3])


def test_a_front_view_cube_fills_the_canvas_with_its_south_face(ws: Workspace) -> None:
    image = ws.render(model({"t": ws.solid("south", YELLOW)}, [cube()]))
    assert image.size == (128, 128)
    assert {image.getpixel((x, y)) for x in (0, 64, 127) for y in (0, 64, 127)} == {YELLOW}


def test_a_small_box_is_centred_at_128_pixels_per_block(ws: Workspace) -> None:
    image = ws.render(
        model(
            {"t": ws.solid("south", YELLOW)},
            [{**CUBE, "from": [4, 4, 4], "to": [12, 12, 12], "faces": {"south": {"texture": "t"}}}],
        )
    )
    assert image.getpixel((32, 32)) == YELLOW and image.getpixel((95, 95)) == YELLOW
    assert image.getpixel((31, 64))[3] == 0 and image.getpixel((96, 64))[3] == 0
    assert image.getbbox() == (32, 32, 96, 96)


def test_the_inventory_transform_shows_top_east_on_the_left_and_north_on_the_right(ws: Workspace) -> None:
    textures = {
        name: ws.solid(name, colour)
        for name, colour in zip(FACES, (RED, GREEN, BLUE, YELLOW, WHITE, (0, 255, 255, 255)), strict=True)
    }
    elements = [{**CUBE, "faces": {name: {"texture": name} for name in FACES}}]
    image = ws.render(model(textures, elements, gui=GUI, light="side"))
    assert image.getpixel((64, 29)) == GREEN  # the up texture
    assert image.getpixel((36, 78)) == shaded((0, 255, 255, 255), 0.6)
    assert image.getpixel((92, 78)) == shaded(BLUE, 0.8)


@pytest.mark.parametrize(
    ("rotation", "top_left"), [(0, RED), (90, BLUE), (180, YELLOW), (270, GREEN)], ids=["0", "90", "180", "270"]
)
def test_face_rotation_turns_the_texture_clockwise(
    ws: Workspace, rotation: int, top_left: tuple[int, int, int, int]
) -> None:
    element = {**CUBE, "faces": {"south": {"texture": "t", "rotation": rotation}}}
    image = ws.render(model({"t": ws.quadrants("quads")}, [element]))
    assert image.getpixel((32, 32)) == top_left


def test_a_mirrored_uv_flips_the_texture(ws: Workspace) -> None:
    element = {**CUBE, "faces": {"south": {"texture": "t", "uv": [16, 0, 0, 16]}}}
    image = ws.render(model({"t": ws.quadrants("quads")}, [element]))
    assert image.getpixel((32, 32)) == GREEN and image.getpixel((96, 32)) == RED


def test_a_missing_uv_defaults_to_the_boxes_own_part_of_the_texture(ws: Workspace) -> None:
    texture = Image.new("RGBA", (16, 16), RED)
    texture.paste(BLUE, (4, 4, 12, 12))
    element = {"from": [4, 4, 4], "to": [12, 12, 12], "faces": {"south": {"texture": "t"}}}
    image = ws.render(model({"t": ws.png("framed", texture)}, [element]))
    assert {image.getpixel((x, y)) for x in (33, 64, 94) for y in (33, 64, 94)} == {BLUE}


def test_an_element_rotation_turns_the_box_and_rescale_restores_its_width(ws: Workspace) -> None:
    turned = {
        "from": [4, 0, 7],
        "to": [12, 16, 8],
        "rotation": {"origin": [8, 8, 8], "axis": "y", "angle": 45},
        "faces": {"south": {"texture": "t"}},
    }
    plain = ws.render(model({"t": ws.solid("s", YELLOW)}, [turned]))
    assert plain.getpixel((64, 64)) == YELLOW and plain.getpixel((84, 64)) == YELLOW
    assert plain.getpixel((89, 64))[3] == 0 and plain.getpixel((38, 64))[3] == 0
    rescaled_element = {**turned, "rotation": {**turned["rotation"], "rescale": True}}
    rescaled = ws.render(model({"t": ws.solid("s", YELLOW)}, [rescaled_element]))
    assert rescaled.getpixel((36, 64)) == YELLOW and rescaled.getpixel((94, 64)) == YELLOW
    assert rescaled.getpixel((28, 64))[3] == 0 and rescaled.getpixel((100, 64))[3] == 0


def test_a_face_turned_away_is_not_drawn(ws: Workspace) -> None:
    element = {**CUBE, "faces": {"north": {"texture": "t"}, "down": {"texture": "t"}}}
    assert ws.render(model({"t": ws.solid("s", RED)}, [element])).getbbox() is None


def test_a_cutout_hole_shows_the_element_behind_it(ws: Workspace) -> None:
    cutout = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    cutout.paste(GREEN, (8, 0, 16, 16))
    elements = [south("far", z=4), south("near", z=12)]
    image = ws.render(model({"far": ws.solid("far", RED), "near": ws.png("cutout", cutout)}, elements))
    assert image.getpixel((32, 64)) == RED and image.getpixel((96, 64)) == GREEN


def test_a_translucent_face_is_composited_over_what_is_behind_it(ws: Workspace) -> None:
    elements = [south("far", z=4), south("near", z=12)]
    textures = {"far": ws.solid("far", RED), "near": ws.solid("near", (0, 0, 255, 128))}
    pixel = ws.render(model(textures, elements)).getpixel((64, 64))
    alpha = 128 / 255
    assert pixel == (int(255 * (1 - alpha) + 0.5), 0, int(255 * alpha + 0.5), 255)


def test_a_coplanar_later_element_is_drawn_on_top(ws: Workspace) -> None:
    textures = {"first": ws.solid("first", RED), "second": ws.solid("second", GREEN)}
    image = ws.render(model(textures, [south("first"), south("second")]))
    assert image.getpixel((64, 64)) == GREEN
    swapped = ws.render(model(textures, [south("second"), south("first")]))
    assert swapped.getpixel((64, 64)) == RED


def test_a_face_tint_multiplies_the_texture(ws: Workspace) -> None:
    image = ws.render(model({"t": ws.solid("white", WHITE)}, [south(tint="#80c040")]))
    assert image.getpixel((64, 64)) == (128, 192, 64, 255)


@pytest.mark.parametrize(
    ("extra", "factor"),
    [
        ({}, 0.6),
        ({"shade": False}, 1.0),
        ({"shadeDirection": "up"}, 1.0),
        ({"shadeDirection": "down"}, 0.5),
    ],
    ids=["shaded", "shade-false", "direction-up", "direction-down"],
)
def test_shading_follows_the_face_or_the_override(ws: Workspace, extra: dict[str, Any], factor: float) -> None:
    image = ws.render(model({"t": ws.solid("grey", (100, 150, 200, 255))}, [cube(**extra)], gui=GUI, light="side"))
    assert image.getpixel((36, 78)) == shaded((100, 150, 200, 255), factor)


def test_front_light_draws_every_face_unshaded(ws: Workspace) -> None:
    image = ws.render(model({"t": ws.solid("grey", (100, 150, 200, 255))}, [cube()], gui=GUI, light="front"))
    assert image.getpixel((36, 78)) == (100, 150, 200, 255) and image.getpixel((92, 78)) == (100, 150, 200, 255)


def test_a_texture_op_applies_inside_the_model(ws: Workspace) -> None:
    strip = Image.new("RGBA", (16, 48))
    for frame, colour in enumerate((RED, GREEN, BLUE)):
        strip.paste(colour, (0, 16 * frame, 16, 16 * frame + 16))
    document = {
        "textures": {
            "t": {"image": ws.png("strip", strip), "ops": [{"op": "frame-crop", "frames": 3, "frame": 1, "gap": 0}]}
        },
        "elements": [cube()],
        "light": "front",
    }
    assert ws.render(document).getpixel((64, 64)) == GREEN


def test_layer_ops_follow_the_render(ws: Workspace) -> None:
    box = {"from": [4, 4, 4], "to": [12, 12, 12], "faces": {"south": {"texture": "t"}}}
    image = ws.render(model({"t": ws.solid("s", YELLOW)}, [box]), ops=[{"op": "trim"}])
    assert image.size == (64, 64)


def test_a_model_layer_composites_with_an_image_layer(ws: Workspace) -> None:
    ws.solid("back", RED, (128, 128))
    box = {"from": [4, 4, 4], "to": [12, 12, 12], "faces": {"south": {"texture": "t"}}}
    ws.render(model({"t": ws.solid("s", YELLOW)}, [box]))
    document = {
        "schemaVersion": 1,
        "game": "fixture-model",
        "recipes": [{"code": "c", "layers": [{"image": "images/back.png"}, {"model": "models/m.json"}]}],
    }
    (ws.dir / "recipes.json").write_text(json.dumps(document))
    out = ws.tmp_path / "out2"
    code, payload, _ = ws.run("icons", "build", "--recipes", str(ws.dir / "recipes.json"), "--out", str(out))
    assert code == 0, payload
    pack = out / "pack" / "fixture-model"
    with Image.open(pack / json.loads((pack / "index.json").read_text())["icons"]["c"]) as image:
        assert image.convert("RGBA").getpixel((64, 64)) == YELLOW and image.convert("RGBA").getpixel((8, 8)) == RED


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda m: m["elements"][0].update(extra=1), "extra"),
        (lambda m: m["elements"][0]["faces"].update(sideways={"texture": "t"}), "sideways"),
        (lambda m: m["elements"][0]["faces"]["south"].update(uv=[0, 0, 17, 16]), "17"),
        (lambda m: m.update(elements=[cube()] * 513), "elements"),
        (
            lambda m: m["elements"][0].update(
                rotation={"origin": [8, 8, 8], "axis": "y", "angle": 90, "rescale": True}
            ),
            "rescale",
        ),
        (lambda m: m["elements"][0]["faces"]["south"].update(texture="nope"), "nope"),
        (lambda m: m["elements"][0]["faces"]["south"].update(rotation=45), "45"),
        (lambda m: m.update(light="dark"), "dark"),
    ],
    ids=["extra-key", "bad-face", "uv-range", "too-many", "rescale-90", "undefined-texture", "face-rotation", "light"],
)
def test_an_invalid_model_document_is_exit_2_naming_the_recipe(ws: Workspace, change: Any, message: str) -> None:
    document = model({"t": ws.solid("s", RED)}, [cube()])
    change(document)
    code, payload, out = ws.build(document)
    assert code == 2 and "'c'" in payload["error"] and message in json.dumps(payload)
    assert not out.exists() or not list(out.rglob("index.json"))


def test_a_missing_model_or_texture_file_is_exit_4(ws: Workspace, run: Any) -> None:
    code, payload, _ = run("icons", "build", "--recipes", str(_missing_model(ws)), "--out", str(ws.tmp_path / "gone"))
    assert code == 4 and "model file not found" in payload["error"]
    document = model({"t": "images/nowhere.png"}, [cube()])
    code, payload, _ = ws.build(document)
    assert code == 4 and "image file not found" in payload["error"]


def _missing_model(ws: Workspace) -> Path:
    document = {
        "schemaVersion": 1,
        "game": "fixture-model",
        "recipes": [{"code": "c", "layers": [{"model": "models/none.json"}]}],
    }
    path = ws.dir / "missing.json"
    path.write_text(json.dumps(document))
    return path
