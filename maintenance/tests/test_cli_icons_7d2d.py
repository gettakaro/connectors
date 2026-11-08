"""``icons build --game 7d2d`` and its inputs, end to end: server-depot pixels and the tint-multiply op.

The server depot is the DepotDownloader stand-in serving ``fixtures/icons/7d2d/depots``; there is no client.
Every assertion is on exit codes, the JSON on stdout and the files the command wrote.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import sys
from pathlib import Path
from typing import Any

import pytest
from PIL import Image

import fake_depotdownloader as fake
from conftest import FIXTURES, REPO_ROOT
from takaro_maint import paths
from takaro_maint.icons import contract
from takaro_maint.steam.install import COMPLETE_MARKER

FIXTURE = FIXTURES / "icons" / "7d2d"
MANIFEST = "7440000000000000745"
TARGET = "linux-3.3.0.b18"
PIXEL_SELECTOR = "regex:^Data/ItemIcons/[^/]+\\.png$"
DATA_SELECTORS = [
    "Data/Config/items.xml",
    "Data/Config/item_modifiers.xml",
    "Data/Config/blocks.xml",
    "Data/Config/Localization.csv",
]
CONFIG = "Data/Config"


def _load_maker() -> Any:
    spec = importlib.util.spec_from_file_location("seven_days_make_fixture", FIXTURE / "make_fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


maker = _load_maker()

# code -> (kind, rule); every localized entry the fixture lists, matched or not.
EXPECTED: dict[str, tuple[str, str]] = {
    "itemOwnIcon": ("item", "own-icon"),
    "itemNameFallback": ("item", "name"),
    "parentTinted": ("item", "own-icon-tinted"),
    "childInherits": ("item", "inherited-icon-tinted"),
    "grandchild": ("item", "inherited-icon-tinted"),
    "childExcludesIcon": ("item", "name-tinted"),
    "childExcludesBoth": ("item", "name"),
    "childOverrides": ("item", "own-icon-tinted"),
    "tintHex": ("item", "own-icon-tinted"),
    "tintRgb": ("item", "own-icon-tinted"),
    "tintRgba": ("item", "own-icon-tinted"),
    "tintHexAlpha": ("item", "own-icon-tinted"),
    "tintWhite": ("item", "own-icon"),
    "tintNone": ("item", "own-icon"),
    "tintBad": ("item", "own-icon"),
    "itemMissingIcon": ("item", "own-icon"),
    "itemCaseIcon": ("item", "own-icon"),
    "itemNestedDecoy": ("item", "name"),
    "itemOrphan": ("item", "name"),
    "itemCycleA": ("item", "name"),
    "itemCycleB": ("item", "name"),
    "itemBigIcon": ("item", "own-icon"),
    "modByName": ("item", "name"),
    "blockOwnIcon": ("block", "own-icon"),
    "blockNameFallback": ("block", "name"),
    "blockTinted": ("block", "own-icon-tinted"),
    "blockExcludes": ("block", "name-tinted"),
    "blockExtendsItem": ("block", "name"),
}
UNMATCHED = {"tintBad": "invalid-tint", "itemMissingIcon": "missing-texture"}
EXCLUDED = {
    "itemUnlocalized",
    "itemKeyAsValue",
    "itemEmptyEnglish",
    "modUnlocalized",
    "blockUnlocalized",
}
MATCHED = set(EXPECTED) - set(UNMATCHED)
GREY_OPAQUE = (4, 8)
GREY_HALF_ALPHA = (12, 8)


def make_repo(tmp_path: Path) -> Path:
    """The real catalog and tool lock, with the 7d2d server input pinned at the fixture depot."""
    root = tmp_path / "repo"
    shutil.copytree(REPO_ROOT / "catalog", root / "catalog")
    (root / "maintenance").mkdir()
    shutil.copy2(REPO_ROOT / "maintenance" / "tools.lock.json", root / "maintenance" / "tools.lock.json")
    path = root / "catalog" / "7d2d" / "targets" / f"{TARGET}.json"
    record = json.loads(path.read_text())
    record["inputs"]["server"]["depots"]["294422"]["manifest"] = MANIFEST
    path.write_text(json.dumps(record, indent=2) + "\n")
    return root


def edit_game(root: Path, change: Any) -> None:
    path = root / "catalog" / "7d2d" / "game.json"
    record = json.loads(path.read_text())
    change(record["icons"])
    path.write_text(json.dumps(record, indent=2) + "\n")


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    return make_repo(tmp_path)


@pytest.fixture
def depots(tmp_path: Path) -> Path:
    """A writable copy of the fixture depot, for the tests that damage it."""
    destination = tmp_path / "depots"
    shutil.copytree(FIXTURE / "depots", destination)
    return destination


@pytest.fixture
def dd_log(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    log = tmp_path / "depotdownloader-argv.jsonl"
    monkeypatch.setenv("TAKARO_MAINT_DEPOTDOWNLOADER", str(Path(fake.__file__).resolve()))
    monkeypatch.setenv("FAKE_DD_ROOT", str(FIXTURE / "depots"))
    monkeypatch.setenv("FAKE_DD_LOG", str(log))
    return log


def build(run: Any, repo: Path, out: Path, *extra: str) -> tuple[int, Any, str]:
    return run("icons", "build", "--game", "7d2d", "--out", str(out), *extra, repo=repo)


def downloads(log: Path) -> list[list[str]]:
    return [argv for argv in fake.argv_log(log) if "-filelist" in argv]


def tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def pixel(path: Path, point: tuple[int, int]) -> tuple[int, ...]:
    with Image.open(path) as image:
        return image.convert("RGBA").getpixel(point)  # type: ignore[return-value]


def index_of(out: Path) -> dict[str, str]:
    return json.loads((out / "pack" / "7d2d" / "index.json").read_text())["icons"]


def icon_file(out: Path, code: str) -> Path:
    return out / "pack" / "7d2d" / index_of(out)[code]


def tinted(rgb: tuple[int, int, int], tint: tuple[int, int, int]) -> tuple[int, int, int]:
    """The pinned tint-multiply formula: per channel ``c * t // 255``."""
    return tuple(c * t // 255 for c, t in zip(rgb, tint, strict=True))  # type: ignore[return-value]


# ------------------------------------------------------------------ catalog and inputs


def test_the_real_catalog_validates(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")
    assert code == 0, payload
    failing = [check for check in payload["checks"] if check["status"] == "fail"]
    assert failing == []
    files = {check["file"] for check in payload["checks"] if check["id"].startswith("icons-")}
    assert "catalog/7d2d/game.json" in files


def test_inputs_lists_server_depot_pixels_without_any_network(run: Any, repo: Path, dd_log: Path) -> None:
    code, payload, _ = run("icons", "inputs", "--game", "7d2d", repo=repo)
    assert code == 0, payload
    assert (payload["game"], payload["packGame"], payload["target"], payload["mapper"]) == (
        "7d2d",
        "7d2d",
        TARGET,
        "7d2d",
    )
    data, pixels = payload["inputs"]
    assert data["role"] == "data" and data["app"] == 294420 and data["files"] == DATA_SELECTORS
    assert list(data["depots"]) == ["294422"] and data["cached"] is False
    assert pixels["role"] == "pixels" and pixels["source"] == "server-depot"
    assert pixels["files"] == [PIXEL_SELECTOR] and pixels["depots"] == data["depots"]
    assert pixels["cache"] == data["cache"] and pixels["cached"] is False
    assert not {"app", "depot", "decoder", "clientManifest", "expectedClientBuilds", "suggestedMirror"} & set(pixels)
    assert fake.argv_log(dd_log) == []


def test_inputs_refuses_a_game_without_an_icons_block(run: Any) -> None:
    code, _, stderr = run("icons", "inputs", "--game", "rust")
    assert code == 2 and "game 'rust' has no icons block" in stderr


@pytest.mark.parametrize(
    ("change", "check"),
    [
        (lambda block: block["pixels"].update(app=294420), "game-schema"),
        (lambda block: block["pixels"].update(clientBuilds={"3.3.0.b18": [5]}), "game-schema"),
        (lambda block: block["pixels"].update(decoder="unreal"), "game-schema"),
        (lambda block: block["pixels"].pop("files"), "game-schema"),
        (lambda block: block["pixels"].update(source="floppy"), "game-schema"),
    ],
    ids=["app", "client-builds", "decoder", "no-files", "unknown-source"],
)
def test_a_bad_server_depot_pixels_block_fails_validation(
    run: Any, catalog_copy: Path, change: Any, check: str
) -> None:
    edit_game(catalog_copy, change)
    code, payload, _ = run("catalog", "validate", repo=catalog_copy)
    assert code == 2
    assert check in {entry["id"] for entry in payload["checks"] if entry["status"] == "fail"}


# ------------------------------------------------------------------ the build


def test_the_build_maps_game_data_to_a_pack(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    code, payload, stderr = build(run, repo, out)
    assert code == 0, payload
    assert payload["game"] == "7d2d" and payload["catalogGame"] == "7d2d" and payload["target"] == TARGET
    assert "clientBuild" not in payload and payload["warnings"] == [] and "warning" not in stderr
    assert payload["inputs"]["pixels"] == {"source": "server-depot", "files": 17}
    assert "decoder" not in payload

    assert set(index_of(out)) == MATCHED
    coverage = json.loads((out / "coverage.json").read_text())
    assert (coverage["total"], coverage["matched"], coverage["unmatched"]) == (len(EXPECTED), len(MATCHED), 2)
    listed = {row["code"] for row in coverage["unmatchedCodes"]}
    assert listed == set(UNMATCHED)
    assert not EXCLUDED & (set(index_of(out)) | listed)
    assert coverage["unmatchedByReason"] == {"invalid-tint": 1, "missing-texture": 1}
    kinds = {"item": 0, "block": 0}
    for kind, _ in EXPECTED.values():
        kinds[kind] += 1
    assert {kind: row["total"] for kind, row in coverage["byKind"].items()} == kinds
    by_rule: dict[str, dict[str, int]] = {}
    for code_name, (_, rule) in EXPECTED.items():
        row = by_rule.setdefault(rule, {"total": 0, "matched": 0, "unmatched": 0})
        row["total"] += 1
        row["matched" if code_name in MATCHED else "unmatched"] += 1
    assert coverage["byRule"] == by_rule
    details = {row["code"]: row.get("detail") for row in coverage["unmatchedCodes"]}
    assert details == {"tintBad": "notacolour", "itemMissingIcon": "nope.png"}
    assert coverage["measuredAgainst"] == "game-data"
    assert "#811" in coverage["codesFrom"] and "build 25661908" in coverage["codesFrom"]
    assert "item_modifiers.xml" in coverage["codesFrom"] and "localization" in coverage["codesFrom"]
    assert coverage["flaggedCodes"] == [{"code": "itemCaseIcon", "flag": "case-mismatch", "detail": "iconA.png"}]

    recipes = {row["code"]: row for row in json.loads((out / "recipes.json").read_text())["recipes"]}
    assert {code_name: (row["kind"], row["rule"]) for code_name, row in recipes.items()} == {
        code_name: EXPECTED[code_name] for code_name in MATCHED
    }
    assert (out / "contact-sheet.png").is_file()
    assert list((paths.cache_dir() / "icons" / "work").iterdir()) == []
    assert sorted(path.name for path in out.rglob("*.png")) == ["contact-sheet.png"]


def test_icon_art_comes_from_the_right_png(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, repo, out)[0] == 0
    colour = maker.colour
    assert pixel(icon_file(out, "itemOwnIcon"), (4, 4)) == (*colour("iconA"), 255)  # the first definition wins
    assert pixel(icon_file(out, "itemCaseIcon"), (4, 4)) == (*colour("iconA"), 255)  # a casefold-only file hit
    assert pixel(icon_file(out, "itemNameFallback"), (4, 4)) == (*colour("itemNameFallback"), 255)
    assert pixel(icon_file(out, "itemNestedDecoy"), (4, 4)) == (*colour("itemNestedDecoy"), 255)  # nested ignored
    assert pixel(icon_file(out, "blockNameFallback"), (4, 4)) == (*colour("blockNameFallback"), 255)
    assert pixel(icon_file(out, "modByName"), (4, 4)) == (*colour("modByName"), 255)
    assert pixel(icon_file(out, "blockOwnIcon"), (4, 4)) == (*colour("blockArt"), 255)
    with Image.open(icon_file(out, "itemBigIcon")) as image:
        assert image.size == (contract.MAX_EDGE, contract.MAX_EDGE)
    assert pixel(icon_file(out, "itemBigIcon"), (4, 4)) == (*colour(maker.BIG), 255)


def test_a_tint_multiplies_the_sprite_and_keeps_its_alpha(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, repo, out)[0] == 0
    grey = maker.TINT_BASE_GREY
    assert tinted(grey, (0x80, 0xFF, 0x40)) == (64, 128, 32)
    for code in ("tintHex", "tintRgb", "tintRgba", "tintHexAlpha"):
        assert pixel(icon_file(out, code), GREY_OPAQUE) == (64, 128, 32, 255), code
        assert pixel(icon_file(out, code), GREY_HALF_ALPHA) == (64, 128, 32, maker.HALF_ALPHA), code
    assert pixel(icon_file(out, "tintNone"), GREY_OPAQUE) == (*grey, 255)
    assert pixel(icon_file(out, "tintNone"), GREY_HALF_ALPHA) == (*grey, maker.HALF_ALPHA)
    assert pixel(icon_file(out, "blockTinted"), (4, 4)) == (0, maker.colour("blockArt")[1], 0, 255)


def test_inheritance_edge_cases_by_colour(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, repo, out)[0] == 0
    parent, red = maker.colour("iconParent"), (255, 0, 0)
    assert pixel(icon_file(out, "parentTinted"), (4, 4)) == (*tinted(parent, red), 255)
    assert pixel(icon_file(out, "childInherits"), (4, 4)) == (*tinted(parent, red), 255)
    assert pixel(icon_file(out, "grandchild"), (4, 4)) == (*tinted(parent, red), 255)  # two levels
    # param1 excludes the icon only: the entry's own name, with the tint still inherited
    assert pixel(icon_file(out, "childExcludesIcon"), (4, 4)) == (*tinted(maker.colour("childExcludesIcon"), red), 255)
    # param1 lists both properties (with a space after the comma): the own name, untinted
    assert pixel(icon_file(out, "childExcludesBoth"), (4, 4)) == (*maker.colour("childExcludesBoth"), 255)
    # an own icon beats the inherited one, and the tint is still inherited
    assert pixel(icon_file(out, "childOverrides"), (4, 4)) == (*tinted(maker.colour("iconB"), red), 255)
    # a block excluding CustomIcon falls to its own name and keeps the inherited tint
    assert pixel(icon_file(out, "blockExcludes"), (4, 4)) == (*tinted(maker.colour("blockExcludes"), (0, 255, 0)), 255)
    # Extends resolves inside its own file, an unknown parent and a cycle both end at the entry's own name
    for code in ("itemOrphan", "itemCycleA", "itemCycleB", "blockExtendsItem"):
        assert pixel(icon_file(out, code), (4, 4)) == (*maker.colour(code), 255), code


def test_both_tint_formats_give_the_same_object_and_white_gives_none(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    out = tmp_path / "out"
    assert build(run, repo, out)[0] == 0
    index = index_of(out)
    assert len({index[code] for code in ("tintHex", "tintRgb", "tintRgba", "tintHexAlpha")}) == 1
    assert index["tintWhite"] == index["tintNone"] and index["tintWhite"] != index["tintHex"]
    recipes = {row["code"]: row for row in json.loads((out / "recipes.json").read_text())["recipes"]}
    assert recipes["tintRgb"]["layers"][0]["ops"] == [{"op": "tint-multiply", "color": "#80ff40"}]
    assert recipes["tintWhite"]["layers"][0]["ops"] == []


def test_the_build_record_names_what_was_built_from(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    code, payload, _ = build(run, repo, out)
    assert code == 0
    record = json.loads((out / "build-record.json").read_text())
    assert record["source"] == "catalog" and record["game"] == "7d2d" and record["catalogGame"] == "7d2d"
    assert "client" not in record and record["warnings"] == []
    assert record["target"] == TARGET and record["fingerprint"] == payload["fingerprint"]
    assert record["recipesSha256"] == hashlib.sha256((out / "recipes.json").read_bytes()).hexdigest()

    tree_root = FIXTURE / "depots" / "294422" / MANIFEST / "tree"
    kept = sorted([*DATA_SELECTORS, *(f"Data/ItemIcons/{name}" for name in maker.icons())])
    listing = "".join(f"{name}\t{hashlib.sha256((tree_root / name).read_bytes()).hexdigest()}\n" for name in kept)
    data = record["data"]
    assert data["selectors"] == [*DATA_SELECTORS, PIXEL_SELECTOR]
    assert data["depots"] == {"294422": MANIFEST} and data["files"] == len(kept)
    assert data["treeSha256"] == hashlib.sha256(listing.encode()).hexdigest()
    for decoy in ("Data/ItemIcons/sub/nested.png", "Data/ItemIcons/readme.txt", "Data/Config/notes.xml"):
        assert decoy not in kept

    assert record["pixels"] == {
        "source": "server-depot",
        "selectors": [PIXEL_SELECTOR],
        "files": 17,
        "used": 17,
    }
    assert record["tools"]["decoder"] == "none (PNG and XML read in-process)"
    mapper = record["mapper"]
    assert mapper["name"] == "7d2d" and mapper["version"] == 1
    assert mapper["sources"] == {"items": 26, "itemModifiers": 2, "blocks": 7}
    assert mapper["excluded"] == {"items": 3, "itemModifiers": 1, "blocks": 1}
    assert mapper["icons"] == {"files": 17, "used": 17}
    notes = " ".join(mapper["notes"])
    assert "3 items entries left out" in notes and "1 blocks entries left out" in notes
    assert "2 repeated names" in notes and "2 entries extend an entry that is not in their file" in notes
    assert "1 Extends cycles met" in notes and "1 icons found only by a case-insensitive" in notes


def test_the_client_flag_is_refused_before_any_download(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    code, payload, stderr = build(run, repo, tmp_path / "out", "--client", str(tmp_path))
    assert code == 2
    assert "game 7d2d takes its pixels from the server depot; --client is not used" in stderr + json.dumps(payload)
    assert fake.argv_log(dd_log) == [] and not (tmp_path / "out").exists()


def test_one_filtered_download_serves_data_and_pixels_and_is_cached(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    assert build(run, repo, tmp_path / "one")[0] == 0
    assert build(run, repo, tmp_path / "two")[0] == 0
    assert len(downloads(dd_log)) == 1
    cache = paths.cache_dir() / "icons" / "steam" / "294420" / "294422" / MANIFEST
    [filtered] = cache.iterdir()
    marker = json.loads((filtered / COMPLETE_MARKER).read_text())
    assert marker["selectors"] == [*DATA_SELECTORS, PIXEL_SELECTOR]
    assert sorted(marker["files"]) == sorted([*DATA_SELECTORS, *(f"Data/ItemIcons/{name}" for name in maker.icons())])
    assert tree(tmp_path / "one") == tree(tmp_path / "two")


# ------------------------------------------------------------------ refusals


def _damage(depots: Path, relative: str, change: Any) -> None:
    path = depots / "294422" / MANIFEST / "tree" / relative
    path.write_bytes(change(path.read_bytes()))


def _use_depots(monkeypatch: pytest.MonkeyPatch, depots: Path) -> None:
    monkeypatch.setenv("FAKE_DD_ROOT", str(depots))


def test_a_doctype_is_refused_and_writes_no_pack(
    run: Any, repo: Path, dd_log: Path, depots: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _damage(
        depots,
        f"{CONFIG}/items.xml",
        lambda raw: raw.replace(b"<items>", b'<!DOCTYPE items [<!ENTITY a "b">]>\n<items>', 1),
    )
    _use_depots(monkeypatch, depots)
    code, payload, _ = build(run, repo, tmp_path / "out")
    assert code == 6, payload
    assert payload["file"] == f"{CONFIG}/items.xml"
    assert not (tmp_path / "out" / "pack").exists()


def test_malformed_xml_is_refused(
    run: Any, repo: Path, dd_log: Path, depots: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _damage(depots, f"{CONFIG}/blocks.xml", lambda raw: raw.replace(b"</blocks>", b""))
    _use_depots(monkeypatch, depots)
    code, payload, _ = build(run, repo, tmp_path / "out")
    assert code == 6 and payload["file"] == f"{CONFIG}/blocks.xml"


def test_a_localization_file_without_an_english_column_is_refused(
    run: Any, repo: Path, dd_log: Path, depots: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _damage(depots, f"{CONFIG}/Localization.csv", lambda raw: raw.replace(b",english,", b",englisch,", 1))
    _use_depots(monkeypatch, depots)
    code, payload, _ = build(run, repo, tmp_path / "out")
    assert code == 6 and "english" in payload["error"]


def test_a_missing_data_file_is_upstream_unavailable_and_named(
    run: Any, repo: Path, dd_log: Path, depots: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (depots / "294422" / MANIFEST / "tree" / CONFIG / "item_modifiers.xml").unlink()
    _use_depots(monkeypatch, depots)
    code, payload, _ = build(run, repo, tmp_path / "out")
    assert code == 4 and f"{CONFIG}/item_modifiers.xml" in payload["error"]


def test_a_pinned_manifest_steam_will_not_serve_asks_for_a_re_pin(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", MANIFEST)
    code, payload, stderr = build(run, repo, tmp_path / "out")
    assert code == 4
    assert f"steam pin --game 7d2d --target {TARGET} --write" in stderr + payload["error"]
    assert not (tmp_path / "out").exists()


def test_the_committed_fixture_is_what_the_generator_writes(tmp_path: Path) -> None:
    maker.write(tmp_path)
    committed = {
        path.relative_to(FIXTURE).as_posix(): path.read_bytes()
        for path in (FIXTURE / "depots").rglob("*")
        if path.is_file()
    }
    regenerated = {
        path.relative_to(tmp_path).as_posix(): path.read_bytes()
        for path in (tmp_path / "depots").rglob("*")
        if path.is_file()
    }
    assert regenerated == committed
    for name in ("items.xml", "item_modifiers.xml", "blocks.xml", "Localization.csv"):
        assert (tmp_path / maker.TREE / CONFIG / name).read_bytes().startswith(maker.BOM)
