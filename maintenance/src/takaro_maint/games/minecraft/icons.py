"""Minecraft item icons: codes from the server's item registry, pixels from the client jar's item models.

The registry (``--reports`` of the dedicated server jar, produced by the ``minecraft-datagen`` container) lists every
item; the connector reports the same keys except ``minecraft:air``. The client jar holds, per item, an item definition
(``assets/<ns>/items/<name>.json``) that names a model, the models (a parent chain, flat ``builtin/generated`` or boxes)
and the textures. Resolution follows the default case of the definition, flat items stack their ``layer0..n`` textures
and box models are written as block-model documents for the renderer (``icons/block_model.py``). Items drawn by an
entity renderer (``minecraft:special``) have no icon art to take and stay unmatched. The jar is read in-process,
entry by exact entry name, and every entry is size-capped.
"""

from __future__ import annotations

import io
import json
import re
import zipfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from PIL import Image, UnidentifiedImageError

from ...exit_codes import BuildFailed, UsageError
from ...icons import block_model
from ...icons.build import check_model
from ...icons.mappers import MapContext, MapResult
from ...paths import safe_relative

MAPPER_VERSION = 1
AIR = "minecraft:air"
MAX_ENTRY_BYTES = 8 * 1024 * 1024
MAX_EDGE = 4096
MAX_DEPTH = 32
MAX_LAYERS = 8
MAX_FRAMES = 64
DEFAULT_NAMESPACE = "minecraft"

CODES_FROM = (
    "Item codes from the item registry in the data reports of the Minecraft {version} dedicated server "
    "(server.jar sha1 {sha1}, --reports registries.json), minus minecraft:air, which the connector skips. "
    "Not measured against a live code list from a staging server (#811 skipped)."
)

_LOCATION = re.compile(r"^([a-z0-9_.-]+:)?[a-z0-9_./-]+$")
_ELEMENT_KEYS = {"from", "to", "faces", "rotation", "shade", "shade_direction_override"}
_IGNORED_ELEMENT_KEYS = {"name", "__comment", "light_emission"}
_FACE_KEYS = {"texture", "uv", "rotation", "tintindex"}
_IGNORED_FACE_KEYS = {"cullface"}
_COLORED_BY_DEFAULT = {"dye", "potion", "firework", "map_color", "team", "custom_model_data"}
_GRASS_COLORMAP = "textures/colormap/grass.png"
_DEFINITION_COUNTERS = {"range_dispatch": "rangeDispatch"}


class Unmatched(Exception):
    """An item that cannot be drawn: ``reason`` is the coverage reason, ``detail`` says what was met."""

    def __init__(self, reason: str, detail: str = "") -> None:
        super().__init__(reason)
        self.reason = reason
        self.detail = detail


def _name(value: Any) -> str:
    return str(value).removeprefix("minecraft:")


def _sprite(value: Any) -> str:
    """A model texture entry: a reference string, or an object naming its ``sprite`` (and a translucency hint)."""
    return str(value["sprite"] if isinstance(value, dict) else value)


def location(value: Any) -> tuple[str, str]:
    """``(namespace, path)`` of a resource location; anything that is not one is an unmatched ``invalid-reference``."""
    text = str(value)
    if not _LOCATION.fullmatch(text) or ".." in text.split(":")[-1].split("/"):
        raise Unmatched("invalid-reference", text[:100])
    namespace, _, path = text.rpartition(":")
    return (namespace or DEFAULT_NAMESPACE, path)


class ClientJar:
    """Entries of the client jar read by their exact constructed names, each capped, JSON parsed once."""

    def __init__(self, path: Path) -> None:
        self._zip = zipfile.ZipFile(path)
        self._names = {info.filename: info for info in self._zip.infolist()}
        self.names = sorted(self._names)
        self._json: dict[str, Any] = {}

    def read(self, name: str) -> bytes | None:
        info = self._names.get(name)
        if info is None:
            return None
        if info.file_size > MAX_ENTRY_BYTES:
            raise BuildFailed(f"client.jar entry {name} is {info.file_size} bytes, over the {MAX_ENTRY_BYTES} cap")
        return self._zip.read(info)

    def json(self, name: str) -> Any | None:
        if name not in self._json:
            raw = self.read(name)
            self._json[name] = None if raw is None else json.loads(raw)
        return self._json[name]


@dataclass(frozen=True)
class Texture:
    image: str
    operations: list[dict[str, Any]]
    animated: bool


@dataclass
class Evidence:
    version: str = ""
    definitions: Counter[str] = field(default_factory=Counter)
    models: Counter[str] = field(default_factory=Counter)
    gui_light: Counter[str] = field(default_factory=Counter)
    tint_types: Counter[str] = field(default_factory=Counter)
    non_opaque_alpha: int = 0
    animated: Counter[str] = field(default_factory=Counter)
    render_fitted: int = 0
    ignored: Counter[str] = field(default_factory=Counter)


@dataclass(frozen=True)
class Chosen:
    model: str
    tints: list[dict[str, Any]]
    flags: list[dict[str, str]]


@dataclass(frozen=True)
class Flattened:
    flat: bool
    textures: dict[str, str]
    elements: list[dict[str, Any]] | None
    display: dict[str, Any]
    gui_light: str


class Mapping:
    def __init__(self, jar: ClientJar, textures_root: Path, evidence: Evidence) -> None:
        self.jar = jar
        self.root = textures_root
        self.evidence = evidence
        self._textures: dict[str, Texture] = {}
        self._colormap: Image.Image | None = None

    @property
    def texture_count(self) -> int:
        return len(self._textures)

    # -- item definitions ---------------------------------------------------------------------------------------

    def choose(self, node: dict[str, Any], flags: list[dict[str, str]], depth: int = 0) -> Chosen:
        """The default model of an item definition node, following the rules documented in ``docs/icons.md``."""
        if depth > MAX_DEPTH:
            raise Unmatched("model-cycle", "item definition nesting")
        kind = _name(node.get("type"))
        if kind == "model":
            return Chosen(str(node["model"]), list(node.get("tints") or []), flags)
        if kind == "condition":
            return self.choose(node["on_false"], flags, depth + 1)
        if kind == "select":
            if _name(node.get("property")) == "display_context":
                for case in node.get("cases") or []:
                    when = case["when"]
                    if "gui" in (when if isinstance(when, list) else [when]):
                        self.evidence.definitions["selectGuiCase"] += 1
                        return self.choose(case["model"], [*flags, {"flag": "gui-case"}], depth + 1)
            if "fallback" not in node:
                raise Unmatched("no-default-model", "select without a fallback")
            return self.choose(node["fallback"], flags, depth + 1)
        if kind == "range_dispatch":
            if "fallback" in node:
                return self.choose(node["fallback"], flags, depth + 1)
            entries = node.get("entries") or []
            if not entries:
                raise Unmatched("no-default-model", "range_dispatch without a fallback or entries")
            self.evidence.definitions["rangeLowestThreshold"] += 1
            lowest = min(entries, key=lambda entry: entry["threshold"])
            return self.choose(lowest["model"], [*flags, {"flag": "lowest-threshold"}], depth + 1)
        if kind == "composite":
            return self.choose(node["models"][0], flags, depth + 1)
        if kind == "special":
            raise Unmatched("entity-rendered", f"special:{_name((node.get('model') or {}).get('type'))}")
        if kind == "empty":
            raise Unmatched("no-icon-in-game")
        raise Unmatched("unsupported-model", str(node.get("type")))

    # -- models -------------------------------------------------------------------------------------------------

    def flatten(self, reference: str) -> Flattened:
        chain: list[dict[str, Any]] = []
        seen: set[tuple[str, str]] = set()
        current: str | None = reference
        root = ""
        while current is not None:
            namespace, path = location(current)
            if path.startswith("builtin/"):
                root = path
                break
            if (namespace, path) in seen or len(chain) >= MAX_DEPTH:
                raise Unmatched("model-cycle", f"{namespace}:{path}")
            seen.add((namespace, path))
            document = self.jar.json(f"assets/{namespace}/models/{path}.json")
            if document is None:
                raise Unmatched("missing-model", f"{namespace}:{path}")
            chain.append(document)
            current = document.get("parent")
        if root == "builtin/entity":
            raise Unmatched("entity-rendered", "builtin/entity")
        if root not in ("", "builtin/generated"):
            raise Unmatched("unsupported-model", root)
        textures: dict[str, str] = {}
        display: dict[str, Any] = {}
        gui_light = "side"
        elements: list[dict[str, Any]] | None = None
        for document in reversed(chain):
            textures.update({str(key): _sprite(value) for key, value in (document.get("textures") or {}).items()})
            display.update(document.get("display") or {})
            gui_light = str(document.get("gui_light", gui_light))
            if "elements" in document:
                elements = document["elements"]
        return Flattened(root == "builtin/generated", textures, elements, display, gui_light)

    @staticmethod
    def resolve_texture(value: str, textures: dict[str, str]) -> str:
        for _ in range(MAX_DEPTH):
            if not value.startswith("#"):
                return value
            if value[1:] not in textures:
                raise Unmatched("missing-texture", value)
            value = textures[value[1:]]
        raise Unmatched("missing-texture", f"{value} (reference loop)")

    # -- textures -----------------------------------------------------------------------------------------------

    def texture(self, value: str) -> Texture:
        """A texture's recipe image path and ops; the PNG is copied under the textures root once."""
        namespace, path = location(value)
        key = f"{namespace}:{path}"
        if key in self._textures:
            return self._textures[key]
        raw = self.jar.read(f"assets/{namespace}/textures/{path}.png")
        if raw is None:
            raise Unmatched("missing-texture", key)
        width, height = self._png_size(raw, key)
        image = f"textures/{namespace}/{path}.png"
        try:
            relative = safe_relative(image, field="icon texture path")
        except UsageError as exc:
            raise Unmatched("invalid-reference", key) from exc
        operations: list[dict[str, Any]] = []
        meta = self.jar.json(f"assets/{namespace}/textures/{path}.png.mcmeta")
        animation = (meta or {}).get("animation") if isinstance(meta, dict) else None
        if animation is not None:
            frames = height // width if width and height % width == 0 else 0
            if "width" in animation or "height" in animation or not 1 <= frames <= MAX_FRAMES:
                raise Unmatched("unsupported-animation", key)
            listed = animation.get("frames")
            first = listed[0] if listed else 0
            index = int(first["index"] if isinstance(first, dict) else first)
            if not 0 <= index < frames:
                raise Unmatched("unsupported-animation", f"{key} frame {index}")
            operations.append({"op": "frame-crop", "frames": frames, "frame": index, "gap": 0})
        destination = self.root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
        self._textures[key] = Texture(image, operations, animation is not None)
        return self._textures[key]

    @staticmethod
    def _png_size(raw: bytes, key: str) -> tuple[int, int]:
        try:
            with Image.open(io.BytesIO(raw), formats=["PNG"]) as image:
                width, height = image.size
        except (UnidentifiedImageError, OSError) as exc:
            raise BuildFailed(f"client.jar texture {key} is not a PNG ({exc})") from exc
        if not (0 < width <= MAX_EDGE and 0 < height <= MAX_EDGE):
            raise BuildFailed(f"client.jar texture {key} is {width}x{height}, outside 1..{MAX_EDGE}")
        return width, height

    # -- tints --------------------------------------------------------------------------------------------------

    def _grass(self, source: dict[str, Any]) -> int:
        if self._colormap is None:
            raw = self.jar.read(f"assets/{DEFAULT_NAMESPACE}/{_GRASS_COLORMAP}")
            if raw is None:
                raise BuildFailed(f"client.jar has no {_GRASS_COLORMAP}")
            self._png_size(raw, _GRASS_COLORMAP)
            self._colormap = Image.open(io.BytesIO(raw), formats=["PNG"]).convert("RGBA")
        temperature = min(max(float(source.get("temperature", 0.5)), 0.0), 1.0)
        downfall = min(max(float(source.get("downfall", 1.0)), 0.0), 1.0) * temperature
        x, y = int((1 - temperature) * 255), int((1 - downfall) * 255)
        pixel = self._colormap.getpixel((x, y))
        assert isinstance(pixel, tuple)
        red, green, blue, alpha = pixel
        return (alpha << 24) | (red << 16) | (green << 8) | blue

    @staticmethod
    def _argb(value: Any) -> int:
        if isinstance(value, bool) or not isinstance(value, int):
            raise Unmatched("unsupported-tint", f"value {value!r}"[:100])
        return value & 0xFFFFFFFF

    def tint_colors(self, tints: list[dict[str, Any]]) -> list[str | None]:
        """``#rrggbb`` per tint index (``None`` for white); unsupported tint types leave the item unmatched."""
        colors: list[str | None] = []
        for source in tints:
            kind = _name(source.get("type"))
            if kind == "constant":
                argb = self._argb(source.get("value"))
            elif kind in _COLORED_BY_DEFAULT:
                argb = self._argb(source.get("default"))
            elif kind == "grass":
                argb = self._grass(source)
            else:
                raise Unmatched("unsupported-tint", str(source.get("type")))
            self.evidence.tint_types[kind] += 1
            if argb >> 24 != 0xFF:
                self.evidence.non_opaque_alpha += 1
            colors.append(None if argb & 0xFFFFFF == 0xFFFFFF else f"#{argb & 0xFFFFFF:06x}")
        return colors

    # -- recipes ------------------------------------------------------------------------------------------------

    def flat_layers(self, model: Flattened, colors: list[str | None]) -> list[dict[str, Any]]:
        layers: list[dict[str, Any]] = []
        while len(layers) < MAX_LAYERS and f"layer{len(layers)}" in model.textures:
            index = len(layers)
            texture = self.texture(self.resolve_texture(model.textures[f"layer{index}"], model.textures))
            operations = list(texture.operations)
            if index < len(colors) and colors[index] is not None:
                operations.append({"op": "tint-multiply", "color": colors[index]})
            layers.append({"image": texture.image, **({"ops": operations} if operations else {})})
            self.evidence.animated["flat"] += texture.animated
        if not layers:
            raise Unmatched("no-layers")
        return layers

    def model_document(self, model: Flattened, colors: list[str | None]) -> dict[str, Any]:
        assert model.elements is not None
        textures: dict[str, Any] = {}
        elements = []
        for source in model.elements:
            self._check_keys(source, _ELEMENT_KEYS, _IGNORED_ELEMENT_KEYS, "element")
            element: dict[str, Any] = {"from": source["from"], "to": source["to"], "faces": {}}
            if "shade" in source:
                element["shade"] = bool(source["shade"])
            if "shade_direction_override" in source:
                element["shadeDirection"] = source["shade_direction_override"]
            if "rotation" in source:
                element["rotation"] = self._rotation(source["rotation"])
            for name, face in source["faces"].items():
                self._check_keys(face, _FACE_KEYS, _IGNORED_FACE_KEYS, "face")
                texture = self.texture(self.resolve_texture(str(face["texture"]), model.textures))
                key = texture.image
                if key not in textures:
                    textures[key] = {
                        "image": texture.image,
                        **({"ops": texture.operations} if texture.operations else {}),
                    }
                    self.evidence.animated["model"] += texture.animated
                built: dict[str, Any] = {"texture": key}
                for field_name in ("uv", "rotation"):
                    if field_name in face:
                        built[field_name] = face[field_name]
                index = face.get("tintindex")
                if index is not None and 0 <= index < len(colors) and colors[index] is not None:
                    built["tint"] = colors[index]
                element["faces"][name] = built
            elements.append(element)
        document: dict[str, Any] = {"schemaVersion": 1, "textures": textures, "elements": elements}
        gui = model.display.get("gui")
        if gui:
            document["gui"] = {key: gui[key] for key in ("rotation", "translation", "scale") if key in gui}
        document["light"] = model.gui_light
        try:
            check_model(document, code="model")
        except UsageError as exc:
            problems = exc.detail.get("problems")
            first = problems[0] if isinstance(problems, list) and problems else "invalid"
            raise Unmatched("unsupported-model", str(first)[:200]) from exc
        return document

    def _check_keys(self, source: dict[str, Any], known: set[str], ignored: set[str], what: str) -> None:
        for key in source:
            if key in ignored:
                self.evidence.ignored[f"{what}.{key}"] += 1
            elif key not in known:
                raise Unmatched("unsupported-model", f"{what} key {key}")

    @staticmethod
    def _rotation(source: dict[str, Any]) -> dict[str, Any]:
        if not {"origin", "axis", "angle"} <= set(source):
            raise Unmatched("unsupported-model", "rotation form")
        rotation = {"origin": source["origin"], "axis": source["axis"], "angle": source["angle"]}
        if "rescale" in source:
            rotation["rescale"] = bool(source["rescale"])
        return rotation


def read_registry(path: Path) -> tuple[list[str], set[str]]:
    """The sorted item keys (minus air) and the set of block keys of the registries data report."""
    document = json.loads(path.read_text(encoding="utf-8"))
    try:
        items = [str(key) for key in document["minecraft:item"]["entries"]]
        blocks = {str(key) for key in document["minecraft:block"]["entries"]}
    except (KeyError, TypeError) as exc:
        raise BuildFailed(
            "data report format changed: registries.json has no minecraft:item and minecraft:block entries"
        ) from exc
    return sorted(set(items) - {AIR}), blocks


def map_items(ctx: MapContext) -> MapResult:
    inputs = ctx.minecraft
    assert inputs is not None
    codes, blocks = read_registry(inputs.reports["registries.json"])
    evidence = Evidence(version=inputs.version)
    mapping = Mapping(ClientJar(inputs.client_jar), ctx.work / "textures-root", evidence)
    recipes: list[dict[str, Any]] = []
    unmatched: list[dict[str, Any]] = []
    in_jar_without_code = 0
    for code in codes:
        kind = "block" if code in blocks else "item"
        try:
            recipes.append(_recipe(mapping, evidence, code, kind))
        except Unmatched as problem:
            entry = {"code": code, "kind": kind, "reason": problem.reason}
            if problem.detail:
                entry["detail"] = problem.detail
            unmatched.append(entry)
    known = {*codes, AIR}
    for name in mapping.jar.names:
        match = re.fullmatch(r"assets/minecraft/items/([a-z0-9_/.-]+)\.json", name)
        if match and f"minecraft:{match.group(1)}" not in known:
            in_jar_without_code += 1
    document = {
        "schemaVersion": 1,
        "game": "minecraft",
        "codesFrom": CODES_FROM.format(version=inputs.version, sha1=inputs.server_sha1),
        "recipes": recipes,
        "unmatched": unmatched,
    }
    details: dict[str, Any] = {
        "gameVersion": inputs.version,
        "registry": {"items": len(codes) + 1, "blocks": len(blocks), "excluded": [AIR]},
        "definitions": dict(sorted(evidence.definitions.items())),
        "models": {
            "generated": evidence.models["generated"],
            "elements": evidence.models["elements"],
            "generatedGuiIgnored": evidence.models["generatedGuiIgnored"],
            "guiLight": dict(sorted(evidence.gui_light.items())),
        },
        "tints": {"byType": dict(sorted(evidence.tint_types.items())), "nonOpaqueAlpha": evidence.non_opaque_alpha},
        "animated": {"flat": evidence.animated["flat"], "model": evidence.animated["model"]},
        "textures": mapping.texture_count,
        "renderFitted": evidence.render_fitted,
        "ignoredKeys": dict(sorted(evidence.ignored.items())),
        "itemDefinitionsWithoutCode": in_jar_without_code,
    }
    notes = [
        f"{sum(entry['reason'] == 'entity-rendered' for entry in unmatched)} items are drawn by an entity renderer "
        "and stay unmatched",
        f"{evidence.definitions['selectGuiCase']} select definitions take their gui case and "
        f"{evidence.definitions['rangeLowestThreshold']} range_dispatch definitions take their lowest threshold",
    ]
    return MapResult(pack_usage={}, notes=notes, document=document, details=details)


def _recipe(mapping: Mapping, evidence: Evidence, code: str, kind: str) -> dict[str, Any]:
    namespace, path = location(code)
    definition = mapping.jar.json(f"assets/{namespace}/items/{path}.json")
    if definition is None:
        raise Unmatched("missing-item-model")
    top = _name(definition["model"].get("type"))
    evidence.definitions[_DEFINITION_COUNTERS.get(top, top)] += 1
    for key in definition:
        if key != "model":
            evidence.ignored[f"item.{key}"] += 1
    chosen = mapping.choose(definition["model"], [])
    model = mapping.flatten(chosen.model)
    colors = mapping.tint_colors(chosen.tints)
    flags = list(chosen.flags)
    if model.flat:
        evidence.models["generated"] += 1
        evidence.models["generatedGuiIgnored"] += "gui" in model.display
        layers = mapping.flat_layers(model, colors)
        rule = (
            "flat-tinted"
            if any("tint-multiply" in [op["op"] for op in layer.get("ops", [])] for layer in layers)
            else "flat"
        )
    elif model.elements is None:
        raise Unmatched("unsupported-model", "no elements")
    else:
        evidence.models["elements"] += 1
        evidence.gui_light[model.gui_light] += 1
        document = mapping.model_document(model, colors)
        if block_model.fit_to_canvas(document):
            evidence.render_fitted += 1
            flags.append({"flag": "render-fitted"})
        relative = safe_relative(f"models/{namespace}/{path}.json", field="icon model path")
        destination = mapping.root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(document, sort_keys=True, indent=1) + "\n", encoding="utf-8")
        layers = [{"model": relative.as_posix()}]
        tinted = any("tint" in face for element in document["elements"] for face in element["faces"].values())
        rule = "rendered-tinted" if tinted else "rendered"
    return {
        "code": code,
        "kind": kind,
        "rule": rule,
        "layers": layers,
        "composite": "over",
        **({"flags": flags} if flags else {}),
    }
