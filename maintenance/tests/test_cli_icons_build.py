"""``takaro-maint icons build``: recipes in, a contract pack and its reports out."""

from __future__ import annotations

import hashlib
import json
import re
import shutil
from pathlib import Path
from typing import Any

import pytest
from PIL import Image

from conftest import FIXTURES

FIXTURE = FIXTURES / "icons" / "fixture-846"
OBJECT = re.compile(r"^[0-9a-f]{64}\.webp$")


def _fixture_copy(tmp_path: Path) -> Path:
    copy = tmp_path / "fixture"
    shutil.copytree(FIXTURE, copy)
    return copy


def _build(run: Any, tmp_path: Path, name: str = "out", recipes: Path | None = None) -> tuple[int, Any, str, Path]:
    out = tmp_path / name
    code, payload, stderr = run(
        "icons", "build", "--recipes", str(recipes or FIXTURE / "recipes.json"), "--out", str(out)
    )
    return code, payload, stderr, out


def _riff_chunks(data: bytes) -> list[str]:
    assert data[:4] == b"RIFF" and data[8:12] == b"WEBP"
    chunks, offset = [], 12
    while offset < len(data):
        chunks.append(data[offset : offset + 4].decode("ascii"))
        offset += 8 + int.from_bytes(data[offset + 4 : offset + 8], "little") + (offset + 8) % 2
    return chunks


def _tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def test_fixture_builds_a_contract_pack(run: Any, tmp_path: Path) -> None:
    code, payload, _, out = _build(run, tmp_path)
    assert code == 0
    assert payload["ok"] is True and payload["op"] == "icons build"
    pack = out / "pack" / "fixture-846"
    assert sorted(path.name for path in pack.iterdir() if not path.name.endswith(".webp")) == ["index.json"]
    webps = sorted(pack.glob("*.webp"))
    assert len(webps) == 5

    index = json.loads((pack / "index.json").read_text())
    assert set(index) == {"game", "enabled", "icons"}
    assert index["game"] == "fixture-846" and index["enabled"] is True
    for name in index["icons"].values():
        assert OBJECT.match(name)
        assert hashlib.sha256((pack / name).read_bytes()).hexdigest() + ".webp" == name

    sizes = {}
    for code_name, name in index["icons"].items():
        with Image.open(pack / name) as image:
            assert image.format == "WEBP" and image.mode == "RGBA"
            sizes[code_name] = image.size
        assert not {"EXIF", "ICCP", "XMP "} & set(_riff_chunks((pack / name).read_bytes()))
    assert sizes["fixture-circle"] == (128, 128)
    assert sizes["fixture-banner"] == (128, 64)
    assert sizes["fixture-pillar"] == (32, 128)
    assert sizes["fixture-pebble"] == (32, 32)
    assert sizes["fixture-crate"] == (64, 64)


def test_build_is_deterministic(run: Any, tmp_path: Path) -> None:
    assert _build(run, tmp_path, "one")[0] == 0
    assert _build(run, tmp_path, "two")[0] == 0
    assert _tree(tmp_path / "one" / "pack") == _tree(tmp_path / "two" / "pack")


def test_identical_art_is_stored_once(run: Any, tmp_path: Path) -> None:
    _, payload, _, out = _build(run, tmp_path)
    index = json.loads((out / "pack/fixture-846/index.json").read_text())
    assert index["icons"]["fixture-circle"] == index["icons"]["fixture-circle-alias"]
    assert (payload["codes"], payload["objects"]) == (6, 5)


def test_coverage_report_lists_unmatched_codes_with_reasons(run: Any, tmp_path: Path) -> None:
    _, payload, _, out = _build(run, tmp_path)
    coverage = json.loads((out / "coverage.json").read_text())
    assert coverage["game"] == "fixture-846"
    assert (coverage["total"], coverage["matched"], coverage["unmatched"], coverage["coverage"]) == (8, 6, 2, 0.75)
    assert coverage["objects"] == 5
    assert coverage["measuredAgainst"] == "game-data"
    assert coverage["byKind"] == {
        "block": {"total": 2, "matched": 1, "unmatched": 1},
        "item": {"total": 6, "matched": 5, "unmatched": 1},
    }
    assert coverage["unmatchedByReason"] == {"missing-texture": 1, "no-icon-in-game": 1}
    assert coverage["unmatchedCodes"] == [
        {"code": "fixture-broken", "kind": "block", "reason": "missing-texture"},
        {"code": "fixture-ghost", "kind": "item", "reason": "no-icon-in-game"},
    ]
    assert payload["coverage"] == {"total": 8, "matched": 6, "unmatched": 2, "coverage": 0.75}
    assert not (out / "pack/fixture-846/coverage.json").exists()
    assert not (out / "pack/fixture-846/contact-sheet.png").exists()
    assert sorted(path.name for path in (out / "pack").iterdir()) == ["fixture-846"]


def test_coverage_without_kinds_has_no_by_kind(run: Any, tmp_path: Path) -> None:
    copy = _fixture_copy(tmp_path)
    document = json.loads((copy / "recipes.json").read_text())
    for entry in [*document["recipes"], *document["unmatched"]]:
        del entry["kind"]
    (copy / "recipes.json").write_text(json.dumps(document))
    _, _, _, out = _build(run, tmp_path, recipes=copy / "recipes.json")
    assert "byKind" not in json.loads((out / "coverage.json").read_text())


def test_contact_sheet_is_a_png(run: Any, tmp_path: Path) -> None:
    _, _, _, out = _build(run, tmp_path)
    sheet = out / "contact-sheet.png"
    with Image.open(sheet) as image:
        assert image.format == "PNG"
        assert image.size == (8 * 144, 164)
    assert sheet.stat().st_size > 2000


def test_build_record_names_the_recipes_and_the_encoder(run: Any, tmp_path: Path) -> None:
    _, _, _, out = _build(run, tmp_path)
    record = json.loads((out / "build-record.json").read_text())
    assert record["recipesSha256"] == hashlib.sha256((FIXTURE / "recipes.json").read_bytes()).hexdigest()
    assert record["encoder"]["pillow"] == "13.0.0"
    assert (record["codes"], record["objects"]) == (6, 5)


def _edit(tmp_path: Path, change: Any) -> Path:
    copy = _fixture_copy(tmp_path)
    document = json.loads((copy / "recipes.json").read_text())
    change(document)
    (copy / "recipes.json").write_text(json.dumps(document))
    return copy / "recipes.json"


def _set_game(game: str) -> Any:
    return lambda document: document.update(game=game)


def _duplicate_code(document: dict[str, Any]) -> None:
    document["unmatched"][0]["code"] = document["recipes"][0]["code"]


def _nine_layers(document: dict[str, Any]) -> None:
    layer = document["recipes"][0]["layers"][0]
    document["recipes"][0]["layers"] = [layer] * 9


def _other_composite(document: dict[str, Any]) -> None:
    document["recipes"][0]["composite"] = "multiply"


def _unknown_primary(document: dict[str, Any]) -> None:
    document["recipes"][1]["candidateOf"] = "nope"


def _candidate_of_candidate(document: dict[str, Any]) -> None:
    document["recipes"][1]["candidateOf"] = "fixture-circle"
    document["recipes"][2]["candidateOf"] = "fixture-circle-alias"


def _candidate_in_unmatched(document: dict[str, Any]) -> None:
    document["unmatched"][0]["candidateOf"] = "fixture-circle"


def _primary_in_unmatched(document: dict[str, Any]) -> None:
    document["recipes"][1]["candidateOf"] = "fixture-ghost"


def _with_ops(document: dict[str, Any]) -> None:
    document["recipes"][0]["layers"][0]["ops"] = [{"op": "tint"}]


def _escaping_image(document: dict[str, Any]) -> None:
    document["recipes"][0]["layers"][0]["image"] = "../x.png"


def _missing_image(document: dict[str, Any]) -> None:
    document["recipes"][0]["layers"][0]["image"] = "images/nope.png"


@pytest.mark.parametrize(
    ("change", "exit_code"),
    [
        (_set_game("_store"), 2),
        (_set_game("Bad_Game"), 2),
        (_duplicate_code, 2),
        (_nine_layers, 2),
        (_other_composite, 2),
        (_unknown_primary, 2),
        (_candidate_of_candidate, 2),
        (_candidate_in_unmatched, 2),
        (_primary_in_unmatched, 2),
        (_with_ops, 2),
        (_escaping_image, 2),
        (_missing_image, 4),
    ],
    ids=[
        "store",
        "bad-game",
        "duplicate-code",
        "nine-layers",
        "other-composite",
        "unknown-primary",
        "candidate-of-candidate",
        "candidate-in-unmatched",
        "unmatched-primary",
        "ops",
        "escaping-path",
        "missing-image",
    ],
)
def test_refusals(run: Any, tmp_path: Path, change: Any, exit_code: int) -> None:
    recipes = _edit(tmp_path, change)
    code, payload, _, _ = _build(run, tmp_path, recipes=recipes)
    assert code == exit_code
    assert payload["ok"] is False


def test_out_directory_must_be_empty(run: Any, tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    (out / "stale").write_text("x")
    code, payload, _ = run("icons", "build", "--recipes", str(FIXTURE / "recipes.json"), "--out", str(out))
    assert code == 2
    assert payload["ok"] is False
    assert (out / "stale").read_text() == "x"


def _margin_png(path: Path, *, opaque: bool = True) -> None:
    """A 40x30 image whose only opaque pixels are the 10x6 rectangle at (12, 9)."""
    image = Image.new("RGBA", (40, 30), (0, 0, 0, 0))
    if opaque:
        image.paste((200, 20, 20, 255), (12, 9, 22, 15))
    image.save(path, format="PNG")


def _trim_document(tmp_path: Path, *, opaque: bool = True) -> Path:
    copy = _fixture_copy(tmp_path)
    _margin_png(copy / "images" / "margin.png", opaque=opaque)
    document = json.loads((copy / "recipes.json").read_text())
    document["recipes"][0]["layers"][0] = {"image": "images/margin.png", "ops": [{"op": "trim"}]}
    (copy / "recipes.json").write_text(json.dumps(document))
    return copy / "recipes.json"


def test_trim_crops_to_the_alpha_bounding_box(run: Any, tmp_path: Path) -> None:
    code, _, _, out = _build(run, tmp_path, recipes=_trim_document(tmp_path))
    assert code == 0
    index = json.loads((out / "pack/fixture-846/index.json").read_text())
    with Image.open(out / "pack/fixture-846" / index["icons"]["fixture-circle"]) as image:
        assert image.size == (10, 6)
        assert image.convert("RGBA").getchannel("A").getextrema() == (255, 255)


def test_a_fully_transparent_image_cannot_be_trimmed(run: Any, tmp_path: Path) -> None:
    code, payload, _, out = _build(run, tmp_path, recipes=_trim_document(tmp_path, opaque=False))
    assert code == 6
    assert "fixture-circle" in payload["error"]
    assert not (out / "pack").exists() or not list((out / "pack").rglob("index.json"))


def test_rule_detail_and_codes_from_reach_the_coverage_report(run: Any, tmp_path: Path) -> None:
    def annotate(document: dict[str, Any]) -> None:
        document["codesFrom"] = "derived from test data"
        for recipe in document["recipes"]:
            recipe["rule"] = "icon"
        document["unmatched"][1]["rule"] = "icon"
        document["unmatched"][1]["detail"] = "Item_Broken"

    _, _, _, out = _build(run, tmp_path, recipes=_edit(tmp_path, annotate))
    coverage = json.loads((out / "coverage.json").read_text())
    assert coverage["codesFrom"] == "derived from test data"
    assert coverage["byRule"] == {"icon": {"total": 7, "matched": 6, "unmatched": 1}}
    assert coverage["measuredAgainst"] == "game-data" and "live code list" in coverage["note"]
    broken = {"code": "fixture-broken", "kind": "block", "reason": "missing-texture", "detail": "Item_Broken"}
    assert broken in coverage["unmatchedCodes"]
    assert all("detail" not in row for row in coverage["unmatchedCodes"] if row["code"] == "fixture-ghost")


@pytest.mark.parametrize(
    "entry",
    [{"rule": "Bad Rule"}, {"detail": "x" * 257}, {"detail": "line\nbreak"}],
    ids=["rule-pattern", "detail-length", "detail-control-character"],
)
def test_unmatched_entries_validate_their_rule_and_detail(run: Any, tmp_path: Path, entry: dict[str, Any]) -> None:
    recipes = _edit(tmp_path, lambda document: document["unmatched"][0].update(entry))
    assert _build(run, tmp_path, recipes=recipes)[0] == 2


def _sheet_document(tmp_path: Path) -> Path:
    """200 icons: 150 of rule "icon", 40 of "bulk" and 10 of "rare"."""
    copy = _fixture_copy(tmp_path)
    recipes = []
    for number in range(200):
        rule = "rare" if number < 10 else "bulk" if number < 50 else "icon"
        recipes.append(
            {
                "code": f"item-{number:03}",
                "kind": "item",
                "rule": rule,
                "layers": [{"image": "images/pebble-32.png", "ops": []}],
            }
        )
    (copy / "recipes.json").write_text(json.dumps({"schemaVersion": 1, "game": "fixture-846", "recipes": recipes}))
    return copy / "recipes.json"


def test_the_contact_sheet_samples_every_rule(run: Any, tmp_path: Path) -> None:
    from takaro_maint.icons import build

    by_rule = {
        "rare": [f"item-{n:03}" for n in range(10)],
        "bulk": [f"item-{n:03}" for n in range(10, 50)],
        "icon": [f"item-{n:03}" for n in range(50, 200)],
    }
    sample = build._stratified_sample(by_rule)
    assert len(sample) == 64 and len(set(sample)) == 64
    counts = {rule: len([code for code in sample if code in codes]) for rule, codes in by_rule.items()}
    assert min(counts.values()) >= 8 and sum(counts.values()) == 64
    # grouped by rule (alphabetical), each group in code order
    assert sample == sorted(
        sample, key=lambda code: next((rule, code) for rule, codes in by_rule.items() if code in codes)
    )
    _, _, _, out = _build(run, tmp_path, recipes=_sheet_document(tmp_path))
    with Image.open(out / "contact-sheet.png") as sheet:
        assert sheet.size == (8 * 144, 8 * 164)


def test_a_small_ruled_build_keeps_every_icon_on_the_sheet() -> None:
    from takaro_maint.icons import build

    assert build._stratified_sample({"moveable": ["b", "a"], "icon": ["c"]}) == ["c", "a", "b"]


def _layer_images(copy: Path) -> None:
    base = Image.new("RGBA", (40, 40), (0, 0, 0, 0))
    base.paste((10, 200, 10, 255), (4, 4, 36, 36))
    base.save(copy / "images" / "base-40.png", format="PNG")
    ring = Image.new("RGBA", (80, 80), (0, 0, 0, 0))
    for box in ((0, 0, 80, 6), (0, 74, 80, 80), (0, 0, 6, 80), (74, 0, 80, 80)):
        ring.paste((250, 210, 0, 255), box)
    ring.save(copy / "images" / "ring-80.png", format="PNG")


def _layered_document(tmp_path: Path, change: Any = None) -> Path:
    copy = _fixture_copy(tmp_path)
    _layer_images(copy)
    document = json.loads((copy / "recipes.json").read_text())
    document["recipes"][0]["layers"] = [{"image": "images/base-40.png"}, {"image": "images/ring-80.png"}]
    document["recipes"][0]["rule"] = "icon-layers"
    if change:
        change(document)
    (copy / "recipes.json").write_text(json.dumps(document))
    return copy / "recipes.json"


def _icon(out: Path, code_name: str) -> Image.Image:
    index = json.loads((out / "pack/fixture-846/index.json").read_text())
    return Image.open(out / "pack/fixture-846" / index["icons"][code_name]).convert("RGBA")


def test_layers_of_different_sizes_composite_onto_the_first_layers_canvas(run: Any, tmp_path: Path) -> None:
    code, _, _, out = _build(run, tmp_path, recipes=_layered_document(tmp_path))
    assert code == 0
    image = _icon(out, "fixture-circle")
    assert image.size == (40, 40)  # layer 0 sets the canvas; the 80x80 ring is resized onto it
    assert image.getpixel((1, 1))[:3] == (250, 210, 0) and image.getpixel((1, 1))[3] == 255  # ring on top
    assert image.getpixel((20, 20)) == (10, 200, 10, 255)  # base shows through the ring's hole


def test_flags_reach_the_coverage_report(run: Any, tmp_path: Path) -> None:
    def flagged(document: dict[str, Any]) -> None:
        document["recipes"][0]["flags"] = [{"flag": "mask-texture", "detail": "pkg PF_BC4"}]
        document["recipes"][2]["flags"] = [{"flag": "mask-texture"}, {"flag": "odd-size"}]

    code, _, _, out = _build(run, tmp_path, recipes=_layered_document(tmp_path, flagged))
    assert code == 0
    coverage = json.loads((out / "coverage.json").read_text())
    assert coverage["flagsByName"] == {"mask-texture": 2, "odd-size": 1}
    assert coverage["flaggedCodes"] == [
        {"code": "fixture-banner", "flag": "mask-texture"},
        {"code": "fixture-banner", "flag": "odd-size"},
        {"code": "fixture-circle", "flag": "mask-texture", "detail": "pkg PF_BC4"},
    ]


def test_a_candidate_shares_its_primarys_object_and_is_left_out_of_every_count(run: Any, tmp_path: Path) -> None:
    def candidate(document: dict[str, Any]) -> None:
        document["recipes"].append(
            {
                "code": "fixture-banner-alt",
                "kind": "item",
                "candidateOf": "fixture-banner",
                "layers": document["recipes"][2]["layers"],
            }
        )

    plain_code, _, _, plain = _build(run, tmp_path, "plain")
    code, _, _, out = _build(run, tmp_path, recipes=_layered_document(tmp_path, candidate))
    assert plain_code == 0 and code == 0
    icons = json.loads((out / "pack/fixture-846/index.json").read_text())["icons"]
    assert icons["fixture-banner-alt"] == icons["fixture-banner"]
    coverage = json.loads((out / "coverage.json").read_text())
    baseline = json.loads((plain / "coverage.json").read_text())
    assert (coverage["total"], coverage["matched"]) == (baseline["total"], baseline["matched"])
    assert coverage["byKind"] == baseline["byKind"]
    assert coverage["candidates"] == {
        "count": 1,
        "codes": [{"code": "fixture-banner-alt", "candidateOf": "fixture-banner"}],
    }
    assert "candidates" not in baseline


def _tint_document(tmp_path: Path, operation: dict[str, Any]) -> Path:
    """The first recipe's layer is a 16x16 grey (128) square, opaque on the left half and half-alpha on the right."""
    copy = _fixture_copy(tmp_path)
    image = Image.new("RGBA", (16, 16), (128, 128, 128, 255))
    image.paste((128, 128, 128, 128), (8, 0, 16, 16))
    image.save(copy / "images" / "grey.png", format="PNG")
    document = json.loads((copy / "recipes.json").read_text())
    document["recipes"][0]["layers"][0] = {"image": "images/grey.png", "ops": [operation]}
    (copy / "recipes.json").write_text(json.dumps(document))
    return copy / "recipes.json"


def test_tint_multiply_multiplies_straight_rgb_and_keeps_alpha(run: Any, tmp_path: Path) -> None:
    recipes = _tint_document(tmp_path, {"op": "tint-multiply", "color": "#80ff40"})
    code, _, _, out = _build(run, tmp_path, recipes=recipes)
    assert code == 0
    index = json.loads((out / "pack/fixture-846/index.json").read_text())
    with Image.open(out / "pack/fixture-846" / index["icons"]["fixture-circle"]) as image:
        rgba = image.convert("RGBA")
    # c * t // 255 per channel: 64, 128, 32
    assert rgba.getpixel((4, 8)) == (64, 128, 32, 255)
    assert rgba.getpixel((12, 8)) == (64, 128, 32, 128)


@pytest.mark.parametrize(
    "operation",
    [
        {"op": "tint-multiply"},
        {"op": "tint-multiply", "color": "#80FF40"},
        {"op": "tint-multiply", "color": "#fff"},
        {"op": "tint-multiply", "color": "red"},
        {"op": "tint-multiply", "color": "80ff40"},
        {"op": "tint-multiply", "color": "#80ff40", "alpha": 1},
        {"op": "trim", "color": "#80ff40"},
        {"op": "tint"},
    ],
    ids=["no-color", "uppercase", "short", "name", "no-hash", "extra-key", "trim-with-color", "unknown-op"],
)
def test_a_malformed_op_is_refused(run: Any, tmp_path: Path, operation: dict[str, Any]) -> None:
    code, payload, _, out = _build(run, tmp_path, recipes=_tint_document(tmp_path, operation))
    assert code == 2 and "invalid" in payload["error"]
    assert not out.exists() or not list(out.rglob("index.json"))


def test_tint_multiply_floors_each_channel(run: Any, tmp_path: Path) -> None:
    recipes = _tint_document(tmp_path, {"op": "tint-multiply", "color": "#c8fe80"})
    Image.new("RGBA", (16, 16), (200, 129, 1, 255)).save(recipes.parent / "images" / "grey.png", format="PNG")
    code, _, _, out = _build(run, tmp_path, recipes=recipes)
    assert code == 0
    index = json.loads((out / "pack/fixture-846/index.json").read_text())
    with Image.open(out / "pack/fixture-846" / index["icons"]["fixture-circle"]) as image:
        assert image.convert("RGBA").getpixel((4, 4)) == (156, 128, 0, 255)  # rounding would give 157, 128, 1


# ------------------------------------------------------------------------------------------- atlas-cut


def _atlas_pixel(x: int, y: int) -> tuple[int, int, int, int]:
    return (x * 30, y * 40, 100, 255)


def _atlas_document(tmp_path: Path, operation: dict[str, Any]) -> Path:
    """The first recipe's layer is an 8x6 atlas (top row first) whose pixel (x, y) is ``_atlas_pixel(x, y)``."""
    copy = _fixture_copy(tmp_path)
    atlas = Image.new("RGBA", (8, 6))
    for y in range(6):
        for x in range(8):
            atlas.putpixel((x, y), _atlas_pixel(x, y))
    atlas.save(copy / "images" / "atlas.png", format="PNG")
    document = json.loads((copy / "recipes.json").read_text())
    document["recipes"][0]["layers"][0] = {"image": "images/atlas.png", "ops": [operation]}
    (copy / "recipes.json").write_text(json.dumps(document))
    return copy / "recipes.json"


def _atlas_cut_result(run: Any, tmp_path: Path, **parameters: Any) -> Image.Image:
    recipes = _atlas_document(tmp_path, {"op": "atlas-cut", **parameters})
    code, payload, _, out = _build(run, tmp_path, recipes=recipes)
    assert code == 0, payload
    index = json.loads((out / "pack/fixture-846/index.json").read_text())
    with Image.open(out / "pack/fixture-846" / index["icons"]["fixture-circle"]) as image:
        return image.convert("RGBA")


def _pixels(image: Image.Image) -> list[list[Any]]:
    return [[image.getpixel((x, y)) for x in range(image.width)] for y in range(image.height)]


def test_atlas_cut_reads_the_rect_from_the_bottom_left(run: Any, tmp_path: Path) -> None:
    image = _atlas_cut_result(run, tmp_path, rect=[2, 1, 3, 2], rotation="none")
    # y 1..3 from the bottom of a 6-row atlas is rows 3 and 4 from the top.
    assert _pixels(image) == [[_atlas_pixel(x, y) for x in (2, 3, 4)] for y in (3, 4)]


def test_atlas_cut_rounds_half_to_even_in_both_directions(run: Any, tmp_path: Path) -> None:
    image = _atlas_cut_result(run, tmp_path, rect=[1.5, 0.5, 2, 2], rotation="none")
    # l=round(1.5)=2, r=round(3.5)=4, b=round(0.5)=0, t=round(2.5)=2: columns 2..3, rows 4..5 from the top.
    assert _pixels(image) == [[_atlas_pixel(x, y) for x in (2, 3)] for y in (4, 5)]
    image = _atlas_cut_result(run, tmp_path / "second", rect=[1.4, 0.6, 2, 2], rotation="none")
    # l=1, r=round(3.4)=3, b=1, t=round(2.6)=3: columns 1..2, rows 3..4.
    assert _pixels(image) == [[_atlas_pixel(x, y) for x in (1, 2)] for y in (3, 4)]


@pytest.mark.parametrize(
    ("rotation", "source"),
    [
        ("none", lambda w, h, x, y: (x, y)),
        ("flip-horizontal", lambda w, h, x, y: (w - 1 - x, y)),
        ("flip-vertical", lambda w, h, x, y: (x, h - 1 - y)),
        ("rotate-180", lambda w, h, x, y: (w - 1 - x, h - 1 - y)),
        ("rotate-90", lambda w, h, x, y: (w - 1 - y, x)),
    ],
    ids=["none", "flip-horizontal", "flip-vertical", "rotate-180", "rotate-90"],
)
def test_atlas_cut_undoes_each_packing_rotation(run: Any, tmp_path: Path, rotation: str, source: Any) -> None:
    # The cut is 3x2 at atlas columns 1..3 and rows 3..4 from the top.
    image = _atlas_cut_result(run, tmp_path, rect=[1, 1, 3, 2], rotation=rotation)
    width, height = (2, 3) if rotation == "rotate-90" else (3, 2)
    assert image.size == (width, height)
    for y in range(height):
        for x in range(width):
            cut_x, cut_y = source(3, 2, x, y)
            assert image.getpixel((x, y)) == _atlas_pixel(1 + cut_x, 3 + cut_y)


def test_atlas_cut_places_a_trimmed_sprite_in_its_frame(run: Any, tmp_path: Path) -> None:
    image = _atlas_cut_result(run, tmp_path, rect=[0, 0, 2, 2], rotation="none", canvas=[4, 5], offset=[1, 1])
    assert image.size == (4, 5)
    # paste_left=1, paste_top=5-1-2=2; the cut is columns 0..1 of atlas rows 4..5.
    for y in range(5):
        for x in range(4):
            inside = 1 <= x < 3 and 2 <= y < 4
            expected = _atlas_pixel(x - 1, 4 + y - 2) if inside else (0, 0, 0, 0)
            assert image.getpixel((x, y)) == expected


@pytest.mark.parametrize(
    "operation",
    [
        {"op": "atlas-cut", "rotation": "none"},
        {"op": "atlas-cut", "rect": [0, 0, 2, 2]},
        {"op": "atlas-cut", "rect": [0, 0, 2], "rotation": "none"},
        {"op": "atlas-cut", "rect": [0, 0, 2, 2, 1], "rotation": "none"},
        {"op": "atlas-cut", "rect": [-1, 0, 2, 2], "rotation": "none"},
        {"op": "atlas-cut", "rect": [0, 0, 0, 2], "rotation": "none"},
        {"op": "atlas-cut", "rect": [0, 0, 2, 2], "rotation": "rotate-270"},
        {"op": "atlas-cut", "rect": [0, 0, 2, 2], "rotation": "none", "canvas": [4, 4]},
        {"op": "atlas-cut", "rect": [0, 0, 2, 2], "rotation": "none", "offset": [1, 1]},
        {"op": "atlas-cut", "rect": [0, 0, 2, 2], "rotation": "none", "extra": 1},
    ],
    ids=[
        "no-rect",
        "no-rotation",
        "short-rect",
        "long-rect",
        "negative",
        "zero-width",
        "unknown-rotation",
        "canvas-only",
        "offset-only",
        "extra-key",
    ],
)
def test_a_malformed_atlas_cut_is_refused(run: Any, tmp_path: Path, operation: dict[str, Any]) -> None:
    code, payload, _, out = _build(run, tmp_path, recipes=_atlas_document(tmp_path, operation))
    assert code == 2 and "invalid" in payload["error"]
    assert not out.exists() or not list(out.rglob("index.json"))


@pytest.mark.parametrize(
    "parameters",
    [
        {"rect": [6, 0, 4, 2], "rotation": "none"},
        {"rect": [0, 5, 2, 2], "rotation": "none"},
        {"rect": [0, 0, 2, 2], "rotation": "none", "canvas": [3, 3], "offset": [2, 2]},
    ],
    ids=["past-the-right-edge", "past-the-top-edge", "does-not-fit-the-canvas"],
)
def test_an_atlas_cut_that_does_not_fit_fails_the_build_naming_the_recipe(
    run: Any, tmp_path: Path, parameters: dict[str, Any]
) -> None:
    recipes = _atlas_document(tmp_path, {"op": "atlas-cut", **parameters})
    code, payload, _, out = _build(run, tmp_path, recipes=recipes)
    assert code == 6 and payload["recipe"] == "fixture-circle"
    assert not out.exists() or not list(out.rglob("index.json"))
