"""Render recipes into an Icon Pack, a coverage report, a contact sheet and a build record.

The entry point is ``build_pack``, which takes a parsed recipe document, so a later source of recipes
(derived from the catalog and game data) feeds the same builder.

Byte reproducibility is the point of the pinned rendering below: the same recipes and images give the
same WebP bytes, so the same object names, so a rebuild publishes nothing new.
"""

from __future__ import annotations

import hashlib
import io
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import PIL
import PIL.features
from jsonschema import Draft202012Validator
from PIL import Image, ImageDraw, ImageFont

from .. import output
from ..exit_codes import ConflictError, UpstreamUnavailable, UsageError
from ..paths import safe_relative
from . import contract

WEBP_SETTINGS: dict[str, Any] = {"lossless": True, "quality": 100, "method": 6, "exact": False}
MEASURED_AGAINST = "game-data"
COVERAGE_NOTE = "Coverage is measured against codes derived from game data, not a live code list."

SHEET_COLUMNS = 8
SHEET_MAX_ICONS = 64
CELL_WIDTH, CELL_HEIGHT = 144, 164
CELL_ICON = 128
LABEL_FONT_SIZE = 11

_CODE = {"type": "string", "minLength": 1, "maxLength": 256, "pattern": r"^[^\x00-\x1f\x7f]+$"}
_KIND = {"enum": ["item", "block"]}

RECIPE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "required": ["schemaVersion", "game", "recipes"],
    "additionalProperties": False,
    "properties": {
        "schemaVersion": {"const": 1},
        "game": {"type": "string", "maxLength": contract.GAME_MAX_LENGTH, "pattern": r"^[a-z0-9][a-z0-9-]*$"},
        "measuredAgainst": {"const": MEASURED_AGAINST},
        "recipes": {
            "type": "array",
            "minItems": 1,
            "items": {
                "type": "object",
                "required": ["code", "layers"],
                "additionalProperties": False,
                "properties": {
                    "code": _CODE,
                    "kind": _KIND,
                    "composite": {"const": "over"},
                    "layers": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "required": ["image"],
                            "additionalProperties": False,
                            "properties": {"image": {"type": "string", "minLength": 1}, "ops": {"type": "array"}},
                        },
                    },
                },
            },
        },
        "unmatched": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["code", "reason"],
                "additionalProperties": False,
                "properties": {
                    "code": _CODE,
                    "kind": _KIND,
                    "reason": {"type": "string", "pattern": r"^[a-z][a-z0-9-]*$"},
                },
            },
        },
    },
}

_VALIDATOR = Draft202012Validator(RECIPE_SCHEMA)


@dataclass(frozen=True)
class BuildResult:
    game: str
    pack_dir: Path
    index_path: Path
    codes: int
    objects: int
    coverage: dict[str, Any]
    reports: dict[str, Path]
    encoder: dict[str, Any]


def load_recipes(path: Path) -> tuple[dict[str, Any], bytes]:
    """The validated recipe document and the exact bytes it was read from."""
    if not path.is_file():
        raise UsageError(f"recipe document not found: {path}")
    raw = path.read_bytes()
    try:
        document = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise UsageError(f"recipe document is not valid JSON: {exc}") from exc
    validate_recipes(document)
    return document, raw


def validate_recipes(document: Any) -> None:
    errors = sorted(_VALIDATOR.iter_errors(document), key=lambda error: list(error.absolute_path))
    if errors:
        raise UsageError(
            "recipe document is invalid",
            problems=[
                f"{'/'.join(str(part) for part in error.absolute_path) or '<root>'}: {error.message}"
                for error in errors
            ],
        )
    codes: set[str] = set()
    for entry in [*document["recipes"], *document.get("unmatched", [])]:
        if entry["code"] in codes:
            raise UsageError(f"code {entry['code']!r} appears more than once in the recipe document")
        codes.add(entry["code"])
    for recipe in document["recipes"]:
        if len(recipe["layers"]) != 1 or any(layer.get("ops") for layer in recipe["layers"]):
            raise UsageError(
                f"recipe {recipe['code']!r}: image operations and layer compositing arrive with the games that need "
                "them; a recipe takes exactly one layer with no ops"
            )


def render_icon(png: Path) -> Image.Image:
    """RGBA pixels with no metadata, longest edge at most ``MAX_EDGE``, never upscaled."""
    with Image.open(png, formats=["PNG"]) as source:
        source.load()
        rgba = source.convert("RGBA")
    image = Image.new("RGBA", rgba.size)
    image.paste(rgba)
    width, height = image.size
    if max(width, height) > contract.MAX_EDGE:
        scale = contract.MAX_EDGE / max(width, height)
        size = (max(1, round(width * scale)), max(1, round(height * scale)))
        image = image.convert("RGBa").resize(size, Image.Resampling.LANCZOS).convert("RGBA")
    return image


def encode_webp(image: Image.Image) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="WEBP", **WEBP_SETTINGS)
    return buffer.getvalue()


def encoder_description() -> dict[str, Any]:
    return {
        "pillow": PIL.__version__,
        "libwebp": PIL.features.version("webp"),
        "settings": dict(WEBP_SETTINGS),
        "maxEdge": contract.MAX_EDGE,
    }


def build_pack(document: dict[str, Any], base_dir: Path, out: Path, *, recipes_sha256: str) -> BuildResult:
    """Render ``document`` (already validated) into ``out``; image paths resolve against ``base_dir``."""
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise UsageError(f"--out {out} exists and is not an empty directory")
    game = document["game"]
    sources = {
        recipe["code"]: _resolve_image(base_dir, recipe["layers"][0]["image"], recipe["code"])
        for recipe in document["recipes"]
    }

    out.mkdir(parents=True, exist_ok=True)
    pack_dir = out / "pack" / game
    pack_dir.mkdir(parents=True)

    rendered: dict[str, Image.Image] = {}
    icons: dict[str, str] = {}
    objects: dict[str, bytes] = {}
    for code, source in sorted(sources.items()):
        output.info(f"render {code} <- {source.name}")
        image = render_icon(source)
        webp = encode_webp(image)
        name = contract.object_name(webp)
        icons[code] = name
        objects.setdefault(name, webp)
        rendered[code] = image

    for name, webp in objects.items():
        (pack_dir / name).write_bytes(webp)
    index_path = pack_dir / contract.INDEX_NAME
    index_path.write_bytes(contract.serialize_index(game, True, icons))

    coverage = _coverage(document, objects=len(objects))
    reports = {
        "coverage": out / "coverage.json",
        "contactSheet": out / "contact-sheet.png",
        "buildRecord": out / "build-record.json",
    }
    reports["coverage"].write_text(json.dumps(coverage, indent=2) + "\n", encoding="utf-8")
    _contact_sheet(rendered).save(reports["contactSheet"], format="PNG")
    encoder = encoder_description()
    build_record = {
        "schemaVersion": 1,
        "game": game,
        "recipesSha256": recipes_sha256,
        "encoder": encoder,
        "codes": len(icons),
        "objects": len(objects),
    }
    reports["buildRecord"].write_text(json.dumps(build_record, indent=2) + "\n", encoding="utf-8")

    problems = contract.check_pack_dir(pack_dir)
    if problems:
        raise ConflictError("the built pack violates the pack contract", problems=problems)
    return BuildResult(
        game=game,
        pack_dir=pack_dir,
        index_path=index_path,
        codes=len(icons),
        objects=len(objects),
        coverage={key: coverage[key] for key in ("total", "matched", "unmatched", "coverage")},
        reports=reports,
        encoder=encoder,
    )


def build_from_file(recipes: Path, out: Path) -> BuildResult:
    document, raw = load_recipes(recipes)
    return build_pack(document, recipes.parent, out, recipes_sha256=hashlib.sha256(raw).hexdigest())


def _resolve_image(base_dir: Path, image: str, code: str) -> Path:
    path = base_dir / safe_relative(image, field=f"recipe {code!r} image")
    if not path.is_file():
        raise UpstreamUnavailable(f"recipe {code!r}: image file not found: {image}", recipe=code, image=image)
    return path


def _coverage(document: dict[str, Any], *, objects: int) -> dict[str, Any]:
    matched = [(entry["code"], entry.get("kind")) for entry in document["recipes"]]
    unmatched = document.get("unmatched", [])
    total = len(matched) + len(unmatched)
    report: dict[str, Any] = {
        "schemaVersion": 1,
        "game": document["game"],
        "measuredAgainst": MEASURED_AGAINST,
        "note": COVERAGE_NOTE,
        "total": total,
        "matched": len(matched),
        "unmatched": len(unmatched),
        "coverage": round(len(matched) / total, 4),
        "objects": objects,
    }
    entries = [*(kind for _, kind in matched), *(entry.get("kind") for entry in unmatched)]
    kinds = sorted({kind for kind in entries if kind})
    if kinds:
        report["byKind"] = {}
        for kind in kinds:
            kind_matched = sum(1 for _, k in matched if k == kind)
            kind_unmatched = sum(1 for entry in unmatched if entry.get("kind") == kind)
            report["byKind"][kind] = {
                "total": kind_matched + kind_unmatched,
                "matched": kind_matched,
                "unmatched": kind_unmatched,
            }
    reasons: dict[str, int] = {}
    for entry in unmatched:
        reasons[entry["reason"]] = reasons.get(entry["reason"], 0) + 1
    report["unmatchedByReason"] = dict(sorted(reasons.items()))
    report["unmatchedCodes"] = [
        {key: entry[key] for key in ("code", "kind", "reason") if key in entry}
        for entry in sorted(unmatched, key=lambda entry: entry["code"])
    ]
    return report


def _sample(codes: list[str]) -> list[str]:
    if len(codes) <= SHEET_MAX_ICONS:
        return codes
    return [codes[index * len(codes) // SHEET_MAX_ICONS] for index in range(SHEET_MAX_ICONS)]


def _checkerboard() -> Image.Image:
    board = Image.new("RGBA", (CELL_ICON, CELL_ICON), (255, 255, 255, 255))
    draw = ImageDraw.Draw(board)
    square = 8
    for row in range(CELL_ICON // square):
        for column in range(CELL_ICON // square):
            if (row + column) % 2:
                draw.rectangle(
                    (column * square, row * square, (column + 1) * square - 1, (row + 1) * square - 1),
                    fill=(204, 204, 204, 255),
                )
    return board


def _fit_label(draw: ImageDraw.ImageDraw, font: ImageFont.ImageFont | ImageFont.FreeTypeFont, text: str) -> str:
    limit = CELL_WIDTH - 4
    if draw.textlength(text, font=font) <= limit:
        return text
    while text and draw.textlength(text + "…", font=font) > limit:
        text = text[:-1]
    return text + "…"


def _contact_sheet(rendered: dict[str, Image.Image]) -> Image.Image:
    codes = _sample(sorted(rendered))
    rows = max(1, -(-len(codes) // SHEET_COLUMNS))
    sheet = Image.new("RGBA", (SHEET_COLUMNS * CELL_WIDTH, rows * CELL_HEIGHT), (245, 245, 245, 255))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=LABEL_FONT_SIZE)
    board = _checkerboard()
    for position, code in enumerate(codes):
        left = (position % SHEET_COLUMNS) * CELL_WIDTH
        top = (position // SHEET_COLUMNS) * CELL_HEIGHT
        cell = board.copy()
        icon = rendered[code]
        cell.alpha_composite(icon, ((CELL_ICON - icon.width) // 2, (CELL_ICON - icon.height) // 2))
        sheet.paste(cell, (left + (CELL_WIDTH - CELL_ICON) // 2, top + 4))
        draw.text((left + 2, top + CELL_ICON + 8), _fit_label(draw, font, code), fill=(30, 30, 30, 255), font=font)
    return sheet
