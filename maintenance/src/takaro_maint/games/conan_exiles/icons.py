"""Conan Exiles item icons: codes from the ItemNameToTemplateID tables, icons from the ItemTableRow tables.

Everything comes from decoded game data: the server depot's AssetRegistry says which DataTables use the
``ItemTableRow`` and ``ItemNameToTemplateIDStruct`` row structs, their rows say which template id has which
icon, and the client's textures supply the pixels. The item codes follow the native connector's
``Catalogue::BuildItems`` (``games/conan-exiles/native/core/conan/catalogue.cpp``): the code of a template id is
its first row name in the base name table, otherwise the id itself, and internal rows are not listed.

No code or icon is guessed from how a name looks: a code without an icon in the data is reported as unmatched.
"""

from __future__ import annotations

import json
import re
import shutil
from dataclasses import dataclass
from typing import Any

from ...exit_codes import BuildFailed
from ...icons.mappers import MapContext, MapResult
from ...icons.unreal_session import Missing, Outcome, Texture, TextureRef, Unsupported
from ...paths import safe_relative

MAPPER_VERSION = 1
GAME_ROOT = "ConanSandbox/Content"
BASE_ITEM_TABLE = "/Game/Items/ItemTable"
BASE_NAME_TABLE = "/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToTemplateID"
ITEM_ROW_STRUCT = "/Script/ConanSandbox.ItemTableRow"
NAME_ROW_STRUCT = "ItemNameToTemplateIDStruct"
# Shares the name-to-id row struct but maps item names to harvest XP values, not template ids.
NOT_NAME_TABLES = frozenset({"/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToHarvestXPValue"})
TABLE_CLASSES = ["DataTable", "CompositeDataTable"]
MAX_TEMPLATE_ID = 2_147_483_647
DETAIL_LIMIT = 256
MASK_FORMATS = frozenset({"PF_BC4", "PF_G8"})
C_SPACE = " \t\n\v\f\r"
_INTEGER_ROW = re.compile(r"[+-]?[0-9]+")
_INTERNAL_PREFIXES = ("dev ", "dev_", "test ", "test_", "deprecated", "do not use")
_LAYER_DEFAULTS: dict[str, Any] = {"SubPathString": ""}


def c_trim(text: str) -> str:
    return text.strip(C_SPACE)


def _ascii_lower(text: str) -> str:
    return "".join(chr(ord(ch) + 32) if "A" <= ch <= "Z" else ch for ch in text)


def is_internal_name(text: str) -> bool:
    """Port of the connector's ``IsInternalName``: placeholder, debug and retired names are not listed."""
    stripped = text.lstrip(C_SPACE)
    if not stripped:
        return True
    lowered = _ascii_lower(stripped)
    xs = len(lowered) - len(lowered.lstrip("x"))
    if xs >= 2 and xs < len(lowered) and lowered[xs] in " _":
        return True
    return lowered.startswith(_INTERNAL_PREFIXES)


@dataclass(frozen=True)
class Table:
    package: str
    asset_class: str
    row_struct: str
    export_id: str


@dataclass
class Item:
    template_id: int
    table: str
    name: str
    icon: str
    layers: list[dict[str, Any]]


def _text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        for key in ("SourceString", "LocalizedString", "CultureInvariantString"):
            if isinstance(value.get(key), str):
                return str(value[key])
    return ""


def _template_id(row_name: str) -> int | None:
    text = row_name.strip()
    if not _INTEGER_ROW.fullmatch(text):
        return None
    value = int(text)
    return value if 1 <= value <= MAX_TEMPLATE_ID else None


def _soft_path(value: Any) -> str:
    return str(value.get("AssetPathName", "")) if isinstance(value, dict) else ""


def _package_and_object(soft_path: str) -> tuple[str, str]:
    package, _, object_name = soft_path.partition(".")
    return package, object_name or package.rsplit("/", 1)[-1]


def _stem(package: str) -> str:
    parts = package.split("/")
    if len(parts) < 3 or parts[0] != "" or parts[1] != "Game":
        raise BuildFailed(
            f"texture package {package!r} is not below /Game; the mount point is not known", package=package
        )
    return f"{GAME_ROOT}/{'/'.join(parts[2:])}"


def _tables(registry: dict[str, Any]) -> tuple[list[Table], list[Table]]:
    """``(item tables, name tables)`` from the AssetRegistry, in sorted package order."""
    items: list[Table] = []
    names: list[Table] = []
    assets = sorted(registry["assets"], key=lambda asset: asset["packageName"])
    for index, asset in enumerate(assets):
        row_struct = asset["tags"].get("RowStructure", "")
        table = Table(asset["packageName"], asset["assetClass"], row_struct, f"tbl{index:05d}")
        if row_struct == ITEM_ROW_STRUCT:
            items.append(table)
        elif row_struct.rsplit(".", 1)[-1] == NAME_ROW_STRUCT and asset["packageName"] not in NOT_NAME_TABLES:
            names.append(table)
    return items, names


def _read_rows(manifest: Any, table: Table) -> tuple[dict[str, Any], dict[str, Any]]:
    document = json.loads(manifest.file(table.export_id).read_text(encoding="utf-8"))
    export = next(entry for entry in document if entry.get("Type") == table.asset_class)
    return export["Rows"], export.get("Properties", {})


def _template_of(row: Any) -> int | None:
    for value in row.values():
        if isinstance(value, int) and not isinstance(value, bool):
            return value
    return None


def _item_order(tables: list[Table]) -> list[Table]:
    """Base ItemTable first, then the other plain tables, then composites; each in sorted package order."""

    def rank(table: Table) -> tuple[int, str]:
        if table.package == BASE_ITEM_TABLE:
            return 0, table.package
        return (2 if table.asset_class == "CompositeDataTable" else 1), table.package

    return sorted(tables, key=rank)


def _soft_layers(row: dict[str, Any]) -> tuple[str, list[dict[str, Any]]]:
    return _soft_path(row.get("Icon")), [layer for layer in row.get("IconLayers") or [] if isinstance(layer, dict)]


def _layer_field(layer: dict[str, Any]) -> str | None:
    """The first field of a layer other than its texture that is not at its default, or ``None``."""
    for key, value in layer.items():
        if key == "AssetPathName":
            continue
        if value != _LAYER_DEFAULTS.get(key):
            return key
    return None


def map_items(ctx: MapContext) -> MapResult:
    session = ctx.unreal
    assert session is not None
    registry_manifest = session.export_data([{"id": "registry", "kind": "asset-registry", "classes": TABLE_CLASSES}])
    registry = json.loads(registry_manifest.file("registry").read_text(encoding="utf-8"))
    item_tables, name_tables = _tables(registry)
    if not any(table.package == BASE_ITEM_TABLE for table in item_tables):
        raise BuildFailed(f"the AssetRegistry lists no {BASE_ITEM_TABLE} item table")
    if not any(table.package == BASE_NAME_TABLE for table in name_tables):
        raise BuildFailed(f"the AssetRegistry lists no {BASE_NAME_TABLE} name table")
    manifest = session.export_data(
        [{"id": table.export_id, "kind": "json", "package": table.package} for table in [*item_tables, *name_tables]]
    )
    failed = manifest.failures()
    if failed:
        raise BuildFailed(
            f"{len(failed)} item or name tables could not be decoded",
            failures=[f"{result['id']}: {result['error']}" for result in failed[:20]],
        )

    notes: list[str] = []
    table_record = []
    items: dict[int, Item] = {}
    base_ids: set[int] = set()
    disagreements = 0
    skipped_rows = 0
    for table in _item_order(item_tables):
        rows, properties = _read_rows(manifest, table)
        record: dict[str, Any] = {"package": table.package, "class": table.asset_class, "rows": len(rows)}
        if table.asset_class == "CompositeDataTable":
            record["parents"] = [
                str(parent.get("ObjectPath", "")).removesuffix(".0") for parent in properties.get("ParentTables", [])
            ]
        table_record.append(record)
        for row_name, row in rows.items():
            template_id = _template_id(row_name)
            if template_id is None:
                skipped_rows += 1
                continue
            icon, layers = _soft_layers(row)
            item = Item(template_id, table.package, c_trim(_text(row.get("Name"))), icon, layers)
            if table.package == BASE_ITEM_TABLE:
                base_ids.add(template_id)
            first = items.get(template_id)
            if first is None:
                items[template_id] = item
            elif (first.name, first.icon, first.layers) != (item.name, item.icon, item.layers):
                disagreements += 1
    if skipped_rows:
        notes.append(f"{skipped_rows} item table rows are not keyed by a template id and were skipped")
    if disagreements:
        notes.append(
            f"{disagreements} repeated template ids disagree on Name, Icon or IconLayers (the first table wins)"
        )

    name_for: dict[int, str] = {}
    other_names: dict[int, str] = {}
    name_record = []
    for table in sorted(name_tables, key=lambda candidate: candidate.package):
        rows, _ = _read_rows(manifest, table)
        name_record.append({"package": table.package, "class": table.asset_class, "rows": len(rows), "role": "name"})
        for row_name, row in rows.items():
            template_id = _template_of(row)
            if template_id is None or template_id <= 0:
                continue
            if table.package == BASE_NAME_TABLE:
                name_for.setdefault(template_id, row_name)
            else:
                other_names.setdefault(template_id, row_name)

    primaries: dict[str, Item] = {}
    internal = 0
    duplicates = 0
    for template_id in sorted(items):
        item = items[template_id]
        code = name_for.get(template_id, str(template_id))
        if is_internal_name(item.name) or is_internal_name(code):
            internal += 1
            continue
        if code in primaries:
            duplicates += 1
            continue
        primaries[code] = item
    dlc = {code: item for code, item in primaries.items() if item.template_id not in base_ids}
    taken = set(primaries)
    candidate_codes: dict[str, str] = {}
    dropped = 0
    for code, item in sorted(dlc.items(), key=lambda row: row[1].template_id):
        alternative = str(item.template_id) if item.template_id in name_for else other_names.get(item.template_id)
        if alternative is None or alternative == code:
            continue
        if alternative in taken or alternative in candidate_codes:
            dropped += 1
            continue
        candidate_codes[alternative] = code
    if duplicates:
        notes.append(f"{duplicates} template ids share a code with an earlier template id and were left out")
    if dropped:
        notes.append(f"{dropped} DLC candidate codes collide with another code and were dropped")
    notes.append(f"{len(primaries)} listed codes, {len(dlc)} of them DLC, {len(candidate_codes)} candidate codes")

    refs: dict[str, TextureRef] = {}
    plan: dict[str, tuple[str, list[str]]] = {}
    unmatched: list[dict[str, Any]] = []
    for code, item in sorted(primaries.items()):
        rule = "icon" if item.icon else ("icon-layers" if item.layers else "none")
        if rule == "none":
            unmatched.append({"code": code, "kind": "item", "reason": "no-icon-in-game", "rule": "none"})
            continue
        soft_paths = [item.icon] if item.icon else []
        if rule == "icon-layers":
            offending = next((field for layer in item.layers if (field := _layer_field(layer))), None)
            if offending:
                unmatched.append(
                    {
                        "code": code,
                        "kind": "item",
                        "reason": "unsupported-layer-field",
                        "rule": rule,
                        "detail": f"layer field {offending}",
                    }
                )
                continue
            soft_paths = [_soft_path(layer) for layer in item.layers]
        if not all(soft_paths):
            unmatched.append(
                {
                    "code": code,
                    "kind": "item",
                    "reason": "missing-texture",
                    "rule": rule,
                    "detail": "a layer has no texture",
                }
            )
            continue
        for soft_path in soft_paths:
            package, object_name = _package_and_object(soft_path)
            refs.setdefault(package, TextureRef(package, object_name, _stem(package)))
        plan[code] = (rule, soft_paths)

    outcomes: dict[str, Outcome] = session.export_textures(list(refs.values()))

    textures_root = ctx.work / "textures-root"
    written: dict[str, str] = {}

    def image_for(package: str, texture: Texture) -> str:
        relative = f"textures/{package.lstrip('/')}.png"
        if package not in written:
            target = textures_root / safe_relative(relative, field="texture path")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(texture.path, target)
            written[package] = relative
        return written[package]

    recipes: list[dict[str, Any]] = []
    for code, (rule, soft_paths) in plan.items():
        packages = [_package_and_object(soft_path)[0] for soft_path in soft_paths]
        failure = next((outcomes[package] for package in packages if not isinstance(outcomes[package], Texture)), None)
        if failure is not None:
            reason = "missing-texture" if isinstance(failure, Missing) else "unsupported-pixel-format"
            assert isinstance(failure, Missing | Unsupported)
            entry = {"code": code, "kind": "item", "reason": reason, "rule": rule, "detail": failure.detail}
            unmatched.append(entry)
            continue
        layers = []
        flags = []
        for package in packages:
            texture = outcomes[package]
            assert isinstance(texture, Texture)
            layers.append({"image": image_for(package, texture)})
            if texture.format in MASK_FORMATS:
                flags.append({"flag": "mask-texture", "detail": f"{package} {texture.format}"[:DETAIL_LIMIT]})
        recipe: dict[str, Any] = {"code": code, "kind": "item", "rule": rule, "layers": layers, "composite": "over"}
        if flags:
            recipe["flags"] = flags
        recipes.append(recipe)

    matched = {recipe["code"]: recipe for recipe in recipes}
    candidate_for_primary = {primary: alternative for alternative, primary in candidate_codes.items()}
    for alternative, primary in sorted(candidate_codes.items()):
        base = matched.get(primary)
        if base is not None:
            recipes.append(
                {
                    "code": alternative,
                    "kind": "item",
                    "rule": base["rule"],
                    "candidateOf": primary,
                    "layers": base["layers"],
                    "composite": "over",
                }
            )
    for entry in unmatched:
        alternative = candidate_for_primary.get(entry["code"])
        if alternative is not None:
            detail = f"{entry['detail']}; " if entry.get("detail") else ""
            entry["detail"] = f"{detail}candidate code {alternative}"[:DETAIL_LIMIT]
    unmatched.sort(key=lambda entry: entry["code"])

    codes_from = (
        f"Item codes derived from game data: every listed ItemTableRow row across {len(item_tables)} item tables "
        f"(base {BASE_ITEM_TABLE}, DLC tables, Special_ItemTable_Composite) of server depot app "
        f"{ctx.codes_from_inputs['app']} build {ctx.codes_from_inputs['buildid']}; code = the base "
        f"ItemNameToTemplateID row name, otherwise the template id; IsInternalName rows excluded ({internal}). "
        "DLC items also carry their other candidate code until the live code list settles which one the connector "
        "reports. Not measured against a live code list from a staging server (#811 skipped)."
    )
    document: dict[str, Any] = {
        "schemaVersion": 1,
        "game": "conan-exiles",
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
            "tables": table_record + name_record,
            "internalExcluded": internal,
        },
    )
