"""Valheim item icons: codes and icon sprites come from the asset bundles of the dedicated-server depot.

The connector lists ``ZNetScene.m_prefabs`` entries whose root GameObject carries an ``ItemDrop`` and reports
each prefab's name as the item code. The icon is ``m_itemData.m_shared.m_icons[0]``, a Sprite cut from an atlas
Texture2D. The Unity decoder dumps the two MonoBehaviours and describes sprites and textures; everything that
joins them, by object reference and never by name, is here.
"""

from __future__ import annotations

import hashlib
import shutil
from collections import Counter
from pathlib import Path
from typing import Any

from PIL import Image

from ...exit_codes import BuildFailed, UpstreamUnavailable
from ...icons import ops
from ...icons.mappers import MapContext, MapResult
from ...icons.unity_session import (
    Missing,
    Ref,
    Scan,
    ScriptSpec,
    SpriteMeta,
    Texture,
    UnitySession,
    Unsupported,
    ref_of,
)
from ...paths import safe_relative

MAPPER_VERSION = 1
CODES_DESCRIPTION = (
    "prefab names of ZNetScene.m_prefabs entries with a root ItemDrop component, from the SoftRef asset bundles"
)
EXCLUSIONS = "ZNetScene prefabs without an ItemDrop and repeated prefab names (the first is kept)"

BUNDLES = "valheim_server_Data/StreamingAssets/SoftRef/Bundles"
ASSEMBLY = "assembly_valheim"
ICONS_FIELD = "m_itemData.m_shared.m_icons"
PREFABS_FIELD = "m_prefabs"
REFERENCE_FLAG = "cut-differs-from-reference"
REFERENCE_CAP = 64
REFERENCE_FIRST_CODES = 16
LISTED_BUNDLES = 20


def list_bundles(data_root: Path) -> list[str]:
    root = data_root / BUNDLES
    files = sorted(path.relative_to(data_root).as_posix() for path in root.rglob("*") if path.is_file())
    if not files:
        raise UpstreamUnavailable(f"the server depot served no asset bundles under {BUNDLES}", missing=BUNDLES)
    return files


def _znetscene_prefabs(scan: Scan, notes: list[str]) -> list[dict[str, Any] | None]:
    scenes = scan.behaviours["ZNetScene"]
    if not scenes:
        raise BuildFailed("no ZNetScene MonoBehaviour in the bundles")
    lists = {scene["bundle"]: scene["fields"][PREFABS_FIELD] for scene in scenes}
    distinct = {repr(value) for value in lists.values()}
    if len(distinct) > 1:
        raise BuildFailed("the bundles hold ZNetScene MonoBehaviours with different m_prefabs", bundles=sorted(lists))
    if len(scenes) > 1:
        notes.append(f"{len(scenes)} identical ZNetScene MonoBehaviours; the first is used")
    prefabs: list[dict[str, Any] | None] = scenes[0]["fields"][PREFABS_FIELD]
    return prefabs


def _item_drops(scan: Scan) -> dict[Ref, tuple[str, list[dict[str, Any] | None]]]:
    """Root ItemDrop components by their GameObject reference, with the prefab name and its ``m_icons``."""
    drops: dict[Ref, tuple[str, list[dict[str, Any] | None]]] = {}
    for drop in scan.behaviours["ItemDrop"]:
        owner = ref_of(drop["gameObject"])
        if owner is None:
            continue
        drops.setdefault(owner, (str(drop["gameObject"]["name"]), drop["fields"][ICONS_FIELD] or []))
    return drops


def _needs_frame(meta: SpriteMeta, rotation: str) -> bool:
    width, height = meta.texture_rect[2], meta.texture_rect[3]
    if rotation == "rotate-90":
        width, height = height, width
    return [width, height] != meta.rect[2:] or any(meta.texture_rect_offset)


def _recipe_op(meta: SpriteMeta) -> dict[str, Any]:
    rotation = meta.packing_rotation if meta.packed else "none"
    operation: dict[str, Any] = {"op": "atlas-cut", "rect": meta.texture_rect, "rotation": rotation}
    if _needs_frame(meta, rotation):
        operation["canvas"] = meta.rect[2:]
        operation["offset"] = meta.texture_rect_offset
    return operation


def _reference_sample(
    recipes: dict[str, dict[str, Any]], icon_of: dict[str, Ref], metas: dict[Ref, SpriteMeta]
) -> list[Ref]:
    """Every rotated sprite, then the first codes' sprites, then trimmed ones, each group in a fixed order, capped."""
    distinct = sorted({icon_of[code] for code in recipes})
    rotated = [ref for ref in distinct if _recipe_op(metas[ref])["rotation"] != "none"]
    first_codes = [icon_of[code] for code in sorted(recipes)[:REFERENCE_FIRST_CODES]]
    framed = [ref for ref in distinct if "canvas" in _recipe_op(metas[ref])]
    return list(dict.fromkeys([*rotated, *first_codes, *framed]))[:REFERENCE_CAP]


def _png_rgba(path: Path) -> Image.Image:
    with Image.open(path, formats=["PNG"]) as source:
        source.load()
        return source.convert("RGBA")


def map_items(ctx: MapContext) -> MapResult:
    session = ctx.unity
    assert session is not None
    notes: list[str] = []
    files = list_bundles(ctx.data_root)
    scan = session.scan(
        files,
        [
            ScriptSpec("ZNetScene", "", ASSEMBLY, [PREFABS_FIELD]),
            ScriptSpec("ItemDrop", "", ASSEMBLY, [ICONS_FIELD]),
        ],
    )
    cab_index = scan.cab_index()
    prefabs = _znetscene_prefabs(scan, notes)
    drops = _item_drops(scan)

    counts: Counter[str] = Counter()
    candidates: dict[str, list[dict[str, Any] | None]] = {}
    for prefab in prefabs:
        ref = ref_of(prefab)
        if ref is None:
            counts["null-prefab"] += 1
        elif ref[0] not in cab_index:
            counts["unresolved-prefab"] += 1
        elif ref not in drops:
            counts["non-item-prefabs"] += 1
        else:
            name, icons = drops[ref]
            if name in candidates:
                counts["duplicate-name"] += 1
            else:
                candidates[name] = icons
    first_icon: dict[str, Ref] = {}
    unmatched: list[dict[str, Any]] = []
    rules: dict[str, str] = {}
    for code, icons in sorted(candidates.items()):
        icon = ref_of(icons[0]) if icons else None
        if icon is None:
            unmatched.append({"code": code, "kind": "item", "reason": "no-icon-in-game", "rule": "none"})
            continue
        first_icon[code] = icon
        rules[code] = "first-of-variants" if len(icons) > 1 else "icon"
        counts["multi-icon"] += int(len(icons) > 1)

    sprite_outcomes = session.sprites(set(first_icon.values()), cab_index) if first_icon else {}
    metas: dict[Ref, SpriteMeta] = {}
    broken: dict[Ref, tuple[str, str]] = {}
    for ref, outcome in sprite_outcomes.items():
        if isinstance(outcome, Missing):
            broken[ref] = ("unresolved-reference", outcome.detail)
        elif (outcome.packed and outcome.packing_mode == "tight") or outcome.alpha_texture is not None:
            broken[ref] = ("unsupported-sprite", "separate-alpha" if outcome.alpha_texture else "tight-packed")
        else:
            metas[ref] = outcome
    texture_refs = {meta.texture for meta in metas.values()}
    texture_outcomes = session.textures(texture_refs, cab_index) if texture_refs else {}
    atlases: dict[Ref, Texture] = {}
    for ref, texture_outcome in texture_outcomes.items():
        if isinstance(texture_outcome, Texture):
            atlases[ref] = texture_outcome
    for ref, meta in list(metas.items()):
        found = texture_outcomes[meta.texture]
        if isinstance(found, Unsupported):
            broken[ref] = ("unsupported-texture", found.detail)
            del metas[ref]
        elif isinstance(found, Missing):
            broken[ref] = ("unresolved-reference", found.detail)
            del metas[ref]

    textures_root = ctx.work / "textures-root"
    atlas_names = {ref: f"atlases/tx{number:05d}.png" for number, ref in enumerate(sorted(texture_refs), start=1)}
    atlas_files: dict[str, dict[str, Any]] = {}
    for ref, texture in sorted(atlases.items()):
        destination = textures_root / safe_relative(atlas_names[ref], field="atlas path")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(texture.path, destination)
        atlas_files[atlas_names[ref]] = {
            "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
            "width": texture.width,
            "height": texture.height,
            "format": texture.format,
        }

    icon_of: dict[str, Ref] = {}
    recipes: dict[str, dict[str, Any]] = {}
    for code, ref in first_icon.items():
        if ref in broken:
            reason, detail = broken[ref]
            unmatched.append({"code": code, "kind": "item", "reason": reason, "rule": rules[code], "detail": detail})
            continue
        icon_of[code] = ref
        recipes[code] = {
            "code": code,
            "kind": "item",
            "rule": rules[code],
            "layers": [{"image": atlas_names[metas[ref].texture], "ops": [_recipe_op(metas[ref])]}],
            "composite": "over",
        }

    reference = _reference_check(session, cab_index, recipes, icon_of, metas, atlas_names, textures_root)
    differing = reference.pop("differing")
    for code, recipe in recipes.items():
        if icon_of[code] in differing:
            recipe["flags"] = [{"flag": REFERENCE_FLAG}]
    if differing:
        notes.append(f"{sum(1 for code in recipes if icon_of[code] in differing)} codes differ from UnityPy's cut")

    for label, key in (
        ("null prefab entries", "null-prefab"),
        ("prefabs from no bundle (item status unknown)", "unresolved-prefab"),
        ("prefabs without an ItemDrop", "non-item-prefabs"),
        ("repeated prefab names (first kept)", "duplicate-name"),
        ("prefabs with several icons (first taken)", "multi-icon"),
    ):
        notes.append(f"{counts[key]} {label}")

    packing = Counter(
        meta.packing_rotation if meta.packed else "none" for ref, meta in metas.items() if ref in set(icon_of.values())
    )
    document = {
        "schemaVersion": 1,
        "game": "valheim",
        "codesFrom": ctx.codes_from,
        "recipes": [recipes[code] for code in sorted(recipes)],
        "unmatched": sorted(unmatched, key=lambda entry: entry["code"]),
    }
    details: dict[str, Any] = {
        "bundles": len(files),
        "prefabs": len(prefabs),
        "itemPrefabs": len(candidates),
        "unresolvedPrefabs": counts["unresolved-prefab"],
        "multiIcon": counts["multi-icon"],
        "sprites": len(set(first_icon.values())),
        "atlases": {
            "count": len(atlas_files),
            "formats": dict(sorted(Counter(info["format"] for info in atlas_files.values()).items())),
            "sizes": sorted([info["width"], info["height"]] for info in atlas_files.values()),
        },
        "packing": {
            "rotations": dict(sorted(packing.items())),
            "trimmed": sum(1 for ref in set(icon_of.values()) if "canvas" in _recipe_op(metas[ref])),
        },
        "referenceCheck": reference,
        "atlasFiles": atlas_files,
    }
    return MapResult(pack_usage={}, notes=notes, document=document, details=details)


def _reference_check(
    session: UnitySession,
    cab_index: dict[str, str],
    recipes: dict[str, dict[str, Any]],
    icon_of: dict[str, Ref],
    metas: dict[Ref, SpriteMeta],
    atlas_names: dict[Ref, str],
    textures_root: Path,
) -> dict[str, Any]:
    """Compare the recipe's cut with UnityPy's own, exactly, for a deterministic sample of sprites."""
    sample = _reference_sample(recipes, icon_of, metas) if recipes else []
    references = session.references(set(sample), cab_index) if sample else {}
    equal = 0
    rotated = 0
    differing: set[Ref] = set()
    loaded: dict[Ref, Image.Image] = {}
    for ref in sample:
        meta = metas[ref]
        rotation = meta.packing_rotation if meta.packed else "none"
        rotated += int(rotation != "none")
        reference = references[ref]
        if isinstance(reference, Missing):
            differing.add(ref)
            continue
        if meta.texture not in loaded:
            loaded[meta.texture] = _png_rgba(textures_root / atlas_names[meta.texture])
        cut = ops.atlas_cut(loaded[meta.texture], code=meta.name, rect=meta.texture_rect, rotation=rotation)
        expected = _png_rgba(reference)
        if cut.size == expected.size and cut.tobytes() == expected.tobytes():
            equal += 1
        else:
            differing.add(ref)
    return {"sampled": len(sample), "equal": equal, "rotated": rotated, "differing": differing}
