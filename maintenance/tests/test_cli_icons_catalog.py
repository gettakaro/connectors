"""``icons inputs`` and the catalog-driven ``icons build --game zomboid``, end to end.

The server depot is the DepotDownloader stand-in serving ``fixtures/icons/zomboid/depots``; the client is a
directory of synthetic texture packs (``fixtures/icons/zomboid/client``). Every assertion is on exit codes,
the JSON on stdout and the files the command wrote.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import struct
import sys
from pathlib import Path
from typing import Any

import pytest
from PIL import Image

import fake_depotdownloader as fake
from conftest import FIXTURES, REPO_ROOT
from takaro_maint import paths

FIXTURE = FIXTURES / "icons" / "zomboid"
MANIFEST = "7440000000000000001"
TARGET = "linux-42.21.0"
PACKS = "media/texturepacks"
CODES = {
    "Base.Axe",
    "Base.Bag_LeatherWaterBag",
    "Base.Bandage",
    "Base.Glasses_Normal",
    "Base.Hammer_Fixture",
    "Base.Hematite",
    "Base.MissingTexture",
    "Base.Mov_MissingSprite",
    "Base.Mov_NoSprite",
    "Base.Mov_SodaMachine",
    "Base.Wound_Chest_Bite_Male",
}
MATCHED = CODES - {
    "Base.MissingTexture",
    "Base.Mov_MissingSprite",
    "Base.Mov_NoSprite",
    "Base.Wound_Chest_Bite_Male",
}
RED, YELLOW, BLUE, CYAN = (220, 30, 30, 255), (240, 220, 20, 255), (30, 60, 230, 255), (20, 200, 200, 255)
MAGENTA, ORANGE, GREY, PURPLE = (255, 0, 255, 255), (255, 128, 0, 255), (120, 120, 120, 255), (128, 0, 255, 255)


def _load_maker() -> Any:
    spec = importlib.util.spec_from_file_location("zomboid_make_fixture", FIXTURE / "make_fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


maker = _load_maker()


def make_repo(tmp_path: Path) -> Path:
    """The real catalog and tool lock, with the Zomboid server input pinned at the fixture depot."""
    root = tmp_path / "repo"
    shutil.copytree(REPO_ROOT / "catalog", root / "catalog")
    (root / "maintenance").mkdir()
    shutil.copy2(REPO_ROOT / "maintenance" / "tools.lock.json", root / "maintenance" / "tools.lock.json")
    path = root / "catalog" / "zomboid" / "targets" / f"{TARGET}.json"
    record = json.loads(path.read_text())
    record["inputs"]["server"]["depots"]["380871"]["manifest"] = MANIFEST
    path.write_text(json.dumps(record, indent=2) + "\n")
    return root


def edit_game(root: Path, change: Any) -> None:
    path = root / "catalog" / "zomboid" / "game.json"
    record = json.loads(path.read_text())
    change(record["icons"])
    path.write_text(json.dumps(record, indent=2) + "\n")


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    return make_repo(tmp_path)


@pytest.fixture
def client(tmp_path: Path) -> Path:
    destination = tmp_path / "client"
    shutil.copytree(FIXTURE / "client", destination)
    return destination


@pytest.fixture
def dd_log(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    log = tmp_path / "depotdownloader-argv.jsonl"
    monkeypatch.setenv("TAKARO_MAINT_DEPOTDOWNLOADER", str(Path(fake.__file__).resolve()))
    monkeypatch.setenv("FAKE_DD_ROOT", str(FIXTURE / "depots"))
    monkeypatch.setenv("FAKE_DD_LOG", str(log))
    return log


def build(run: Any, repo: Path, client: Path | None, out: Path, *extra: str) -> tuple[int, Any, str]:
    argv = ["icons", "build", "--game", "zomboid", "--out", str(out), *extra]
    if client is not None:
        argv += ["--client", str(client)]
    return run(*argv, repo=repo)


def downloads(log: Path) -> list[list[str]]:
    return [argv for argv in fake.argv_log(log) if "-filelist" in argv]


def tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def pixel(path: Path, point: tuple[int, int]) -> tuple[int, ...]:
    with Image.open(path) as image:
        return image.convert("RGBA").getpixel(point)  # type: ignore[return-value]


def icon_file(out: Path, code: str) -> Path:
    index = json.loads((out / "pack" / "zomboid" / "index.json").read_text())
    return out / "pack" / "zomboid" / index["icons"][code]


# ------------------------------------------------------------------ catalog and inputs


def test_the_real_catalog_validates(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")
    assert code == 0, payload
    ids = {check["id"] for check in payload["checks"]}
    assert {"icons-game-identifier", "icons-mapper-known", "icons-data-input", "icons-client-builds-revisions"} <= ids


@pytest.mark.parametrize(
    ("change", "check"),
    [
        (lambda block: block.update(extra=1), "game-schema"),
        (lambda block: block.update(game="_store"), "icons-game-identifier"),
        (lambda block: block.update(game="Bad_Game"), "icons-game-identifier"),
        (lambda block: block.update(mapper="nope"), "icons-mapper-known"),
        (lambda block: block.update(secrets=["lower"]), "game-schema"),
        (lambda block: block["pixels"].update(decoder="unknown"), "game-schema"),
        (lambda block: block["data"].update(depots=["999"]), "icons-data-input"),
        (lambda block: block["data"].update(input="client"), "icons-data-input"),
        (lambda block: block["pixels"].update(clientBuilds={"1.2.3": [5]}), "icons-client-builds-revisions"),
        (lambda block: block["pixels"].update(files=["media/texturepacks/*.pack"]), "game-schema"),
    ],
    ids=[
        "unknown-key",
        "store",
        "bad-game",
        "mapper",
        "secret-name",
        "decoder",
        "depot",
        "input",
        "client-builds",
        "glob",
    ],
)
def test_a_bad_icons_block_fails_validation(run: Any, catalog_copy: Path, change: Any, check: str) -> None:
    edit_game(catalog_copy, change)
    code, payload, _ = run("catalog", "validate", repo=catalog_copy)
    assert code == 2
    assert check in {entry["id"] for entry in payload["checks"] if entry["status"] == "fail"}


def test_inputs_lists_both_roles_without_any_network(run: Any, dd_log: Path) -> None:
    code, payload, _ = run("icons", "inputs", "--game", "zomboid")
    assert code == 0, payload
    assert (payload["game"], payload["packGame"], payload["target"], payload["mapper"]) == (
        "zomboid",
        "zomboid",
        TARGET,
        "zomboid",
    )
    assert len(payload["fingerprint"]) == 64
    data, pixels = payload["inputs"]
    assert data["role"] == "data" and data["app"] == 380870 and data["buildid"] == 25485538
    assert data["files"] == ["regex:^media/scripts/.*\\.txt$"]
    assert list(data["depots"]) == ["380871"] and data["cached"] is False
    assert pixels["role"] == "pixels" and (pixels["app"], pixels["depot"]) == (108600, 108604)
    assert pixels["files"] == [
        f"{PACKS}/UI2.pack",
        f"{PACKS}/UI.pack",
        f"{PACKS}/IconsMoveables.pack",
        f"{PACKS}/Tiles2x.pack",
        f"{PACKS}/Tiles2x.floor.pack",
    ]
    assert pixels["expectedClientBuilds"] == [25485521] and pixels["decoder"] == "pz-texturepack"
    assert pixels["suggestedMirror"] == str(paths.cache_dir() / "icons" / "clients" / "zomboid" / "108600")
    assert payload["env"] == []
    assert fake.argv_log(dd_log) == []


def test_inputs_refusals(run: Any) -> None:
    code, _, stderr = run("icons", "inputs", "--game", "rust")
    assert code == 2 and "game 'rust' has no icons block" in stderr
    assert run("icons", "inputs", "--game", "zomboid", "--target", "nope")[0] == 3


def test_inputs_reports_whether_a_secret_is_set_and_never_its_value(
    run: Any, repo: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    edit_game(repo, lambda block: block.update(secrets=["FAKE_ICON_KEY"]))
    monkeypatch.delenv("FAKE_ICON_KEY", raising=False)
    assert run("icons", "inputs", "--game", "zomboid", repo=repo)[1]["env"] == [{"name": "FAKE_ICON_KEY", "set": False}]
    monkeypatch.setenv("FAKE_ICON_KEY", "s3cret-value-9")
    code, payload, stderr = run("icons", "inputs", "--game", "zomboid", repo=repo)
    assert code == 0
    assert payload["env"] == [{"name": "FAKE_ICON_KEY", "set": True}]
    assert "s3cret-value-9" not in json.dumps(payload) + stderr


# ------------------------------------------------------------------ the build


def test_the_build_maps_game_data_to_a_pack(run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    code, payload, _ = build(run, repo, client, out)
    assert code == 0, payload
    assert payload["game"] == "zomboid" and payload["catalogGame"] == "zomboid" and payload["target"] == TARGET
    assert payload["coverage"] == {"total": 11, "matched": 7, "unmatched": 4, "coverage": 0.6364}
    assert payload["clientBuild"] == {"id": 25485521, "status": "match", "expected": [25485521]}
    assert payload["inputs"]["data"]["files"] == 2 and payload["inputs"]["pixels"] == {"files": 5}
    assert payload["warnings"] == []

    index = json.loads((out / "pack" / "zomboid" / "index.json").read_text())
    assert index["game"] == "zomboid" and set(index["icons"]) == MATCHED

    coverage = json.loads((out / "coverage.json").read_text())
    assert (coverage["total"], coverage["matched"], coverage["unmatched"]) == (11, 7, 4)
    listed = {row["code"] for row in coverage["unmatchedCodes"]}
    assert listed == CODES - MATCHED
    for absent in ("Base.Commented", "Base.ObsoleteThing", "Base.DisabledThing", "Base.DebugThing", "Base.NotATxtFile"):
        assert absent not in index["icons"] and absent not in listed
    assert not {"Base.OutsideScripts", "Base.Plank", "Base.WoodStake"} & (set(index["icons"]) | listed)
    assert coverage["unmatchedByReason"] == {
        "missing-sprite": 1,
        "missing-texture": 1,
        "no-icon-in-game": 1,
        "no-world-sprite": 1,
    }
    assert coverage["byRule"] == {
        "icon": {"total": 5, "matched": 4, "unmatched": 1},
        "icons-for-texture": {"total": 1, "matched": 1, "unmatched": 0},
        "moveable": {"total": 4, "matched": 2, "unmatched": 2},
        "none": {"total": 1, "matched": 0, "unmatched": 1},
    }
    details = {row["code"]: row.get("detail") for row in coverage["unmatchedCodes"]}
    assert details == {
        "Base.MissingTexture": "Item_NoSuchTexture",
        "Base.Mov_MissingSprite": "nowhere_01_0",
        "Base.Mov_NoSprite": None,
        "Base.Wound_Chest_Bite_Male": None,
    }
    assert coverage["measuredAgainst"] == "game-data"
    assert "media/scripts" in coverage["codesFrom"] and "#811" in coverage["codesFrom"]
    assert "build 25485538" in coverage["codesFrom"] and "obsolete" in coverage["codesFrom"]

    assert (out / "recipes.json").is_file() and (out / "contact-sheet.png").is_file()
    assert list((paths.cache_dir() / "icons" / "work").iterdir()) == []
    assert sorted(path.name for path in out.rglob("*.png")) == ["contact-sheet.png"]


def test_icon_art_comes_from_the_right_pack_entry(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    out = tmp_path / "out"
    assert build(run, repo, client, out)[0] == 0
    # UI2 is listed first, so it beats UI for the name both hold, and a later definition of Base.Axe changes nothing.
    assert pixel(icon_file(out, "Base.Axe"), (16, 16)) == RED
    assert pixel(icon_file(out, "Base.Bandage"), (16, 16)) == MAGENTA  # UI only, and its first entry
    assert pixel(icon_file(out, "Base.Hammer_Fixture"), (16, 16)) == GREY  # lowercase "icon" key
    assert pixel(icon_file(out, "Base.Bag_LeatherWaterBag"), (16, 16)) == CYAN  # a nested block does not hide Icon
    assert pixel(icon_file(out, "Base.Glasses_Normal"), (16, 16)) == BLUE  # the first IconsForTexture entry
    for code in ("Base.Axe", "Base.Bandage", "Base.Glasses_Normal"):
        with Image.open(icon_file(out, code)) as image:
            assert image.size == (32, 32)  # the frame is kept, not the packed crop
        assert pixel(icon_file(out, code), (0, 0))[3] == 0
        assert pixel(icon_file(out, code), (6, 4))[3] == 255  # the crop sits at its offset
        assert pixel(icon_file(out, code), (5, 4))[3] == 0


def test_moveables_are_trimmed_to_the_sprite(run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, repo, client, out)[0] == 0
    for code, size, colour in (
        ("Base.Hematite", (24, 30), ORANGE),
        ("Base.Mov_SodaMachine", (24, 32), PURPLE),
    ):
        with Image.open(icon_file(out, code)) as image:
            assert image.size == size
            assert image.convert("RGBA").getchannel("A").getextrema() == (255, 255)  # no transparent border left
        assert pixel(icon_file(out, code), (size[0] // 2, size[1] // 2)) == colour


def test_the_build_record_names_what_was_built_from(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    out = tmp_path / "out"
    code, payload, _ = build(run, repo, client, out)
    assert code == 0
    record = json.loads((out / "build-record.json").read_text())
    assert record["source"] == "catalog" and record["game"] == "zomboid" and record["catalogGame"] == "zomboid"
    assert record["target"] == TARGET and record["revision"] == "42.21.0"
    assert record["fingerprint"] == payload["fingerprint"] and len(record["fingerprint"]) == 64
    assert record["recipesSha256"] == hashlib.sha256((out / "recipes.json").read_bytes()).hexdigest()
    assert (record["codes"], record["objects"]) == (7, 7)

    scripts = FIXTURE / "depots" / "380871" / MANIFEST / "tree" / "media" / "scripts"
    rows = sorted(
        (path.relative_to(FIXTURE / "depots" / "380871" / MANIFEST / "tree").as_posix(), path)
        for path in [scripts / "items_fixture.txt", scripts / "generated" / "items" / "fixture_nested.txt"]
    )
    listing = "".join(f"{name}\t{hashlib.sha256(path.read_bytes()).hexdigest()}\n" for name, path in rows)
    assert record["data"] == {
        "source": "server-depot",
        "app": 380870,
        "branch": "public",
        "buildid": 25485538,
        "os": "linux",
        "depots": {"380871": MANIFEST},
        "selectors": ["regex:^media/scripts/.*\\.txt$"],
        "files": 2,
        "treeSha256": hashlib.sha256(listing.encode()).hexdigest(),
    }
    assert record["client"] == {
        "app": 108600,
        "depot": 108604,
        "buildid": 25485521,
        "manifest": "7544133343391543313",
        "status": "match",
        "expected": [25485521],
    }
    assert record["pixels"]["source"] == "client" and record["pixels"]["decoder"] == "pz-texturepack"
    packs = record["pixels"]["files"]
    assert list(packs) == [
        f"{PACKS}/{name}.pack" for name in ("UI2", "UI", "IconsMoveables", "Tiles2x", "Tiles2x.floor")
    ]
    for name, entry in packs.items():
        raw = (client / name).read_bytes()
        assert (entry["sha256"], entry["size"]) == (hashlib.sha256(raw).hexdigest(), len(raw))
    assert [(entry["layout"], entry["pages"], entry["entries"]) for entry in packs.values()] == [
        ("v2", 2, 4),
        ("v1", 2, 4),
        ("v1", 1, 1),
        ("v2", 1, 2),
        ("v2", 1, 1),
    ]
    assert [entry["codesSupplied"] for entry in packs.values()] == [3, 2, 0, 2, 0]
    assert [entry["shadowedNames"] for entry in packs.values()] == [0, 2, 0, 0, 0]
    assert record["mapper"]["name"] == "zomboid"
    notes = " ".join(record["mapper"]["notes"])
    assert "1 repeated full names" in notes and "1 obsolete" in notes and "1 disabled" in notes
    assert "1 debug-only" in notes
    tools = record["tools"]
    assert set(tools) == {"takaroMaint", "python", "depotdownloader", "pillow", "libwebp", "decoder"}
    assert tools["pillow"] == "13.0.0" and tools["decoder"].startswith("pz-texturepack/1")
    assert record["warnings"] == []


def test_two_builds_are_byte_identical(run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path) -> None:
    assert build(run, repo, client, tmp_path / "one")[0] == 0
    assert build(run, repo, client, tmp_path / "two")[0] == 0
    assert tree(tmp_path / "one") == tree(tmp_path / "two")


def test_the_filtered_download_has_a_cache_of_its_own(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    assert build(run, repo, client, tmp_path / "one")[0] == 0
    assert build(run, repo, client, tmp_path / "two")[0] == 0
    assert len(downloads(dd_log)) == 1
    cache = paths.cache_dir()
    [filtered] = (cache / "icons" / "steam" / "380870" / "380871" / MANIFEST).iterdir()
    assert len(filtered.name) == 16
    assert sorted(path.name for path in (filtered / "media" / "scripts").rglob("*.txt")) == [
        "fixture_nested.txt",
        "items_fixture.txt",
    ]
    assert not (filtered / ".DepotDownloader").exists()
    assert not (cache / "steam" / "380870").exists()

    (filtered / "media" / "scripts" / "items_fixture.txt").write_text("module Base\n{\n}\n")
    code, _, stderr = build(run, repo, client, tmp_path / "three")
    assert code == 0 and "corrupt filtered cache" in stderr
    assert len(downloads(dd_log)) == 2
    assert tree(tmp_path / "three") == tree(tmp_path / "one")


# ------------------------------------------------------------------ the client build


@pytest.mark.parametrize(("buildid", "status"), [("25485600", "newer"), ("25485000", "older")])
def test_a_client_of_another_build_warns_and_still_builds(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path, buildid: str, status: str
) -> None:
    manifest = client / "appmanifest_108600.acf"
    manifest.write_text(manifest.read_text().replace("25485521", buildid))
    code, payload, stderr = build(run, repo, client, tmp_path / "out")
    assert code == 0
    assert payload["clientBuild"] == {"id": int(buildid), "status": status, "expected": [25485521]}
    assert len(payload["warnings"]) == 1 and status in payload["warnings"][0]
    assert "warning:" in stderr
    record = json.loads((tmp_path / "out" / "build-record.json").read_text())
    assert record["client"]["status"] == status and record["warnings"] == payload["warnings"]


def test_a_client_without_an_appmanifest_is_unknown(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    (client / "appmanifest_108600.acf").unlink()
    code, payload, _ = build(run, repo, client, tmp_path / "out")
    assert code == 0
    assert payload["clientBuild"] == {"id": None, "status": "unknown", "expected": [25485521]}
    assert len(payload["warnings"]) == 1
    record = json.loads((tmp_path / "out" / "build-record.json").read_text())
    assert (record["client"]["buildid"], record["client"]["manifest"]) == (None, None)


def test_an_appmanifest_two_levels_up_is_found_in_a_steam_library(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    library = tmp_path / "steamapps"
    installed = library / "common" / "ProjectZomboid"
    shutil.copytree(client / "media", installed / "media")
    shutil.move(client / "appmanifest_108600.acf", library / "appmanifest_108600.acf")
    code, payload, _ = build(run, repo, installed, tmp_path / "out")
    assert code == 0
    assert payload["clientBuild"]["status"] == "match" and payload["warnings"] == []


def test_a_client_build_of_another_release_is_named_in_the_warning(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    edit_game(repo, lambda block: block["pixels"].update(clientBuilds={"42.21.0": [25485521], "42.20.4": [24909000]}))
    manifest = client / "appmanifest_108600.acf"
    manifest.write_text(manifest.read_text().replace("25485521", "24909000"))
    code, payload, _ = build(run, repo, client, tmp_path / "out")
    assert code == 0
    assert payload["clientBuild"]["status"] == "older" and "42.20.4" in payload["warnings"][0]


# ------------------------------------------------------------------ refusals, before any download


def test_a_missing_pack_is_refused_before_any_download(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    (client / PACKS / "UI.pack").unlink()
    code, payload, _ = build(run, repo, client, tmp_path / "out")
    assert code == 4
    assert payload["missing"] == [f"{PACKS}/UI.pack"]
    assert fake.argv_log(dd_log) == []


def test_the_client_is_required_and_must_be_a_directory(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    code, payload, _ = build(run, repo, None, tmp_path / "out")
    assert code == 2 and f"{PACKS}/UI2.pack" in payload["error"]
    assert build(run, repo, tmp_path / "nowhere", tmp_path / "out")[0] == 4
    assert fake.argv_log(dd_log) == []


def test_the_build_modes_exclude_each_other(run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path) -> None:
    recipes = str(FIXTURES / "icons" / "fixture-846" / "recipes.json")
    out = str(tmp_path / "out")
    assert run("icons", "build", "--out", out, repo=repo)[0] == 2
    assert run("icons", "build", "--recipes", recipes, "--game", "zomboid", "--out", out, repo=repo)[0] == 2
    assert run("icons", "build", "--recipes", recipes, "--client", str(client), "--out", out, repo=repo)[0] == 2
    assert not (tmp_path / "out").exists()
    assert build(run, repo, client, tmp_path / "x", "--target", "nope")[0] == 3
    assert fake.argv_log(dd_log) == []


def test_an_output_directory_that_is_not_empty_is_refused_first(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    out = tmp_path / "out"
    out.mkdir()
    (out / "keep").write_text("x")
    assert build(run, repo, client, out)[0] == 2
    assert fake.argv_log(dd_log) == []


def test_a_pinned_manifest_steam_will_not_serve_asks_for_a_re_pin(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", MANIFEST)
    code, payload, stderr = build(run, repo, client, tmp_path / "out")
    assert code == 4
    assert "not falling back" in stderr and "steam pin --game zomboid --target linux-42.21.0 --write" in stderr
    assert "steam pin" in payload["error"]
    assert not (tmp_path / "out").exists()


def test_a_download_that_selects_nothing_is_refused(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path
) -> None:
    edit_game(repo, lambda block: block["data"].update(files=["regex:^media/nothing/"]))
    code, payload, _ = build(run, repo, client, tmp_path / "out")
    assert code == 4 and "served no files" in payload["error"]


# ------------------------------------------------------------------ decode errors


def _set_pack(client: Path, name: str, data: bytes) -> None:
    (client / PACKS / name).write_bytes(data)


def _truncated_before_terminator(client: Path) -> None:
    data = (client / PACKS / "UI.pack").read_bytes()
    assert data.endswith(maker.V1_TERMINATOR)
    _set_pack(client, "UI.pack", data[:-4])


def _wrong_terminator(client: Path) -> None:
    data = (client / PACKS / "UI.pack").read_bytes()
    _set_pack(client, "UI.pack", data[:-4] + b"\x00\x00\x00\x00")


def _unsupported_version(client: Path) -> None:
    data = bytearray((client / PACKS / "UI2.pack").read_bytes())
    data[4:8] = struct.pack("<i", 2)
    _set_pack(client, "UI2.pack", bytes(data))


def _rectangle_outside_the_page(client: Path) -> None:
    page = maker.PackPage("UI20", (64, 64), [maker.Texture("Item_Axe", maker.RED, 50, 50, 20, 24, 0, 0, 32, 32)])
    maker.write_pack(client / PACKS / "UI2.pack", "v2", [page])


def _offset_outside_the_frame(client: Path) -> None:
    page = maker.PackPage("UI20", (64, 64), [maker.Texture("Item_Axe", maker.RED, 0, 0, 20, 24, 20, 20, 32, 32)])
    maker.write_pack(client / PACKS / "UI2.pack", "v2", [page])


def _unsafe_entry_name(client: Path) -> None:
    page = maker.PackPage("UI20", (64, 64), [maker.Texture("../x", maker.RED, 0, 0, 20, 24, 0, 0, 32, 32)])
    maker.write_pack(client / PACKS / "UI2.pack", "v2", [page])


def _truncated_v2(client: Path) -> None:
    data = (client / PACKS / "UI2.pack").read_bytes()
    _set_pack(client, "UI2.pack", data[: len(data) // 2])


@pytest.mark.parametrize(
    "damage",
    [
        _truncated_before_terminator,
        _wrong_terminator,
        _unsupported_version,
        _rectangle_outside_the_page,
        _offset_outside_the_frame,
        _unsafe_entry_name,
        _truncated_v2,
    ],
)
def test_a_malformed_pack_fails_the_build_and_writes_nothing(
    run: Any, repo: Path, client: Path, dd_log: Path, tmp_path: Path, damage: Any
) -> None:
    damage(client)
    out = tmp_path / "out"
    code, payload, _ = build(run, repo, client, out)
    assert code == 6, payload
    assert payload["pack"].endswith(".pack")
    assert not (out / "pack").exists()
    assert list((paths.cache_dir() / "icons" / "work").iterdir()) == []


def test_the_committed_packs_are_what_the_generator_writes(tmp_path: Path) -> None:
    maker.generate(tmp_path)
    committed = FIXTURE / "client" / "media" / "texturepacks"
    assert sorted(path.name for path in tmp_path.iterdir()) == sorted(path.name for path in committed.iterdir())
    for path in committed.iterdir():
        assert (tmp_path / path.name).read_bytes() == path.read_bytes(), path.name
