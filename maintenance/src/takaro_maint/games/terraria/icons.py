"""Terraria item icons: codes from the connector's item catalogue, pixels from the Windows client's XNB textures.

The connector's ``listItems`` returns ``TERRARIA_ITEM_CATALOG`` unchanged, so the codes in that file are the codes
the connector reports. Item ``N`` is drawn from ``Content/Images/Item_N.xnb``. Animated items are vertical strips: the
game cuts them with ``Texture2D.Frame(1, frames, 0, frame)`` less two rows. How many frames an item has is read from
the game's own item-animation registration (``Main.RegisterItemAnimation``) in the decompiled assembly, never guessed
from a texture's height. The decoders are generic; everything that joins them is here.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from PIL import Image, ImageChops

from ...exit_codes import BuildFailed
from ...icons.mappers import MapContext, MapResult
from ...icons.xnb_session import Missing, Texture, Unsupported
from ...paths import safe_relative

MAPPER_VERSION = 1
CATALOGUE = "games/terraria/bridge/src/terraria/itemCatalog.ts"
ASSEMBLY_FILE = "Terraria.exe"
REGISTER_MEMBER = "Terraria.Main::RegisterItemAnimation"
SETS_TYPE = "Terraria.ID.ItemID+Sets"
TEXTURE_FILE = "Content/Images/Item_{code}.xnb"
ITEM_FILE = re.compile(r"^Content/Images/Item_([0-9]+)\.xnb$")

GAP = 2
FOOD_FRAMES = 3
FRAME_OVERRIDES = {"5644": 9}
OVERRIDE_TYPE = "DrawAnimationScryingOrb"

CODES_FROM = (
    "Item codes from the Terraria connector's static item catalogue ({catalogue}, sha256 {sha256}), which listItems "
    "returns verbatim. Not measured against a live code list from a staging server (#811 skipped)."
)

_CATALOGUE_START = "TERRARIA_ITEM_CATALOG: readonly TerrariaItemCatalogEntry[] = "
_CATALOGUE_END = "] as const;"
_CODE = re.compile(r"^[0-9]+$")
_FAKE_CHEST = re.compile(r"^Fake (.+) Chest$")

_REGISTER = re.compile(r"RegisterItemAnimation\(")
_REGISTER_DEFINITION = re.compile(r"\bvoid\s+RegisterItemAnimation\(")
_CONSTANT = re.compile(
    r"RegisterItemAnimation\(\s*(\d+)\s*,\s*new DrawAnimationVertical\(\s*(\d+|int\.MaxValue)\s*,\s*(\d+)\s*"
    r"(?:,\s*(?:true|false)\s*)?\)"
)
_FOOD_LOOP = re.compile(
    r"IsFood\[(\w+)\]\s*\)\s*\{\s*RegisterItemAnimation\(\s*\1\s*,\s*new DrawAnimationVertical\(\s*"
    r"(?:\d+|int\.MaxValue)\s*,\s*(\d+)\s*(?:,\s*(?:true|false)\s*)?\)\s*\)"
)
_OVERRIDE_REGISTRATION = re.compile(r"RegisterItemAnimation\(\s*(\d+)\s*,\s*new (\w+)\s*\{([^{}]*)\}")
_OVERRIDE_FRAMES = re.compile(r"\bFrameCount\s*=\s*(\d+)")
_IS_FOOD = re.compile(r"\bIsFood\s*=\s*Factory\.CreateBoolSet\(\s*(?:(?:true|false)\s*,\s*)?([0-9,\s]+?)\s*\)")


@dataclass(frozen=True)
class CatalogueEntry:
    code: str
    name: str


def read_catalogue(data_root: Path) -> tuple[list[CatalogueEntry], str]:
    """The connector's item catalogue and the sha256 of the file it came from."""
    raw = (data_root / CATALOGUE).read_bytes()
    text = raw.decode("utf-8")
    start = text.find(_CATALOGUE_START)
    end = text.find(_CATALOGUE_END, start)
    if start < 0 or end < 0:
        raise BuildFailed("connector catalogue format changed: the TERRARIA_ITEM_CATALOG array was not found")
    try:
        entries = json.loads(text[start + len(_CATALOGUE_START) : end + 1])
    except json.JSONDecodeError as exc:
        raise BuildFailed(f"connector catalogue format changed: the array is not JSON ({exc})") from exc
    catalogue: list[CatalogueEntry] = []
    seen: set[str] = set()
    for entry in entries:
        code, name = (entry.get("code"), entry.get("name")) if isinstance(entry, dict) else (None, None)
        if not isinstance(code, str) or not _CODE.fullmatch(code) or not isinstance(name, str) or not name:
            raise BuildFailed("connector catalogue format changed: an entry has no numeric code and a name")
        if code in seen:
            raise BuildFailed(f"connector catalogue format changed: code {code} is listed twice")
        seen.add(code)
        catalogue.append(CatalogueEntry(code, name))
    if not catalogue:
        raise BuildFailed("connector catalogue format changed: the array is empty")
    return catalogue, hashlib.sha256(raw).hexdigest()


@dataclass(frozen=True)
class Animations:
    """Frame counts read from the game code, by item code, and how they were found."""

    frames: dict[str, int]
    food: set[str]
    overrides: dict[str, dict[str, Any]]
    registered: int
    unparsed: int
    conflicts: int


def parse_animations(sources: dict[str, str], sets_source: str) -> Animations:
    """The frame table of ``Main.RegisterItemAnimation`` calls in the decompiled caller types."""
    frames: dict[str, int] = {}
    conflicting: set[str] = set()
    constants = 0
    food_loops = 0
    unparsed = 0
    registered_as: dict[str, str] = {}

    def record(code: str, count: int) -> None:
        if frames.setdefault(code, count) != count:
            conflicting.add(code)

    food_frames: list[int] = []
    for source in sources.values():
        consumed: set[int] = set()
        for match in _CONSTANT.finditer(source):
            record(match.group(1), int(match.group(3)))
            consumed.add(match.start())
            constants += 1
        for match in _FOOD_LOOP.finditer(source):
            food_frames.append(int(match.group(2)))
            consumed.update(range(match.start(), match.end()))
            food_loops += 1
        for match in _OVERRIDE_REGISTRATION.finditer(source):
            code, kind, initializer = match.group(1), match.group(2), match.group(3)
            declared = _OVERRIDE_FRAMES.search(initializer)
            is_override = code in FRAME_OVERRIDES and kind == OVERRIDE_TYPE
            if is_override and declared and int(declared.group(1)) == FRAME_OVERRIDES[code]:
                registered_as[code] = kind
                consumed.add(match.start())
        for match in _REGISTER.finditer(source):
            if not _REGISTER_DEFINITION.search(source[max(0, match.start() - 24) : match.end()]) and (
                match.start() not in consumed
            ):
                unparsed += 1
    food: set[str] = set()
    if food_loops:
        if len(set(food_frames)) != 1 or food_frames[0] != FOOD_FRAMES:
            raise BuildFailed(
                f"the IsFood animation loop registers {sorted(set(food_frames))} frames, not {FOOD_FRAMES}"
            )
        found = _IS_FOOD.search(sets_source)
        if found is None:
            raise BuildFailed("the IsFood loop was found but ItemID.Sets.IsFood is not a Factory.CreateBoolSet of ids")
        food = {part.strip() for part in found.group(1).split(",") if part.strip()}
        for code in food:
            record(code, FOOD_FRAMES)
    else:
        raise BuildFailed("the game code has no IsFood item-animation loop (3-frame food strips)")
    registered = constants + len(food) + len(registered_as)
    overrides: dict[str, dict[str, Any]] = {}
    for code, count in FRAME_OVERRIDES.items():
        if code in frames:
            continue
        if code not in registered_as:
            raise BuildFailed(
                f"item {code} needs the {count}-frame override but its {OVERRIDE_TYPE} registration with "
                f"FrameCount = {count} was not found in the game code",
                item=code,
            )
        frames[code] = count
        overrides[code] = {"frames": count, "registeredAs": registered_as[code]}
    return Animations(frames, food, overrides, registered, unparsed, len(conflicting))


def _ranges(ids: list[int]) -> str:
    """Sorted ids as ``0, 6148-6195``."""
    spans: list[list[int]] = []
    for number in ids:
        if spans and number == spans[-1][1] + 1:
            spans[-1][1] = number
        else:
            spans.append([number, number])
    return ", ".join(str(low) if low == high else f"{low}-{high}" for low, high in spans)


def _png_rgba(path: Path) -> Image.Image:
    with Image.open(path, formats=["PNG"]) as source:
        source.load()
        return source.convert("RGBA")


def _alpha_evidence(image: Image.Image) -> tuple[int, int]:
    """``(translucent pixels, pixels with a colour channel above their alpha)``."""
    red, green, blue, alpha = image.split()
    translucent = sum(alpha.histogram()[1:255])
    above = ImageChops.lighter(
        ImageChops.lighter(ImageChops.subtract(red, alpha), ImageChops.subtract(green, alpha)),
        ImageChops.subtract(blue, alpha),
    )
    return translucent, sum(above.histogram()[1:])


def _gap_rows_blank(image: Image.Image, frames: int) -> tuple[bool, bool]:
    """``(strip height divides into frames, the rows below frame 0 are fully transparent)``."""
    pitch = image.height // frames
    divisible = (image.height + GAP) % frames == 0 or image.height % frames == 0
    gap = image.crop((0, pitch - GAP, image.width, pitch))
    return divisible, gap.getchannel("A").getbbox() is None


def map_items(ctx: MapContext) -> MapResult:
    xnb, clr = ctx.xnb, ctx.clr
    assert xnb is not None and clr is not None
    catalogue, catalogue_sha256 = read_catalogue(ctx.data_root)
    names = {entry.code: entry.name for entry in catalogue}
    codes = sorted(names, key=int)

    info, callers = clr.facts(ASSEMBLY_FILE, REGISTER_MEMBER)
    if not callers:
        raise BuildFailed(f"no method of {ASSEMBLY_FILE} calls {REGISTER_MEMBER}")
    caller_types = sorted({str(entry["type"]) for entry in callers})
    sources = clr.decompile(ASSEMBLY_FILE, [*caller_types, SETS_TYPE])
    animations = parse_animations({name: sources[name] for name in caller_types}, sources[SETS_TYPE])
    if animations.unparsed or animations.conflicts:
        raise BuildFailed(
            f"the item-animation registrations are not fully understood: {animations.unparsed} unparsed, "
            f"{animations.conflicts} with conflicting frame counts",
            unparsed=animations.unparsed,
            conflicts=animations.conflicts,
        )

    available = {int(match.group(1)): file for file in ctx.client_files if (match := ITEM_FILE.fullmatch(file))}
    outcomes = xnb.textures([TEXTURE_FILE.format(code=code) for code in codes if int(code) in available])
    clients_only = sorted(set(available) - {int(code) for code in codes})

    textures_root = ctx.work / "textures-root"
    textures: dict[str, Texture] = {}
    broken: dict[str, tuple[str, str]] = {}
    for code in codes:
        file = TEXTURE_FILE.format(code=code)
        outcome = outcomes.get(file)
        if outcome is None or isinstance(outcome, Missing):
            broken[code] = ("missing-texture", file)
        elif isinstance(outcome, Unsupported):
            broken[code] = ("unsupported-texture", outcome.detail)
        else:
            textures[code] = outcome
            destination = textures_root / safe_relative(f"items/{code}.png", field="icon path")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(outcome.path, destination)

    by_name = Counter(names.values())
    code_of_name = {name: code for code, name in names.items()}

    def layers_for(code: str) -> tuple[list[dict[str, Any]], str]:
        frame_count = animations.frames.get(code)
        operations: list[dict[str, Any]] = []
        if frame_count is not None:
            operations.append({"op": "frame-crop", "frames": frame_count, "frame": 0, "gap": GAP})
        if code in animations.food:
            rule = "food-frame"
        elif code in animations.overrides:
            rule = "frame-override"
        else:
            rule = "item" if frame_count is None else "animation-frame"
        layer: dict[str, Any] = {"image": f"items/{code}.png"}
        if operations:
            layer["ops"] = operations
        return [layer], rule

    recipes: list[dict[str, Any]] = []
    unmatched: list[dict[str, Any]] = []
    fallbacks = 0
    for code in codes:
        if code in textures:
            layers, rule = layers_for(code)
            recipes.append({"code": code, "kind": "item", "rule": rule, "layers": layers, "composite": "over"})
            continue
        reason, detail = broken[code]
        fake = _FAKE_CHEST.fullmatch(names[code])
        if reason == "missing-texture" and fake:
            target_name = f"{fake.group(1)} Chest"
            target = code_of_name.get(target_name) if by_name[target_name] == 1 else None
            if target is not None and target in textures:
                layers, _ = layers_for(target)
                recipes.append(
                    {
                        "code": code,
                        "kind": "item",
                        "rule": "fake-chest-fallback",
                        "layers": layers,
                        "composite": "over",
                        "flags": [{"flag": "fallback-art", "detail": f"drawn from item {target}"}],
                    }
                )
                fallbacks += 1
                continue
            reason, detail = "fallback-unresolved", f"{target_name} has no usable texture"
        unmatched.append({"code": code, "kind": "item", "reason": reason, "rule": "none", "detail": detail})

    document = {
        "schemaVersion": 1,
        "game": "terraria",
        "codesFrom": CODES_FROM.format(catalogue=CATALOGUE, sha256=catalogue_sha256),
        "recipes": recipes,
        "unmatched": unmatched,
    }

    non_divisible = 0
    gap_not_transparent = 0
    translucent = 0
    above = 0
    for code, texture in textures.items():
        image = _png_rgba(texture.path)
        pixels_translucent, pixels_above = _alpha_evidence(image)
        translucent += pixels_translucent
        above += pixels_above
        frame_count = animations.frames.get(code)
        if frame_count is not None:
            divisible, blank = _gap_rows_blank(image, frame_count)
            non_divisible += not divisible
            gap_not_transparent += not blank

    notes = [
        f"{len(clients_only)} client textures (Item_{_ranges(clients_only)}) are not in the connector catalogue "
        "and were not used"
        if clients_only
        else "every client item texture is in the connector catalogue",
        f"{fallbacks} fake chests without a texture use the art of the matching real chest",
    ]
    details: dict[str, Any] = {
        "catalogue": {"entries": len(catalogue), "sha256": catalogue_sha256},
        "assembly": {"name": info["name"], "version": info["version"], "mvid": info["mvid"]},
        "animations": {
            "registered": animations.registered,
            "food": len(animations.food),
            "overrides": animations.overrides,
            "unparsed": animations.unparsed,
            "conflicts": animations.conflicts,
            "callers": [
                {"type": entry["type"], "method": entry["method"], "count": entry["count"]} for entry in callers
            ],
        },
        "frames": {"nonDivisible": non_divisible},
        "gapNotTransparent": gap_not_transparent,
        "alpha": {"translucentPixels": translucent, "channelAboveAlpha": above},
        "clientOnlyIds": len(clients_only),
        "fallbacks": fallbacks,
        "textures": len(textures),
    }
    return MapResult(pack_usage={}, notes=notes, document=document, details=details)
