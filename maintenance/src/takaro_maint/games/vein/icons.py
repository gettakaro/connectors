"""VEIN item icons: codes from the item Blueprints of the server depot, thumbnails from the client's paks.

The native connector lists every non-abstract ``UItem`` subclass it can load and reports the class' short name,
which for a Blueprint is ``<AssetName>_C`` (``games/vein/mod/src/actions.cpp``, ``BuildItems``). The server's
AssetRegistry lists the Blueprints and their parent classes; each Blueprint's class default object says which
thumbnail the game shows. The paks do not say which native classes derive from ``UItem``, so the item roots are
read from the data: a native class is an item root when one of its Blueprints has an ItemType as its ``Type``,
the property ``UItem`` declares. No code or icon is guessed from how a name looks.
"""

from __future__ import annotations

import json
import re
import shutil
from collections import Counter
from dataclasses import dataclass
from typing import Any

from ...exit_codes import BuildFailed
from ...icons.mappers import MapContext, MapResult
from ...icons.unreal_session import Missing, Outcome, Texture, TextureRef, Unsupported
from ...paths import safe_relative

MAPPER_VERSION = 1
GAME_ROOT = "Vein/Content"
GAME_MODULE = "/Script/Vein."
DETAIL_LIMIT = 256
ABSTRACT_FLAG = "CLASS_Abstract"
GENERATED_PREFIXES = ("SKEL_", "REINST_")
_PARENT = re.compile(r"'(/[^']+)'")
EXCLUSIONS = "abstract Blueprints, SKEL_ and REINST_ classes, and Blueprints missing from the server depot"


@dataclass(frozen=True)
class Blueprint:
    package: str
    name: str
    parent: str

    @property
    def code(self) -> str:
        return f"{self.name}_C"


@dataclass(frozen=True)
class DefaultObject:
    """The class default object's two item properties and the class flags; ``None`` for a value it does not set."""

    abstract: bool
    thumbnail: str | None
    item_type: bool | None


def _parent_of(tags: dict[str, Any]) -> str:
    """The parent class: a package path for a Blueprint parent, ``/Script/<module>.<Class>`` for a native one."""
    match = _PARENT.search(str(tags.get("ParentClass", "")))
    if match is None:
        return ""
    path = match.group(1)
    return path if path.startswith("/Script/") else path.split(".")[0]


def _blueprints(registry: dict[str, Any]) -> dict[str, Blueprint]:
    found: dict[str, Blueprint] = {}
    for asset in sorted(registry["assets"], key=lambda row: row["packageName"]):
        if asset["assetClass"] != "Blueprint":
            continue
        found[asset["packageName"]] = Blueprint(asset["packageName"], asset["assetName"], _parent_of(asset["tags"]))
    return found


def _root_of(blueprints: dict[str, Blueprint], start: Blueprint) -> str:
    """The native class at the top of a Blueprint chain (``/Script/...``), or ``""`` when the chain breaks."""
    seen: set[str] = set()
    current = start
    while True:
        parent = current.parent
        if parent.startswith("/Script/") or not parent:
            return parent
        if parent in seen or parent not in blueprints:
            return ""
        seen.add(parent)
        current = blueprints[parent]


def _soft_path(value: Any) -> str | None:
    path = str(value.get("AssetPathName", "")) if isinstance(value, dict) else ""
    return path if path and path != "None" else None


def _default_object(exports: list[dict[str, Any]]) -> DefaultObject:
    flags = next((str(export.get("ClassFlags", "")) for export in exports if "ClassFlags" in export), "")
    properties = next(
        (export.get("Properties") or {} for export in exports if str(export.get("Name", "")).startswith("Default__")),
        {},
    )
    item_type = properties.get("Type")
    return DefaultObject(
        abstract=ABSTRACT_FLAG in flags,
        thumbnail=_soft_path(properties["Thumbnail"]) if "Thumbnail" in properties else None,
        item_type=str(item_type.get("ObjectName", "")).startswith("ItemType'") if isinstance(item_type, dict) else None,
    )


def _stem(package: str) -> str:
    parts = package.split("/")
    if len(parts) < 3 or parts[0] != "" or parts[1] != "Game":
        raise BuildFailed(
            f"texture package {package!r} is not below /Game; the mount point is not known", package=package
        )
    return f"{GAME_ROOT}/{'/'.join(parts[2:])}"


def _package_and_object(soft_path: str) -> tuple[str, str]:
    package, _, object_name = soft_path.partition(".")
    return package, object_name or package.rsplit("/", 1)[-1]


def _inherited(
    blueprints: dict[str, Blueprint], defaults: dict[str, DefaultObject], blueprint: Blueprint, field: str
) -> Any:
    """The nearest value of ``field`` along the Blueprint chain, own CDO first."""
    seen: set[str] = set()
    current: Blueprint | None = blueprint
    while current is not None and current.package not in seen:
        seen.add(current.package)
        default = defaults.get(current.package)
        if default is not None and getattr(default, field) is not None:
            return getattr(default, field)
        current = blueprints.get(current.parent)
    return None


def map_items(ctx: MapContext) -> MapResult:
    session = ctx.unreal
    assert session is not None
    registry_manifest = session.export_data(
        [{"id": "registry", "kind": "asset-registry", "classes": ["Blueprint"], "tags": ["ParentClass"]}]
    )
    registry = json.loads(registry_manifest.file("registry").read_text(encoding="utf-8"))
    blueprints = _blueprints(registry)
    candidates = sorted(
        (bp for bp in blueprints.values() if _root_of(blueprints, bp).startswith(GAME_MODULE)),
        key=lambda bp: bp.package,
    )
    if not candidates:
        raise BuildFailed(f"the AssetRegistry lists no Blueprint derived from a {GAME_MODULE}* class")

    ids = {bp.package: f"bp{index:05d}" for index, bp in enumerate(candidates)}
    manifest = session.export_data(
        [{"id": ids[bp.package], "kind": "json", "package": bp.package} for bp in candidates]
    )
    defaults: dict[str, DefaultObject] = {}
    absent = 0
    for bp in candidates:
        result = manifest.results[ids[bp.package]]
        if result["ok"]:
            defaults[bp.package] = _default_object(
                json.loads(manifest.file(ids[bp.package]).read_text(encoding="utf-8"))
            )
        elif result["errorKind"] == "not-found":
            absent += 1
        else:
            raise BuildFailed(f"{bp.package} could not be decoded: {result['error']}", package=bp.package)

    present = [bp for bp in candidates if bp.package in defaults]
    roots_with_type = {
        _root_of(blueprints, bp) for bp in present if _inherited(blueprints, defaults, bp, "item_type") is True
    }
    items = [
        bp
        for bp in present
        if _root_of(blueprints, bp) in roots_with_type
        and not defaults[bp.package].abstract
        and not bp.name.startswith(GENERATED_PREFIXES)
    ]
    if not items:
        raise BuildFailed("no Blueprint has an ItemType as its Type property; the item class cannot be identified")
    notes = [
        f"{len(candidates)} Blueprints derive from {GAME_MODULE}* classes, {absent} are not in the server depot",
        f"{len(roots_with_type)} native item roots, {len(items)} item Blueprints",
        "native-only UItem subclasses cannot be listed from the paks and are a known gap",
    ]

    refs: dict[str, TextureRef] = {}
    soft_path_of: dict[str, str] = {}
    unmatched: list[dict[str, Any]] = []
    for bp in items:
        thumbnail = _inherited(blueprints, defaults, bp, "thumbnail")
        if thumbnail is None:
            unmatched.append({"code": bp.code, "kind": "item", "reason": "no-icon-in-game", "rule": "none"})
            continue
        package, object_name = _package_and_object(thumbnail)
        refs.setdefault(package, TextureRef(package, object_name, _stem(package)))
        soft_path_of[bp.code] = package

    outcomes: dict[str, Outcome] = session.export_textures(list(refs.values()))
    textures_root = ctx.work / "textures-root"
    written: dict[str, str] = {}
    sizes: Counter[str] = Counter()
    formats: Counter[str] = Counter()

    def image_for(package: str, texture: Texture) -> str:
        relative = f"textures/{package.lstrip('/')}.png"
        if package not in written:
            target = textures_root / safe_relative(relative, field="texture path")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(texture.path, target)
            written[package] = relative
            sizes[f"{texture.width}x{texture.height}"] += 1
            formats[texture.format] += 1
        return written[package]

    recipes: list[dict[str, Any]] = []
    for code, package in sorted(soft_path_of.items()):
        outcome = outcomes[package]
        if isinstance(outcome, Texture):
            recipes.append(
                {
                    "code": code,
                    "kind": "item",
                    "rule": "thumbnail",
                    "layers": [{"image": image_for(package, outcome)}],
                    "composite": "over",
                }
            )
            continue
        assert isinstance(outcome, Missing | Unsupported)
        reason = "missing-texture" if isinstance(outcome, Missing) else "unsupported-pixel-format"
        unmatched.append(
            {
                "code": code,
                "kind": "item",
                "reason": reason,
                "rule": "thumbnail",
                "detail": outcome.detail[:DETAIL_LIMIT],
            }
        )
    unmatched.sort(key=lambda entry: entry["code"])

    codes_from = (
        f"Item codes derived from game data: every non-abstract Blueprint of server depot app "
        f"{ctx.codes_from_inputs['app']} build {ctx.codes_from_inputs['buildid']} whose native root class has "
        f"ItemType-typed Blueprints ({len(items)} Blueprints), code = <AssetName>_C as the connector's BuildItems "
        f"reports it; native-only UItem subclasses are not listed. Thumbnails are the Thumbnail property of each "
        f"class default object (parent Blueprints followed) from the client paks. "
        "Not measured against a live code list from a staging server (#811 skipped)."
    )
    document: dict[str, Any] = {
        "schemaVersion": 1,
        "game": "vein",
        "measuredAgainst": "game-data",
        "codesFrom": codes_from,
        "recipes": recipes,
    }
    if unmatched:
        document["unmatched"] = unmatched
    return MapResult(
        document=document,
        pack_usage={},
        notes=notes,
        details={
            "itemRoots": sorted(roots_with_type),
            "thumbnailSizes": dict(sorted(sizes.items())),
            "thumbnailFormats": dict(sorted(formats.items())),
            "blueprintsNotInServerDepot": absent,
        },
    )
