#!/usr/bin/env python3
"""Trim real decoder output to the rows the fixtures need. Run by hand after a live build.

    record.py DECODED_DIR [DECODED_DIR ...]

Each ``DECODED_DIR`` is a directory of the decode cache (``<cache>/icons/decoded/<key>/``) holding the
decoder's ``manifest.json`` and ``json/*.json``. The tables and the AssetRegistry excerpt are found by their
``Package`` and written to ``decoder/json/*.json``, with only the fields the mapper reads (``Name``,
``Icon``, ``IconLayers``) kept on item rows. A few rows the live data does not contain (an empty ``Name``,
a ``dev_`` code, a non-integer row name, two names for one id, a DLC id that the base name table also
names, a composite row that disagrees with a plain table) are added by hand and listed under ``synthetic``
in ``selection.json``. ``make_fixture.py`` reads the files written here.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
BASE_ITEMS = "/Game/Items/ItemTable"
BASE_NAMES = "/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToTemplateID"
DLC_ITEMS = "/Game/DLC/DLC_Pict/DLC_Pict_ItemTable"
DLC_NAMES = "/Game/DLC/DLC_Pict/DLC_Pict_ItemNameToTemplateID"
COMPOSITE = "/Game/DLC/Special/Special_ItemTable_Composite"
SPECIAL_ITEMS = "/Game/DLC/Special/Special_ItemTable"
HARVEST = "/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToHarvestXPValue"
STORE = "/Game/DLC/Special/Store/Data/StoreItems"
WANTED = [BASE_ITEMS, BASE_NAMES, DLC_ITEMS, DLC_NAMES, COMPOSITE, SPECIAL_ITEMS, HARVEST, STORE]
ITEM_FIELDS = ("Name", "Icon", "IconLayers")


def load(directories: list[Path]) -> tuple[dict[str, Any], dict[str, Any]]:
    tables: dict[str, Any] = {}
    registry: dict[str, Any] = {}
    for directory in directories:
        manifest = json.loads((directory / "manifest.json").read_text())
        for result in manifest["results"]:
            if not result["ok"] or not result["file"].endswith(".json"):
                continue
            document = json.loads((directory / result["file"]).read_text())
            if isinstance(document, dict) and "assets" in document:
                registry = document
            elif isinstance(document, list) and document and document[0].get("Package") in WANTED:
                tables[document[0]["Package"]] = document[0]
    return tables, registry


def text(value: Any) -> str:
    return str(value.get("SourceString", "")) if isinstance(value, dict) else str(value)


def is_internal(name: str) -> bool:
    stripped = name.lstrip(" \t\n\v\f\r")
    if not stripped:
        return True
    lowered = stripped.lower()
    xs = len(lowered) - len(lowered.lstrip("x"))
    return (xs >= 2 and xs < len(lowered) and lowered[xs] in " _") or lowered.startswith(
        ("dev ", "dev_", "test ", "test_", "deprecated", "do not use")
    )


def template_of(row: dict[str, Any]) -> int:
    return next(value for value in row.values() if isinstance(value, int))


def trim(row: dict[str, Any]) -> dict[str, Any]:
    return {field: row[field] for field in ITEM_FIELDS}


def pick(rows: list[str], count: int) -> list[str]:
    """``count`` evenly spaced entries, so the choice is stable and spread over the table."""
    return [rows[index * len(rows) // count] for index in range(count)]


def main(argv: list[str]) -> int:
    tables, registry = load([Path(arg) for arg in argv])
    missing = [package for package in WANTED if package not in tables]
    if missing or not registry:
        print(f"missing from the decoded directories: {missing or 'asset registry'}", file=sys.stderr)
        return 1
    base = tables[BASE_ITEMS]["Rows"]
    base_names = tables[BASE_NAMES]["Rows"]
    name_for: dict[int, str] = {}
    for row_name, row in base_names.items():
        name_for.setdefault(template_of(row), row_name)

    def code_of(row_name: str) -> str:
        return name_for.get(int(row_name), row_name)

    listed = [
        row_name
        for row_name, row in base.items()
        if row_name.isdigit() and not is_internal(text(row["Name"]).strip()) and not is_internal(code_of(row_name))
    ]
    plain = [
        name
        for name in listed
        if base[name]["Icon"]["AssetPathName"] and "battlepass" not in base[name]["Icon"]["AssetPathName"]
    ]
    layered = [name for name in listed if not base[name]["Icon"]["AssetPathName"] and base[name]["IconLayers"]]
    bare = [name for name in listed if not base[name]["Icon"]["AssetPathName"] and not base[name]["IconLayers"]]
    battle = [name for name in listed if "battlepass" in base[name]["Icon"]["AssetPathName"]]
    internal = [name for name in base if name.isdigit() and text(base[name]["Name"]).strip().lower().startswith("xx_")]

    roles = {
        "icon": pick(plain, 3),
        "layers": pick(layered, 2),
        "none": bare[:1],
        "internal-name": internal[:1],
        "battlepass": battle[:1],
        "mask": pick(plain, 7)[4:5],
        "unsupported": pick(plain, 7)[5:6],
        "missing": pick(plain, 7)[6:7],
        "small": pick(plain, 7)[3:4],
    }
    chosen = [name for names in roles.values() for name in names]
    assert len(set(chosen)) == len(chosen), "role picks overlap"

    base_rows = {name: trim(base[name]) for name in chosen}
    synthetic: dict[str, list[str]] = {"rows": [], "nameRows": []}
    plain_row = base_rows[roles["icon"][0]]
    base_rows["9000001"] = {**plain_row, "Name": {**plain_row["Name"], "SourceString": "Dev Probe"}}
    base_rows["9000002"] = {**plain_row, "Name": {**plain_row["Name"], "SourceString": ""}}
    base_rows["NotATemplateId"] = dict(plain_row)
    synthetic["rows"] += ["9000001", "9000002", "NotATemplateId"]
    if not roles["none"]:
        base_rows["9000003"] = {
            **plain_row,
            "Name": {**plain_row["Name"], "SourceString": "Placeholder Without Icon"},
            "Icon": {"AssetPathName": "", "SubPathString": ""},
            "IconLayers": [],
        }
        synthetic["rows"].append("9000003")
        roles["none"] = ["9000003"]
    roles["internal-code"] = ["9000001"]
    roles["empty-name"] = ["9000002"]
    roles["non-integer"] = ["NotATemplateId"]

    name_rows = {name_for[int(name)]: base_names[name_for[int(name)]] for name in chosen if int(name) in name_for}
    struct_member = next(key for key in next(iter(base_names.values())))
    first_id = int(roles["icon"][0])
    name_rows["Dev_Probe"] = {struct_member: 9000001}
    name_rows["Duplicate_Name_For_First_Icon"] = {struct_member: first_id}
    synthetic["nameRows"] += ["Dev_Probe", "Duplicate_Name_For_First_Icon"]

    dlc = tables[DLC_ITEMS]["Rows"]
    dlc_names = tables[DLC_NAMES]["Rows"]
    dlc_named_ids = {template_of(row) for row in dlc_names.values()}
    in_dlc_names = [
        name
        for name in dlc
        if name.isdigit() and int(name) in dlc_named_ids and not is_internal(text(dlc[name]["Name"]))
    ]
    in_no_names = [
        name
        for name in dlc
        if name.isdigit() and int(name) not in dlc_named_ids and not is_internal(text(dlc[name]["Name"]))
    ]
    dlc_a, dlc_b = pick(in_dlc_names, 2)
    dlc_c = in_no_names[0]
    dlc_rows = {name: trim(dlc[name]) for name in (dlc_a, dlc_b, dlc_c)}
    dlc_name_rows = {
        row_name: row for row_name, row in dlc_names.items() if template_of(row) in {int(dlc_a), int(dlc_b)}
    }
    # dlc_b is also named by the base name table, which is what makes its primary code a name.
    name_rows["Base_Table_Name_For_Dlc_Item"] = {struct_member: int(dlc_b)}
    synthetic["nameRows"].append("Base_Table_Name_For_Dlc_Item")
    roles["dlc"] = [dlc_a, dlc_b, dlc_c]

    special = tables[SPECIAL_ITEMS]["Rows"]
    special_name = next(name for name in special if name.isdigit() and not is_internal(text(special[name]["Name"])))
    clash = {**trim(dlc[dlc_a]), "Icon": trim(dlc[dlc_c])["Icon"]}
    composite_rows = {special_name: trim(special[special_name]), dlc_a: clash}
    synthetic["rows"].append(f"{COMPOSITE}:{dlc_a}")
    roles["composite"] = [special_name, dlc_a]

    out = HERE / "decoder" / "json"
    out.mkdir(parents=True, exist_ok=True)

    def table(package: str, rows: dict[str, Any], file: str, extra: dict[str, Any] | None = None) -> None:
        source = tables[package]
        document = [
            {
                "Type": source["Type"],
                "Name": source["Name"],
                "Class": source["Class"],
                "Package": package,
                "Properties": {
                    **{k: v for k, v in source["Properties"].items() if k != "ParentTables"},
                    **(extra or {}),
                },
                "Rows": rows,
            }
        ]
        (out / file).write_text(json.dumps(document, indent=2) + "\n")

    table(BASE_ITEMS, base_rows, "item-table.json")
    table(BASE_NAMES, name_rows, "name-table.json")
    table(DLC_ITEMS, dlc_rows, "dlc-item-table.json")
    table(DLC_NAMES, dlc_name_rows, "dlc-name-table.json")
    table(
        COMPOSITE,
        composite_rows,
        "composite-item-table.json",
        {"ParentTables": [{"ObjectName": "DataTable'Special_ItemTable'", "ObjectPath": f"{SPECIAL_ITEMS}.0"}]},
    )
    kept = {BASE_ITEMS, BASE_NAMES, DLC_ITEMS, DLC_NAMES, COMPOSITE, HARVEST, STORE}
    excerpt = {
        "registries": registry.get("registries", []),
        "assets": [asset for asset in registry["assets"] if asset["packageName"] in kept],
    }
    (HERE / "decoder" / "registry.json").write_text(json.dumps(excerpt, indent=2) + "\n")
    (HERE / "selection.json").write_text(json.dumps({"roles": roles, "synthetic": synthetic}, indent=2) + "\n")
    print(f"wrote {len(base_rows)} base rows, {len(dlc_rows)} DLC rows, {len(composite_rows)} composite rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
