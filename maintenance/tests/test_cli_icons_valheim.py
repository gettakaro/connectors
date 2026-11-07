"""``icons inputs`` and the catalog-driven ``icons build --game valheim``, end to end, against fakes.

The server depot is the DepotDownloader stand-in serving ``fixtures/icons/valheim/depots``; the Unity decoder
container is ``fake_icons_decoder.py`` answering from the recorded outputs ``make_fixture.py`` generates. No
game file, game art or game name is involved. Every assertion is on exit codes, the JSON on stdout and the files
the command wrote.
"""

from __future__ import annotations

import importlib.util
import json
import shutil
import sys
from pathlib import Path
from typing import Any

import pytest
from PIL import Image

import fake_depotdownloader as fake_dd
import fake_icons_decoder as fake_decoder
from conftest import FIXTURES, REPO_ROOT
from takaro_maint import paths

FIXTURE = FIXTURES / "icons" / "valheim"
TARGET = "linux-1.0.21"
BUNDLE_SELECTOR = "regex:^valheim_server_Data/StreamingAssets/SoftRef/Bundles/[0-9a-f]+$"


def _load_maker() -> Any:
    spec = importlib.util.spec_from_file_location("valheim_make_fixture", FIXTURE / "make_fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


maker = _load_maker()

# What each fixture prefab must come out as: (rule, sprite path id) for the matched codes.
MATCHED = {
    "SynthPlain": ("icon", 101),
    "SynthTri": ("first-of-variants", 102),
    "SynthDup": ("icon", 105),
    "SynthTwinA": ("icon", 108),
    "SynthTwinB": ("icon", 108),
    "SynthRoundDown": ("icon", 109),
    "SynthRoundUp": ("icon", 110),
    "SynthHalfEvenLow": ("icon", 111),
    "SynthHalfEvenHigh": ("icon", 112),
    "SynthFlipH": ("icon", 113),
    "SynthFlipV": ("icon", 114),
    "SynthTurn180": ("icon", 115),
    "SynthTurn90": ("icon", 116),
    "SynthTrimmed": ("icon", 117),
    "SynthViaAtlas": ("icon", 118),
}
UNMATCHED = {
    "SynthTight": "unsupported-sprite",
    "SynthAlpha": "unsupported-sprite",
    "SynthBadFormat": "unsupported-texture",
    "SynthLost": "unresolved-reference",
    "SynthBlank": "no-icon-in-game",
    "SynthHollow": "no-icon-in-game",
}


class Env:
    def __init__(self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
        self.tmp_path = tmp_path
        self.monkeypatch = monkeypatch
        self.repo = tmp_path / "repo"
        self.depots = tmp_path / "depots"
        self.decoder = tmp_path / "decoder"
        self.dd_log = tmp_path / "dd.jsonl"
        self.decoder_log = tmp_path / "decoder.jsonl"
        self.state = tmp_path / "decoder-state"
        shutil.copytree(REPO_ROOT / "catalog", self.repo / "catalog")
        (self.repo / "maintenance").mkdir()
        shutil.copytree(
            REPO_ROOT / "maintenance" / "decoders",
            self.repo / "maintenance" / "decoders",
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        shutil.copyfile(REPO_ROOT / "maintenance" / "tools.lock.json", self.repo / "maintenance" / "tools.lock.json")
        target = self.repo / "catalog" / "valheim" / "targets" / f"{TARGET}.json"
        record = json.loads(target.read_text())
        record["inputs"]["server"]["depots"][maker.DEPOT]["manifest"] = maker.MANIFEST
        target.write_text(json.dumps(record, indent=2) + "\n")
        shutil.copytree(FIXTURE / "depots", self.depots)
        shutil.copytree(FIXTURE / "decoder", self.decoder)
        monkeypatch.setenv("TAKARO_MAINT_DEPOTDOWNLOADER", str(Path(fake_dd.__file__).resolve()))
        monkeypatch.setenv("FAKE_DD_ROOT", str(self.depots))
        monkeypatch.setenv("FAKE_DD_LOG", str(self.dd_log))
        monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(Path(fake_decoder.__file__).resolve()))
        monkeypatch.setenv("FAKE_DECODER_ROOT", str(self.decoder))
        monkeypatch.setenv("FAKE_DECODER_STATE", str(self.state))
        monkeypatch.setenv("FAKE_DECODER_LOG", str(self.decoder_log))

    def downloads(self) -> list[list[str]]:
        return [argv for argv in fake_dd.argv_log(self.dd_log) if "-filelist" in argv]

    def decoder_runs(self) -> list[list[str]]:
        return [argv for argv in fake_decoder.argv_log(self.decoder_log) if argv[:1] == ["run"]]

    def edit_decoder_json(self, name: str, change: Any) -> None:
        path = self.decoder / "json" / f"{name}.json"
        document = json.loads(path.read_text())
        change(document)
        path.write_text(json.dumps(document, indent=2) + "\n")


@pytest.fixture
def env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Env:
    return Env(tmp_path, monkeypatch)


def build(run: Any, env: Env, out: Path, *extra: str) -> tuple[int, Any, str]:
    return run("icons", "build", "--game", "valheim", "--out", str(out), *extra, repo=env.repo)


def tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def index_of(out: Path) -> dict[str, str]:
    return json.loads((out / "pack" / "valheim" / "index.json").read_text())["icons"]  # type: ignore[no-any-return]


def icon(out: Path, code: str) -> Image.Image:
    return Image.open(out / "pack" / "valheim" / index_of(out)[code]).convert("RGBA")


@pytest.fixture
def built(run: Any, env: Env, tmp_path: Path) -> Path:
    out = tmp_path / "out"
    code, payload, stderr = build(run, env, out)
    assert code == 0, (payload, stderr)
    return out


# ------------------------------------------------------------------------------- catalog and inputs


def test_the_real_catalog_validates_the_valheim_block(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")
    assert code == 0, payload
    checks = [c for c in payload["checks"] if c["id"].startswith("icons-") and c["file"].endswith("valheim/game.json")]
    assert {c["id"] for c in checks} >= {"icons-server-pixels-decoder", "icons-data-input", "icons-mapper-known"}
    assert all(c["status"] == "pass" for c in checks)


@pytest.mark.parametrize(
    ("change", "check"),
    [
        (lambda block: block["pixels"].update(decoder="unreal"), "game-schema"),
        (lambda block: block["data"].update(decoder="bogus"), "game-schema"),
        (lambda block: block["data"].pop("decoder"), "icons-server-pixels-decoder"),
    ],
    ids=["pixels-decoder-unreal", "bogus-data-decoder", "pixels-decoder-differs-from-data"],
)
def test_a_bad_unity_block_fails_validation(run: Any, catalog_copy: Path, change: Any, check: str) -> None:
    path = catalog_copy / "catalog" / "valheim" / "game.json"
    record = json.loads(path.read_text())
    change(record["icons"])
    path.write_text(json.dumps(record, indent=2) + "\n")
    code, payload, _ = run("catalog", "validate", repo=catalog_copy)
    assert code == 2
    assert check in {entry["id"] for entry in payload["checks"] if entry["status"] == "fail"}


def test_inputs_shows_server_pixels_with_a_decoder_and_runs_nothing(run: Any, env: Env) -> None:
    code, payload, _ = run("icons", "inputs", "--game", "valheim", repo=env.repo)
    assert code == 0, payload
    data, pixels = payload["inputs"]
    assert data["role"] == "data" and data["files"] == [BUNDLE_SELECTOR] and list(data["depots"]) == [maker.DEPOT]
    assert pixels["role"] == "pixels" and pixels["source"] == "server-depot" and pixels["decoder"] == "unity"
    assert pixels["files"] == [BUNDLE_SELECTOR]
    assert not {"app", "depot", "clientManifest", "suggestedMirror", "expectedClientBuilds"} & set(pixels)
    state = payload["unity"]["decoder"]
    assert state["name"] == "unity" and len(state["context"]) == 64 and state["imageBuilt"] is False
    assert env.downloads() == [] and env.decoder_runs() == []
    assert not [argv for argv in fake_decoder.argv_log(env.decoder_log) if argv[:1] == ["build"]]


# ----------------------------------------------------------------------------------- the build


def test_the_build_maps_the_fixture_to_a_pack(built: Path) -> None:
    coverage = json.loads((built / "coverage.json").read_text())
    icons = index_of(built)
    assert set(icons) == set(MATCHED)
    assert coverage["total"] == len(MATCHED) + len(UNMATCHED)
    assert coverage["matched"] == len(MATCHED) and coverage["unmatched"] == len(UNMATCHED)
    assert coverage["byRule"]["icon"]["matched"] == len(MATCHED) - 1
    assert coverage["byRule"]["first-of-variants"] == {"total": 1, "matched": 1, "unmatched": 0}
    assert coverage["unmatchedByReason"] == {
        "no-icon-in-game": 2,
        "unresolved-reference": 1,
        "unsupported-sprite": 2,
        "unsupported-texture": 1,
    }
    assert {entry["code"]: entry["reason"] for entry in coverage["unmatchedCodes"]} == UNMATCHED
    details = {entry["code"]: entry.get("detail") for entry in coverage["unmatchedCodes"]}
    assert details["SynthTight"] == "tight-packed" and details["SynthAlpha"] == "separate-alpha"
    assert details["SynthLost"] == "cab-x:1" and "SYNTH_UNSUPPORTED" in str(details["SynthBadFormat"])
    assert coverage["measuredAgainst"] == "game-data"
    assert "ZNetScene.m_prefabs" in coverage["codesFrom"] and "#811" in coverage["codesFrom"]


def test_every_code_comes_out_as_the_pixels_the_game_draws(built: Path) -> None:
    for code, (_, sprite_id) in MATCHED.items():
        expected = maker.expected_image(maker.SPRITES[sprite_id])
        image = icon(built, code)
        assert image.size == expected.size, code
        assert image.tobytes() == expected.convert("RGBA").tobytes(), code


def test_the_cross_bundle_chain_the_first_variant_and_the_first_copy_decide_the_icon(built: Path) -> None:
    atlas = maker.atlas_image()
    first_cell = atlas.crop((0, 0, 8, 8))
    # SynthTri: the first of three variant sprites is the cell at atlas (0, 40) from the bottom-left.
    assert icon(built, "SynthTri").tobytes() == atlas.crop((0, 0, 8, 8)).tobytes()
    assert icon(built, "SynthTri").tobytes() != atlas.crop((8, 0, 16, 8)).tobytes()
    # SynthDup: the first ZNetScene copy (sprite 105) wins over the later copy (sprite 106).
    assert icon(built, "SynthDup").tobytes() == atlas.crop((24, 16, 32, 24)).tobytes()
    # SynthPlain: the unreferenced namesake (sprite 107) is never used.
    assert icon(built, "SynthPlain").tobytes() == atlas.crop((16, 32, 24, 40)).tobytes()
    assert first_cell.size == (8, 8)


def test_the_trimmed_sprite_keeps_its_frame_with_transparent_padding(built: Path) -> None:
    image = icon(built, "SynthTrimmed")
    assert image.size == (16, 16)
    assert image.getpixel((0, 0))[3] == 0 and image.getpixel((15, 15))[3] == 0
    assert image.getpixel((4, 6))[3] == 255  # the cut's top-left corner: left 4, top 16 - 2 - 8


def test_two_codes_with_one_sprite_give_one_object(built: Path) -> None:
    icons = index_of(built)
    assert icons["SynthTwinA"] == icons["SynthTwinB"]
    assert json.loads((built / "coverage.json").read_text())["objects"] < len(icons)


def test_the_reference_check_passes_on_every_sampled_sprite(built: Path) -> None:
    record = json.loads((built / "build-record.json").read_text())
    check = record["mapper"]["referenceCheck"]
    assert check["sampled"] == check["equal"] == 14 and check["rotated"] == 4
    assert "flagsByName" not in json.loads((built / "coverage.json").read_text())


def test_a_cut_that_differs_from_the_reference_is_flagged_not_failed(run: Any, env: Env, tmp_path: Path) -> None:
    altered = Image.open(env.decoder / "png" / "reference-111.png").convert("RGBA")
    altered.putpixel((0, 0), (1, 2, 3, 255))
    altered.save(env.decoder / "png" / "reference-111.png", format="PNG")
    out = tmp_path / "out"
    code, payload, _ = build(run, env, out)
    assert code == 0, payload
    coverage = json.loads((out / "coverage.json").read_text())
    assert coverage["flagsByName"] == {"cut-differs-from-reference": 1}
    assert coverage["flaggedCodes"] == [{"code": "SynthHalfEvenLow", "flag": "cut-differs-from-reference"}]
    record = json.loads((out / "build-record.json").read_text())
    assert record["mapper"]["referenceCheck"]["equal"] == record["mapper"]["referenceCheck"]["sampled"] - 1
    assert any("differ from UnityPy's cut" in note for note in record["mapper"]["notes"])


def test_the_build_record_names_the_decoder_and_what_the_pixels_came_from(
    run: Any, env: Env, built: Path, tmp_path: Path
) -> None:
    record = json.loads((built / "build-record.json").read_text())
    stored = next(v for k, v in json.loads(env.state.joinpath("images.json").read_text()).items() if k != "_builds")
    assert record["decoder"]["name"] == "unity" and record["decoder"]["imageId"] == stored["Id"]
    assert record["decoder"]["contextSha256"] == stored["label"] and record["decoder"]["tool"]["name"] == (
        "fake-unity-export"
    )
    assert "client" not in record
    assert record["data"]["buildid"] == 26266951 and record["data"]["depots"] == {maker.DEPOT: maker.MANIFEST}
    assert record["data"]["selectors"] == [BUNDLE_SELECTOR]
    pixels = record["pixels"]
    assert (
        pixels["source"] == "server-depot" and pixels["decoder"] == "unity" and pixels["selectors"] == [BUNDLE_SELECTOR]
    )
    [(name, atlas)] = pixels["atlases"].items()
    assert name == "atlases/tx00001.png" and len(atlas["sha256"]) == 64
    assert (atlas["width"], atlas["height"], atlas["format"]) == (64, 48, "RGBA32")
    assert pixels["sprites"] == 18  # 14 cut, 4 unmatched
    mapper = record["mapper"]
    assert mapper["name"] == "valheim"
    notes = " | ".join(mapper["notes"])
    for expected in (
        "1 null prefab entries",
        "1 prefabs from no bundle",
        "1 prefabs without an ItemDrop",
        "1 repeated prefab names",
        "1 prefabs with several icons",
    ):
        assert expected in notes
    assert mapper["bundles"] == 5 and mapper["multiIcon"] == 1 and mapper["unresolvedPrefabs"] == 1
    assert mapper["atlases"] == {"count": 1, "formats": {"RGBA32": 1}, "sizes": [[64, 48]]}
    assert mapper["packing"]["rotations"]["rotate-90"] == 1 and mapper["packing"]["trimmed"] == 1
    assert "atlasFiles" not in mapper


def test_stdout_reports_the_decoder_and_no_client(run: Any, env: Env, tmp_path: Path) -> None:
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 0, payload
    assert "clientBuild" not in payload
    assert payload["decoder"]["name"] == "unity" and len(payload["decoder"]["contextSha256"]) == 64
    assert payload["inputs"]["pixels"] == {"source": "server-depot", "decoder": "unity", "atlases": 1}


# ---------------------------------------------------------------------------------------- hardening


def test_the_decoder_runs_hardened_with_a_read_only_game_mount(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "out")[0] == 0
    runs = env.decoder_runs()
    assert len(runs) == 4  # scan, sprites, textures, references
    for argv in runs:
        assert argv[argv.index("--network") + 1] == "none"
        assert "--read-only" in argv and "no-new-privileges" in argv and "--cap-drop" in argv
        mounts = [argv[i + 1] for i, item in enumerate(argv) if item == "-v"]
        assert [m for m in mounts if not m.endswith(":ro")] == [m for m in mounts if m.endswith(":/out")]
        assert sum(m.endswith(":/game:ro") for m in mounts) == 1
        assert "-e" not in argv


def test_a_planted_symlink_in_the_decoder_output_fails_the_build(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DECODER_PLANT", "symlink")
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 6 and not (out / "pack").exists()


def test_the_filtered_download_fetches_only_bundles_and_a_second_build_changes_nothing(
    run: Any, env: Env, tmp_path: Path
) -> None:
    assert build(run, env, tmp_path / "one")[0] == 0
    assert len(env.downloads()) == 1
    record = json.loads((tmp_path / "one" / "build-record.json").read_text())
    assert record["data"]["selectors"] == [BUNDLE_SELECTOR]  # the one selector the filelist was written from
    assert record["data"]["files"] == len(maker.BUNDLES)  # the four decoys are not among the downloaded files
    runs_before = len(env.decoder_runs())
    assert build(run, env, tmp_path / "two")[0] == 0
    assert len(env.downloads()) == 1 and len(env.decoder_runs()) == runs_before
    assert tree(tmp_path / "one") == tree(tmp_path / "two")


def test_a_fresh_decode_gives_the_same_pack(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "one")[0] == 0
    shutil.rmtree(paths.cache_dir() / "icons" / "decoded")
    assert build(run, env, tmp_path / "two")[0] == 0
    assert tree(tmp_path / "one") == tree(tmp_path / "two")


# --------------------------------------------------------------------------------- refusals and failures


def test_a_client_is_refused_before_any_download(run: Any, env: Env, tmp_path: Path) -> None:
    client = tmp_path / "client"
    client.mkdir()
    code, payload, _ = build(run, env, tmp_path / "out", "--client", str(client))
    assert code == 2 and "--client" in payload["error"]
    assert env.downloads() == [] and fake_dd.argv_log(env.dd_log) == []


def test_a_missing_docker_is_refused_before_any_download(run: Any, env: Env, tmp_path: Path) -> None:
    env.monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(tmp_path / "no-such-docker"))
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 2 and "docker is required" in payload["error"]
    assert fake_dd.argv_log(env.dd_log) == []


def test_no_znetscene_is_a_failed_build(run: Any, env: Env, tmp_path: Path) -> None:
    env.edit_decoder_json("znetscene", lambda document: document.clear())
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and "ZNetScene" in payload["error"]


def test_two_znetscenes_with_different_prefabs_are_a_failed_build(run: Any, env: Env, tmp_path: Path) -> None:
    def second(document: list[dict[str, Any]]) -> None:
        other = json.loads(json.dumps(document[0]))
        other["bundle"] = f"{maker.BUNDLES_DIR}/aaaa0003"
        other["fields"]["m_prefabs"] = other["fields"]["m_prefabs"][:3]
        document.append(other)

    env.edit_decoder_json("znetscene", second)
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and len(payload["bundles"]) == 2


def test_identical_znetscenes_are_counted_and_the_first_is_used(run: Any, env: Env, tmp_path: Path) -> None:
    def second(document: list[dict[str, Any]]) -> None:
        other = json.loads(json.dumps(document[0]))
        other["bundle"] = f"{maker.BUNDLES_DIR}/aaaa0003"
        document.append(other)

    env.edit_decoder_json("znetscene", second)
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    record = json.loads((out / "build-record.json").read_text())
    assert any("2 identical ZNetScene" in note for note in record["mapper"]["notes"])


def test_one_cab_in_two_bundles_is_a_failed_build(run: Any, env: Env, tmp_path: Path) -> None:
    env.edit_decoder_json("bundle-index", lambda document: document[1]["cabs"].append("cab-a"))
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and "cab-a" in payload["error"]


def test_a_decoder_that_exits_non_zero_fails_the_build(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DECODER_EXIT", "3")
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and payload["exit"] == 3 and payload["stderr"]


def test_a_failed_export_fails_the_build(run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("FAKE_DECODER_FAIL", "sp00001")
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and any("sp00001" in line for line in payload["failures"])


def test_a_depot_with_no_bundles_is_unavailable(run: Any, env: Env, tmp_path: Path) -> None:
    tree_root = env.depots / maker.DEPOT / maker.MANIFEST / "tree"
    shutil.rmtree(tree_root / maker.BUNDLES_DIR)
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 4, payload


def test_an_unavailable_manifest_is_unavailable_with_the_repin_hint(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", maker.MANIFEST)
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 4 and "re-pin" in json.dumps(payload).lower()


# ------------------------------------------------------------------------------------ the fixture files


def test_the_committed_fixture_files_equal_the_generators_output(tmp_path: Path) -> None:
    regenerated = maker.all_files(tmp_path)
    committed = {
        str(path.relative_to(FIXTURE)): path.read_bytes()
        for path in sorted(FIXTURE.rglob("*"))
        if path.is_file()
        and path.relative_to(FIXTURE).parts[0] in ("decoder", "depots")
        and "__pycache__" not in path.parts
    }
    assert committed == regenerated


def test_the_fixture_holds_no_real_looking_bundle_bytes() -> None:
    for name in maker.BUNDLES:
        data = (FIXTURE / "depots" / maker.DEPOT / maker.MANIFEST / "tree" / maker.BUNDLES_DIR / name).read_bytes()
        assert data == f"fixture bundle {name}\n".encode()
        assert not data.startswith(b"UnityFS")
