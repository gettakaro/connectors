"""The block-model renderer: boxes with textured faces, drawn the way an inventory slot draws a block.

A model document (``schemaVersion`` 1) holds ``textures`` (a key to a recipe image with optional ops), ``elements``
(boxes in 1/16 block units with up to six faces each), a ``gui`` transform and a ``light`` mode. ``render_model`` turns
one into a 128x128 RGBA image in pure Python and Pillow: every face is projected, back faces are dropped, each pixel
collects the fragments of the faces covering it, and the fragments are composited far to near. The semantics are
specified in ``docs/icons.md`` ("The block-model renderer").
"""

from __future__ import annotations

import math
from collections.abc import Callable
from typing import Any

from jsonschema import Draft202012Validator
from PIL import Image

from ..exit_codes import UsageError

CANVAS = 128
PIXELS_PER_BLOCK = 128
MAX_ELEMENTS = 512
MAX_TEXTURES = 64
FACES = ("down", "up", "north", "south", "west", "east")
FACE_ORDER = {name: number for number, name in enumerate(FACES)}
SHADE = {"up": 1.0, "down": 0.5, "north": 0.8, "south": 0.8, "east": 0.6, "west": 0.6}
TRANSLATION_LIMIT = 80.0
SCALE_LIMIT = 4.0

Vector = tuple[float, float, float]


def _vector(low: float, high: float) -> dict[str, Any]:
    return {"type": "array", "minItems": 3, "maxItems": 3, "items": {"type": "number", "minimum": low, "maximum": high}}


def model_schema(ops_schema: dict[str, Any]) -> dict[str, Any]:
    """The JSON schema of a model document; ``ops_schema`` is the recipe schema of a layer's ops."""
    face = {
        "type": "object",
        "required": ["texture"],
        "additionalProperties": False,
        "properties": {
            "texture": {"type": "string", "minLength": 1},
            "uv": {
                "type": "array",
                "minItems": 4,
                "maxItems": 4,
                "items": {"type": "number", "minimum": 0, "maximum": 16},
            },
            "rotation": {"enum": [0, 90, 180, 270]},
            "tint": {"type": "string", "pattern": "^#[0-9a-f]{6}$"},
        },
    }
    element = {
        "type": "object",
        "required": ["from", "to", "faces"],
        "additionalProperties": False,
        "properties": {
            "from": _vector(-80, 96),
            "to": _vector(-80, 96),
            "rotation": {
                "type": "object",
                "required": ["origin", "axis", "angle"],
                "additionalProperties": False,
                "properties": {
                    "origin": _vector(-80, 96),
                    "axis": {"enum": ["x", "y", "z"]},
                    "angle": {"type": "number", "minimum": -180, "maximum": 180},
                    "rescale": {"type": "boolean"},
                },
            },
            "shade": {"type": "boolean"},
            "shadeDirection": {"enum": list(FACES)},
            "faces": {
                "type": "object",
                "minProperties": 1,
                "additionalProperties": False,
                "properties": {name: face for name in FACES},
            },
        },
    }
    return {
        "type": "object",
        "required": ["schemaVersion", "textures", "elements"],
        "additionalProperties": False,
        "properties": {
            "schemaVersion": {"const": 1},
            "textures": {
                "type": "object",
                "minProperties": 1,
                "maxProperties": MAX_TEXTURES,
                "additionalProperties": {
                    "type": "object",
                    "required": ["image"],
                    "additionalProperties": False,
                    "properties": {"image": {"type": "string", "minLength": 1}, "ops": ops_schema},
                },
            },
            "elements": {"type": "array", "minItems": 1, "maxItems": MAX_ELEMENTS, "items": element},
            "gui": {
                "type": "object",
                "additionalProperties": False,
                "properties": {
                    "rotation": {"type": "array", "minItems": 3, "maxItems": 3, "items": {"type": "number"}},
                    "translation": {"type": "array", "minItems": 3, "maxItems": 3, "items": {"type": "number"}},
                    "scale": {"type": "array", "minItems": 3, "maxItems": 3, "items": {"type": "number"}},
                },
            },
            "light": {"enum": ["side", "front"]},
        },
    }


def validator(ops_schema: dict[str, Any]) -> Draft202012Validator:
    return Draft202012Validator(model_schema(ops_schema))


def check_document(document: Any, check: Draft202012Validator, *, code: str) -> None:
    """Refuse a model document that breaks the schema, a texture reference it does not define or a 90 degree rescale."""
    errors = sorted(check.iter_errors(document), key=lambda error: list(error.absolute_path))
    problems = [
        f"{'/'.join(str(part) for part in error.absolute_path) or '<root>'}: {error.message[:200]}"
        for error in errors[:20]
    ]
    if not problems:
        textures = set(document["textures"])
        for number, element in enumerate(document["elements"]):
            rotation = element.get("rotation")
            if rotation and rotation.get("rescale") and abs(rotation["angle"]) >= 90:
                problems.append(f"elements/{number}/rotation: rescale needs |angle| below 90")
            for name, face in element["faces"].items():
                if face["texture"] not in textures:
                    problems.append(f"elements/{number}/faces/{name}: texture {face['texture']!r} is not defined")
    if problems:
        raise UsageError(f"recipe {code!r}: the model document is invalid", problems=problems)


def referenced_images(document: dict[str, Any]) -> list[str]:
    return [str(texture["image"]) for texture in document["textures"].values()]


def default_uv(face: str, low: Vector, high: Vector) -> list[float]:
    (x1, _, z1), (x2, _, z2) = low, high
    y1, y2 = low[1], high[1]
    return {
        "down": [x1, 16 - z2, x2, 16 - z1],
        "up": [x1, z1, x2, z2],
        "north": [16 - x2, 16 - y2, 16 - x1, 16 - y1],
        "south": [x1, 16 - y2, x2, 16 - y1],
        "west": [z1, 16 - y2, z2, 16 - y1],
        "east": [16 - z2, 16 - y2, 16 - z1, 16 - y1],
    }[face]


def face_frame(face: str, low: Vector, high: Vector) -> tuple[Vector, Vector, Vector]:
    """Corner ``O`` (the texture's top left seen from outside) and edges ``U`` (u increasing), ``V`` (v increasing)."""
    (x1, y1, z1), (x2, y2, z2) = low, high
    return {
        "north": ((x2, y2, z1), (x1 - x2, 0.0, 0.0), (0.0, y1 - y2, 0.0)),
        "south": ((x1, y2, z2), (x2 - x1, 0.0, 0.0), (0.0, y1 - y2, 0.0)),
        "west": ((x1, y2, z1), (0.0, 0.0, z2 - z1), (0.0, y1 - y2, 0.0)),
        "east": ((x2, y2, z2), (0.0, 0.0, z1 - z2), (0.0, y1 - y2, 0.0)),
        "up": ((x1, y2, z1), (x2 - x1, 0.0, 0.0), (0.0, 0.0, z2 - z1)),
        "down": ((x1, y1, z2), (x2 - x1, 0.0, 0.0), (0.0, 0.0, z1 - z2)),
    }[face]


def _rotate(point: Vector, axis: str, degrees: float) -> Vector:
    sin, cos = math.sin(math.radians(degrees)), math.cos(math.radians(degrees))
    x, y, z = point
    if axis == "x":
        return (x, y * cos - z * sin, y * sin + z * cos)
    if axis == "y":
        return (x * cos + z * sin, y, -x * sin + z * cos)
    return (x * cos - y * sin, x * sin + y * cos, z)


def _element_transform(element: dict[str, Any]) -> Callable[[Vector], Vector]:
    rotation = element.get("rotation")
    if not rotation:
        return lambda point: point
    origin, axis, angle = rotation["origin"], rotation["axis"], float(rotation["angle"])
    factor = 1.0 / math.cos(math.radians(angle)) if rotation.get("rescale") else 1.0
    scale = tuple(1.0 if name == axis else factor for name in "xyz")

    def transform(point: Vector) -> Vector:
        local = (point[0] - origin[0], point[1] - origin[1], point[2] - origin[2])
        turned = _rotate(local, axis, angle)
        return (
            origin[0] + scale[0] * turned[0],
            origin[1] + scale[1] * turned[1],
            origin[2] + scale[2] * turned[2],
        )

    return transform


def _gui_transform(model: dict[str, Any]) -> Callable[[Vector], Vector]:
    """Model units to viewer-space blocks, ``T + Rx Ry Rz (S * (p/16 - 0.5))``: x right, y up, z toward the viewer."""
    gui = model.get("gui") or {}
    rx, ry, rz = (float(value) for value in gui.get("rotation", (0, 0, 0)))
    translation = tuple(
        min(max(float(v), -TRANSLATION_LIMIT), TRANSLATION_LIMIT) / 16 for v in gui.get("translation", (0, 0, 0))
    )
    scale = tuple(min(max(float(v), -SCALE_LIMIT), SCALE_LIMIT) for v in gui.get("scale", (1, 1, 1)))

    def transform(point: Vector) -> Vector:
        local = tuple(scale[i] * (point[i] / 16 - 0.5) for i in range(3))
        turned = _rotate(_rotate(_rotate((local[0], local[1], local[2]), "z", rz), "y", ry), "x", rx)
        return (translation[0] + turned[0], translation[1] + turned[1], translation[2] + turned[2])

    return transform


def _projected_faces(model: dict[str, Any]) -> list[tuple[int, str, dict[str, Any], Vector, Vector, Vector]]:
    """Every face with its screen corner and edges (X, Y in canvas pixels, depth), dropping the faces turned away."""
    to_view = _gui_transform(model)
    faces = []
    for number, element in enumerate(model["elements"]):
        to_model = _element_transform(element)
        low, high = tuple(element["from"]), tuple(element["to"])
        for name, face in element["faces"].items():
            corner, edge_u, edge_v = face_frame(name, low, high)
            points = [
                to_view(to_model(point))
                for point in (
                    corner,
                    (corner[0] + edge_u[0], corner[1] + edge_u[1], corner[2] + edge_u[2]),
                    (corner[0] + edge_v[0], corner[1] + edge_v[1], corner[2] + edge_v[2]),
                )
            ]
            o, u_end, v_end = points
            u = (u_end[0] - o[0], u_end[1] - o[1], u_end[2] - o[2])
            v = (v_end[0] - o[0], v_end[1] - o[1], v_end[2] - o[2])
            facing = v[0] * u[1] - v[1] * u[0]
            if facing <= 0:
                continue
            screen = (
                (CANVAS / 2 + PIXELS_PER_BLOCK * o[0], CANVAS / 2 - PIXELS_PER_BLOCK * o[1], o[2]),
                (PIXELS_PER_BLOCK * u[0], -PIXELS_PER_BLOCK * u[1], u[2]),
                (PIXELS_PER_BLOCK * v[0], -PIXELS_PER_BLOCK * v[1], v[2]),
            )
            faces.append((number, name, face, *screen))
    return faces


def screen_bounds(model: dict[str, Any]) -> tuple[float, float, float, float]:
    """``(min X, min Y, max X, max Y)`` of every element corner on the canvas, before clipping."""
    to_view = _gui_transform(model)
    xs, ys = [], []
    for element in model["elements"]:
        to_model = _element_transform(element)
        low, high = element["from"], element["to"]
        for x in (low[0], high[0]):
            for y in (low[1], high[1]):
                for z in (low[2], high[2]):
                    q = to_view(to_model((x, y, z)))
                    xs.append(CANVAS / 2 + PIXELS_PER_BLOCK * q[0])
                    ys.append(CANVAS / 2 - PIXELS_PER_BLOCK * q[1])
    return min(xs), min(ys), max(xs), max(ys)


def fit_to_canvas(model: dict[str, Any]) -> bool:
    """Shrink and centre ``model`` when its boxes reach beyond the canvas, so nothing is cut off; ``True`` if it did.

    A slot in the game lets such a model overflow it; an icon cannot. The gui scale is multiplied by the factor that
    leaves a one pixel margin and the gui translation moves the bounds' centre to the canvas centre.
    """
    low_x, low_y, high_x, high_y = screen_bounds(model)
    if min(low_x, low_y) >= 0 and max(high_x, high_y) <= CANVAS:
        return False
    factor = (CANVAS - 2) / max(high_x - low_x, high_y - low_y)
    gui = model.setdefault("gui", {})
    gui["scale"] = [round(float(value) * factor, 6) for value in gui.get("scale", (1, 1, 1))]
    low_x, low_y, high_x, high_y = screen_bounds(model)
    translation = [float(value) for value in gui.get("translation", (0, 0, 0))]
    pixels_per_unit = PIXELS_PER_BLOCK / 16
    translation[0] -= ((low_x + high_x) / 2 - CANVAS / 2) / pixels_per_unit
    translation[1] += ((low_y + high_y) / 2 - CANVAS / 2) / pixels_per_unit
    gui["translation"] = [round(value, 6) for value in translation]
    return True


_Fragment = tuple[float, int, int, float, float, float, float]


def _rasterize(
    faces: list[tuple[int, str, dict[str, Any], Vector, Vector, Vector]],
    model: dict[str, Any],
    textures: dict[str, Image.Image],
    fragments: dict[int, list[_Fragment]],
) -> None:
    lit = model.get("light", "side") == "side"
    pixels = {key: image.tobytes() for key, image in textures.items()}
    for number, name, face, corner, edge_u, edge_v in faces:
        element = model["elements"][number]
        det = edge_u[0] * edge_v[1] - edge_u[1] * edge_v[0]
        if abs(det) < 1e-9:
            continue
        texture = textures[face["texture"]]
        access = pixels[face["texture"]]
        width, height = texture.size
        uv = face.get("uv") or default_uv(name, tuple(element["from"]), tuple(element["to"]))
        rotation = face.get("rotation", 0)
        tint = face.get("tint")
        multiply = tuple(int(tint[i : i + 2], 16) for i in (1, 3, 5)) if tint else None
        factor = 1.0
        if lit and element.get("shade", True):
            factor = SHADE[element.get("shadeDirection", name)]
        xs = [corner[0] + a * edge_u[0] + b * edge_v[0] for a in (0, 1) for b in (0, 1)]
        ys = [corner[1] + a * edge_u[1] + b * edge_v[1] for a in (0, 1) for b in (0, 1)]
        x_from, x_to = max(0, math.floor(min(xs) - 0.5)), min(CANVAS - 1, math.ceil(max(xs) + 0.5))
        y_from, y_to = max(0, math.floor(min(ys) - 0.5)), min(CANVAS - 1, math.ceil(max(ys) + 0.5))
        u1, v1, u2, v2 = uv
        order = FACE_ORDER[name]
        for py in range(y_from, y_to + 1):
            dy = py + 0.5 - corner[1]
            for px in range(x_from, x_to + 1):
                dx = px + 0.5 - corner[0]
                s = (dx * edge_v[1] - dy * edge_v[0]) / det
                if not 0.0 <= s < 1.0:
                    continue
                t = (edge_u[0] * dy - edge_u[1] * dx) / det
                if not 0.0 <= t < 1.0:
                    continue
                if rotation == 90:
                    s, t = t, 1 - s
                elif rotation == 180:
                    s, t = 1 - s, 1 - t
                elif rotation == 270:
                    s, t = 1 - t, s
                tx = min(width - 1, max(0, math.floor((u1 + s * (u2 - u1)) * width / 16)))
                ty = min(height - 1, max(0, math.floor((v1 + t * (v2 - v1)) * height / 16)))
                offset = (ty * width + tx) * 4
                red, green, blue, alpha = access[offset], access[offset + 1], access[offset + 2], access[offset + 3]
                if alpha == 0:
                    continue
                if multiply is not None:
                    red, green, blue = red * multiply[0] // 255, green * multiply[1] // 255, blue * multiply[2] // 255
                if factor != 1.0:
                    red, green, blue = int(red * factor + 0.5), int(green * factor + 0.5), int(blue * factor + 0.5)
                depth = corner[2] + s * edge_u[2] + t * edge_v[2]
                fragments.setdefault(py * CANVAS + px, []).append((depth, number, order, red, green, blue, alpha / 255))


def _composite(fragments: list[_Fragment]) -> tuple[int, int, int, int]:
    """Porter-Duff over in straight alpha, the farthest fragment first (ties: element, then face order)."""
    red = green = blue = alpha = 0.0
    for _, _, _, r, g, b, a in sorted(fragments, key=lambda item: item[:3]):
        out_alpha = a + alpha * (1 - a)
        red = (r * a + red * alpha * (1 - a)) / out_alpha
        green = (g * a + green * alpha * (1 - a)) / out_alpha
        blue = (b * a + blue * alpha * (1 - a)) / out_alpha
        alpha = out_alpha
    return tuple(min(255, int(value + 0.5)) for value in (red, green, blue, alpha * 255))  # type: ignore[return-value]


def render_model(model: dict[str, Any], textures: dict[str, Image.Image]) -> Image.Image:
    """The 128x128 RGBA drawing of a validated model; ``textures`` holds every key the faces name, ops applied."""
    fragments: dict[int, list[_Fragment]] = {}
    _rasterize(_projected_faces(model), model, textures, fragments)
    canvas = bytearray(CANVAS * CANVAS * 4)
    for key, items in fragments.items():
        canvas[key * 4 : key * 4 + 4] = bytes(_composite(items))
    return Image.frombytes("RGBA", (CANVAS, CANVAS), bytes(canvas))
