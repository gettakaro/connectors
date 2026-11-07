"""``icons inputs`` and the catalog-driven ``icons build --game terraria``, end to end, against fakes.

The item codes are a synthetic catalogue in the repository copy, the client is a synthetic tree, and the XNB and CLR
decoder containers are ``fake_icons_decoder.py`` answering from the recorded outputs ``make_fixture.py`` generates.
No game file, game art, decompiled game text or game name is involved. Every assertion is on exit codes, the JSON on
stdout and the files the command wrote.
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

import fake_depotdownloader as fake_dd
import fake_icons_decoder as fake_decoder
from conftest import FIXTURES, REPO_ROOT
from takaro_maint import paths

FIXTURE = FIXTURES / "icons" / "terraria"
CATALOGUE_PATH = Path("games") / "terraria" / "bridge" / "src" / "terraria" / "itemCatalog.ts"


def _load_maker() -> Any:
    spec = importlib.util.spec_from_file_location("terraria_make_fixture", FIXTURE / "make_fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


maker = _load_maker()

# What each fixture item must come out as: the rule of the matched codes and the reason of the unmatched ones.
MATCHED = {
    "1": "item",
    "2": "animation-frame",
    "3": "food-frame",
    "4": "item",
    "5": "item",
    "10": "item",
    "11": "fake-chest-fallback",
    "12": "item",
    "13": "item",
    "17": "item",
    "5644": "frame-override",
}
UNMATCHED = {"14": "missing-texture", "15": "missing-texture", "16": "unsupported-texture"}
PIXEL_FILES = 12  # eleven items with a file plus the id the catalogue does not list; decoys and the assembly excluded


class Env:
    def __init__(self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
        self.tmp_path = tmp_path
        self.monkeypatch = monkeypatch
        self.repo = tmp_path / "repo"
        self.decoder = tmp_path / "decoder"
        self.dd_log = tmp_path / "dd.jsonl"
        self.decoder_log = tmp_path / "decoder.jsonl"
        self.state = tmp_path / "decoder-state"
        shutil.copytree(REPO_ROOT / "catalog", self.repo / "catalog")
        (self.repo / "maintenance").mkdir()
        shutil.copytree(
            REPO_ROOT / "maintenance" / "decoders",
            self.repo / "maintenance" / "decoders",
            ignore=shutil.ignore_patterns("__pycache__", "node_modules"),
        )
        shutil.copyfile(REPO_ROOT / "maintenance" / "tools.lock.json", self.repo / "maintenance" / "tools.lock.json")
        (self.repo / CATALOGUE_PATH).parent.mkdir(parents=True)
        shutil.copyfile(FIXTURE / "connector" / "itemCatalog.ts", self.repo / CATALOGUE_PATH)
        shutil.copytree(FIXTURE / "decoder", self.decoder)
        maker.write_client(tmp_path / "client")
        self.steamapps = tmp_path / "client" / "steamapps"
        self.client = self.steamapps / "common" / "Terraria"
        monkeypatch.setenv("TAKARO_MAINT_DEPOTDOWNLOADER", str(Path(fake_dd.__file__).resolve()))
        monkeypatch.setenv("FAKE_DD_ROOT", str(tmp_path / "no-depots"))
        monkeypatch.setenv("FAKE_DD_LOG", str(self.dd_log))
        monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(Path(fake_decoder.__file__).resolve()))
        monkeypatch.setenv("FAKE_DECODER_ROOT", str(self.decoder))
        monkeypatch.setenv("FAKE_DECODER_STATE", str(self.state))
        monkeypatch.setenv("FAKE_DECODER_LOG", str(self.decoder_log))

    def decoder_runs(self) -> list[list[str]]:
        return [argv for argv in fake_decoder.argv_log(self.decoder_log) if argv[:1] == ["run"]]

    def decoder_builds(self) -> list[list[str]]:
        return [argv for argv in fake_decoder.argv_log(self.decoder_log) if argv[:1] == ["build"]]

    def edit_decoder_json(self, name: str, change: Any) -> None:
        path = self.decoder / "json" / f"{name}.json"
        document = json.loads(path.read_text())
        change(document)
        path.write_text(json.dumps(document, indent=2) + "\n")

    def edit_block(self, change: Any) -> None:
        path = self.repo / "catalog" / "terraria" / "game.json"
        record = json.loads(path.read_text())
        change(record["icons"])
        path.write_text(json.dumps(record, indent=2) + "\n")


@pytest.fixture
def env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Env:
    return Env(tmp_path, monkeypatch)


def build(run: Any, env: Env, out: Path, *extra: str, client: Path | None = None) -> tuple[int, Any, str]:
    return run(
        "icons",
        "build",
        "--game",
        "terraria",
        "--client",
        str(client or env.client),
        "--out",
        str(out),
        *extra,
        repo=env.repo,
    )


def tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def index_of(out: Path) -> dict[str, str]:
    return json.loads((out / "pack" / "terraria" / "index.json").read_text())["icons"]  # type: ignore[no-any-return]


def icon(out: Path, code: str) -> Image.Image:
    return Image.open(out / "pack" / "terraria" / index_of(out)[code]).convert("RGBA")


def record_of(out: Path) -> dict[str, Any]:
    return json.loads((out / "build-record.json").read_text())  # type: ignore[no-any-return]


def mirror_of(env: Env) -> Path:
    """The host side of the ``/game`` mount of the first decoder run."""
    argv = env.decoder_runs()[0]
    return Path(
        next(argv[i + 1] for i, item in enumerate(argv) if item == "-v" and argv[i + 1].endswith(":/game:ro"))[:-9]
    )


@pytest.fixture
def built(run: Any, env: Env, tmp_path: Path) -> Path:
    out = tmp_path / "out"
    code, payload, stderr = build(run, env, out)
    assert code == 0, (payload, stderr)
    return out


# ------------------------------------------------------------------------------- catalog and inputs


def test_the_real_catalog_validates_the_terraria_block(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")
    assert code == 0, payload
    checks = [c for c in payload["checks"] if c["id"].startswith("icons-") and c["file"].endswith("terraria/game.json")]
    assert {c["id"] for c in checks} >= {
        "icons-client-regex",
        "icons-code-role",
        "icons-xnb-settings",
        "icons-data-input",
    }
    assert all(c["status"] == "pass" for c in checks)


@pytest.mark.parametrize(
    ("change", "check"),
    [
        (lambda block: block["data"]["files"].append("games/terraria/bridge/src/terraria/nope.ts"), "icons-data-input"),
        (lambda block: block["pixels"].update(access="iostore-ranges"), "icons-client-regex"),
        (lambda block: block["pixels"].update(files=["regex:^(unclosed"]), "icons-client-regex"),
        (
            lambda block: block.update(pixels={"source": "server-depot", "files": ["Content/x"]}, xnb=None),
            "icons-code-role",
        ),
        (lambda block: block.pop("xnb"), "icons-xnb-settings"),
        (lambda block: block["pixels"].update(decoder="pz-texturepack"), "icons-xnb-settings"),
        (lambda block: block["data"].update(input="server"), "game-schema"),
        (lambda block: block["code"].update(decoder="unreal"), "game-schema"),
        (lambda block: block["xnb"].update(alpha="premultiplied"), "game-schema"),
        (lambda block: block.pop("code"), "icons-connector-route"),
        (lambda block: block["pixels"].update(decoder="unreal", files=["regex:^a"]), "icons-client-regex"),
    ],
    ids=[
        "connector-file-missing",
        "regex-with-iostore-ranges",
        "regex-does-not-compile",
        "code-with-server-pixels",
        "xnb-decoder-without-settings",
        "xnb-settings-without-decoder",
        "connector-data-with-input",
        "unknown-code-decoder",
        "unknown-alpha",
        "connector-data-without-code",
        "regex-without-the-xnb-decoder",
    ],
)
def test_a_bad_terraria_block_fails_validation(run: Any, catalog_copy: Path, change: Any, check: str) -> None:
    path = catalog_copy / "catalog" / "terraria" / "game.json"
    record = json.loads(path.read_text())
    change(record["icons"])
    if record["icons"].get("xnb", 0) is None:
        record["icons"].pop("xnb")
    path.write_text(json.dumps(record, indent=2) + "\n")
    code, payload, _ = run("catalog", "validate", repo=catalog_copy)
    assert code == 2
    assert check in {entry["id"] for entry in payload["checks"] if entry["status"] == "fail"}


def test_inputs_shows_the_three_roles_and_both_decoders_and_runs_nothing(run: Any, env: Env) -> None:
    code, payload, _ = run("icons", "inputs", "--game", "terraria", repo=env.repo)
    assert code == 0, payload
    data, code_role, pixels = payload["inputs"]
    sha = hashlib.sha256((FIXTURE / "connector" / "itemCatalog.ts").read_bytes()).hexdigest()
    assert data == {"role": "data", "source": "connector", "files": {CATALOGUE_PATH.as_posix(): sha}}
    assert code_role == {"role": "code", "source": "client", "decoder": "clr", "files": ["Terraria.exe"]}
    assert pixels["role"] == "pixels" and pixels["decoder"] == "xnb" and pixels["app"] == 105600
    assert pixels["files"] == ["regex:^Content/Images/Item_[0-9]+\\.xnb$"]
    assert pixels["expectedClientBuilds"] == [maker.BUILD_ID]
    assert sorted(payload["decoders"]) == ["clr", "xnb"]
    assert all(len(state["context"]) == 64 and state["imageBuilt"] is False for state in payload["decoders"].values())
    assert not fake_dd.argv_log(env.dd_log) and env.decoder_runs() == [] and env.decoder_builds() == []


# ----------------------------------------------------------------------------------- the build


def test_the_build_maps_the_fixture_to_a_pack(built: Path) -> None:
    coverage = json.loads((built / "coverage.json").read_text())
    assert set(index_of(built)) == set(MATCHED)
    assert coverage["total"] == len(MATCHED) + len(UNMATCHED)
    assert coverage["matched"] == len(MATCHED) and coverage["unmatched"] == len(UNMATCHED)
    assert coverage["byRule"]["item"] == {"total": 7, "matched": 7, "unmatched": 0}
    for rule in ("animation-frame", "food-frame", "frame-override", "fake-chest-fallback"):
        assert coverage["byRule"][rule] == {"total": 1, "matched": 1, "unmatched": 0}
    assert coverage["unmatchedByReason"] == {"missing-texture": 2, "unsupported-texture": 1}
    recipes = json.loads((built / "recipes.json").read_text())
    assert {recipe["code"]: recipe["rule"] for recipe in recipes["recipes"]} == MATCHED
    assert {entry["code"]: entry["reason"] for entry in recipes["unmatched"]} == UNMATCHED
    assert "itemCatalog.ts" in recipes["codesFrom"] and "#811" in recipes["codesFrom"]
    sha = hashlib.sha256((FIXTURE / "connector" / "itemCatalog.ts").read_bytes()).hexdigest()
    assert sha in recipes["codesFrom"]


def test_animated_items_show_their_first_frame_without_the_gap(built: Path) -> None:
    for code, seed in (("2", 1), ("3", 2), ("5644", 3)):
        image = icon(built, code)
        expected = maker.expected_first_frame(maker.STRIP_FRAMES[code], seed)
        assert image.size == expected.size == (maker.WIDTH, maker.BODY)
        assert image.tobytes() == expected.tobytes(), code


def test_a_translucent_pixel_keeps_its_straight_rgba(built: Path) -> None:
    assert icon(built, "17").getpixel(maker.GLASS_PIXEL) == maker.GLASS_COLOR


def test_a_fake_chest_without_art_shares_the_real_chests_object_and_one_with_art_keeps_it(built: Path) -> None:
    icons = index_of(built)
    assert icons["11"] == icons["10"]
    assert icons["12"] not in (icons["10"], icons["13"])
    assert "14" not in icons and "15" not in icons


def test_identical_pixels_share_one_object(built: Path) -> None:
    icons = index_of(built)
    assert icons["4"] == icons["5"]
    assert len(set(icons.values())) == len(icons) - 2  # the twins and the fallback


def test_the_build_record_names_what_was_built_from(built: Path) -> None:
    record = record_of(built)
    sha = hashlib.sha256((FIXTURE / "connector" / "itemCatalog.ts").read_bytes()).hexdigest()
    assert record["data"]["source"] == "connector" and set(record["data"]["files"]) == {CATALOGUE_PATH.as_posix()}
    assert record["data"]["files"][CATALOGUE_PATH.as_posix()]["sha256"] == sha and "app" not in record["data"]
    assert record["client"]["buildid"] == maker.BUILD_ID and record["client"]["status"] == "match"
    assert record["code"]["decoder"] == "clr" and record["code"]["assembly"] == maker.FRAME_ASSEMBLY
    assert list(record["code"]["files"]) == ["Terraria.exe"]
    pixels = record["pixels"]
    assert pixels["decoder"] == "xnb" and pixels["files"] == PIXEL_FILES and pixels["alpha"] == "straight"
    assert pixels["used"] == len(MATCHED) - 1  # every matched item but the fallback has a texture of its own
    assert len(pixels["treeSha256"]) == 64 and pixels["bytes"] > 0
    assert [entry["name"] for entry in record["decoders"]] == ["xnb", "clr"]
    assert all(
        entry["imageId"].startswith("sha256:") and len(entry["contextSha256"]) == 64 for entry in record["decoders"]
    )
    assert record["decoders"][0]["tool"]["name"] == "fake-xnb-export"
    assert record["decoders"][1]["tool"]["name"] == "fake-clr-export"
    details = record["mapper"]
    assert details["catalogue"] == {"entries": len(maker.ITEMS), "sha256": sha}
    assert details["assembly"] == maker.FRAME_ASSEMBLY
    animations = details["animations"]
    assert (animations["registered"], animations["food"], animations["unparsed"], animations["conflicts"]) == (
        3,
        1,
        0,
        0,
    )
    assert animations["overrides"] == {"5644": {"frames": 9, "registeredAs": "DrawAnimationScryingOrb"}}
    assert details["frames"] == {"nonDivisible": 0} and details["gapNotTransparent"] == 0
    assert details["alpha"] == {"translucentPixels": 1, "channelAboveAlpha": 1}
    assert details["clientOnlyIds"] == 1 and details["fallbacks"] == 1


def test_stdout_reports_the_inputs_the_client_build_and_the_decoders(run: Any, env: Env, tmp_path: Path) -> None:
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 0
    assert payload["inputs"]["data"]["source"] == "connector" and list(payload["inputs"]["data"]["files"]) == [
        CATALOGUE_PATH.as_posix()
    ]
    assert payload["inputs"]["pixels"] == {"source": "client", "decoder": "xnb", "files": PIXEL_FILES}
    assert payload["clientBuild"] == {"id": maker.BUILD_ID, "status": "match", "expected": [maker.BUILD_ID]}
    assert [entry["name"] for entry in payload["decoders"]] == ["xnb", "clr"]
    assert fake_dd.argv_log(env.dd_log) == []


# ---------------------------------------------------------------------------------------- hardening


def test_the_decoders_run_hardened_on_the_mirror_and_never_on_the_client(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "out")[0] == 0
    runs = env.decoder_runs()
    assert len(runs) == 3  # the textures, the assembly facts and the decompiled types: one run each
    for argv in runs:
        assert ["--network", "none"] == argv[argv.index("--network") : argv.index("--network") + 2]
        assert "--read-only" in argv and "no-new-privileges" in argv and "--cap-drop" in argv
        mounts = [argv[i + 1] for i, item in enumerate(argv) if item == "-v"]
        assert [m for m in mounts if not m.endswith(":ro")] == [m for m in mounts if m.endswith(":/out")]
        game = [m for m in mounts if m.endswith(":/game:ro")]
        assert len(game) == 1 and "client-files" in game[0] and str(env.client) not in game[0]
        assert "-e" not in argv


def test_the_mirror_holds_the_selected_files_and_no_decoy(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "out")[0] == 0
    mirror = mirror_of(env)
    held = {path.relative_to(mirror).as_posix() for path in mirror.rglob("*") if path.is_file()}
    wanted = {"Terraria.exe", ".takaro/complete.json"}
    wanted |= {
        f"Content/Images/Item_{code}.xnb"
        for code in [*maker.CLIENT_ONLY, *[c for c, (_, k) in maker.ITEMS.items() if k != "none"]]
    }
    assert held == wanted
    assert not [name for name in held if "Item_7" in name or "Projectile" in name or "sub/" in name or "Item_x" in name]


def test_a_second_build_copies_and_decodes_nothing_and_is_byte_identical(run: Any, env: Env, tmp_path: Path) -> None:
    first, second = tmp_path / "first", tmp_path / "second"
    assert build(run, env, first)[0] == 0
    runs, mirror_files = len(env.decoder_runs()), tree(mirror_of(env))
    code, _, stderr = build(run, env, second)
    assert code == 0
    assert len(env.decoder_runs()) == runs and tree(mirror_of(env)) == mirror_files
    assert "already complete" in stderr and "already in the decode cache" in stderr
    assert tree(first / "pack") == tree(second / "pack")
    assert (first / "build-record.json").read_bytes() == (second / "build-record.json").read_bytes()


def test_a_tampered_mirror_is_copied_again_once(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "first")[0] == 0
    victim = mirror_of(env) / "Terraria.exe"
    victim.write_bytes(b"tampered")
    code, _, stderr = build(run, env, tmp_path / "second")
    assert code == 0 and "corrupt client files mirror" in stderr
    assert victim.read_bytes() == (env.client / "Terraria.exe").read_bytes()


def test_a_client_of_another_build_warns_and_still_builds(run: Any, env: Env, tmp_path: Path) -> None:
    manifest = env.steamapps / "appmanifest_105600.acf"
    manifest.write_text(manifest.read_text().replace(str(maker.BUILD_ID), str(maker.BUILD_ID + 100)))
    code, payload, stderr = build(run, env, tmp_path / "out")
    assert code == 0 and payload["clientBuild"]["status"] == "newer" and "warning:" in stderr


@pytest.mark.parametrize("plant", ["symlink", "hardlink"])
def test_a_hostile_output_tree_fails_the_build_and_writes_no_pack(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, plant: str
) -> None:
    monkeypatch.setenv("FAKE_DECODER_PLANT", plant)
    out = tmp_path / "out"
    code, payload, _ = build(run, env, out)
    assert code == 6 and not (out / "pack").exists(), payload


# ---------------------------------------------------------------------------------------- refusals


def test_no_client_is_a_usage_error(run: Any, env: Env, tmp_path: Path) -> None:
    code, payload, _ = run("icons", "build", "--game", "terraria", "--out", str(tmp_path / "out"), repo=env.repo)
    assert code == 2 and "--client" in payload["error"]


def test_a_client_without_the_assembly_is_refused_before_anything_runs(run: Any, env: Env, tmp_path: Path) -> None:
    (env.client / "Terraria.exe").unlink()
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 4 and payload["missing"] == ["Terraria.exe"]
    assert env.decoder_runs() == [] and env.decoder_builds() == []


def test_a_client_with_no_item_texture_is_refused_naming_the_selector(run: Any, env: Env, tmp_path: Path) -> None:
    for path in (env.client / "Content" / "Images").glob("Item_*.xnb"):
        path.unlink()
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 4 and payload["missing"] == ["regex:^Content/Images/Item_[0-9]+\\.xnb$"]
    assert env.decoder_runs() == [] and env.decoder_builds() == []


def test_a_missing_docker_is_refused_before_any_mirror_copy(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", "/nonexistent/docker")
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 2 and "docker" in payload["error"]
    assert not (paths.cache_dir() / "icons" / "client-files").exists()
    assert fake_dd.argv_log(env.dd_log) == []


def test_a_catalogue_of_another_shape_fails_the_build(run: Any, env: Env, tmp_path: Path) -> None:
    catalogue = env.repo / CATALOGUE_PATH
    catalogue.write_text(catalogue.read_text().replace("] as const;", "];"))
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and "catalogue format changed" in payload["error"]


@pytest.mark.parametrize(
    "replacement",
    [
        ('"code": "2"', '"code": "two"'),
        ('"name": "Synth Strip"', '"name": ""'),
        ('"code": "3"', '"code": "2"'),
    ],
    ids=["non-numeric-code", "empty-name", "duplicate-code"],
)
def test_a_catalogue_entry_of_another_shape_fails_the_build(
    run: Any, env: Env, tmp_path: Path, replacement: tuple[str, str]
) -> None:
    catalogue = env.repo / CATALOGUE_PATH
    catalogue.write_text(catalogue.read_text().replace(*replacement, 1))
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and "catalogue format changed" in payload["error"]


def _append_registration(line: str) -> Any:
    def change(document: dict[str, Any]) -> None:
        document["source"] = document["source"].replace("\t}\n}\n", f"\t\t{line}\n\t}}\n}}\n")

    return change


@pytest.mark.parametrize(
    "line",
    [
        "RegisterItemAnimation(7, new SomethingElse(1, 2));",
        "RegisterItemAnimation(k, new DrawAnimationVertical(6, 4));",
        "RegisterItemAnimation(2, new DrawAnimationVertical(6, 5));",
    ],
    ids=["unknown-animation-type", "unparsed-identifier", "conflicting-frame-count"],
)
def test_registrations_that_are_not_understood_fail_the_build(run: Any, env: Env, tmp_path: Path, line: str) -> None:
    env.edit_decoder_json("decompile-world", _append_registration(line))
    out = tmp_path / "out"
    code, payload, _ = build(run, env, out)
    assert code == 6 and not (out / "pack").exists()
    assert "not fully understood" in payload["error"]


def test_the_override_needs_its_registration_in_the_game_code(run: Any, env: Env, tmp_path: Path) -> None:
    env.edit_decoder_json(
        "decompile-world",
        lambda document: document.update(source=document["source"].replace("FrameCount = 9", "FrameCount = 8")),
    )
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and "5644" in json.dumps(payload)


def test_a_game_with_no_registering_method_fails_the_build(run: Any, env: Env, tmp_path: Path) -> None:
    env.edit_decoder_json("callers", lambda document: document.clear())
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and "no method" in payload["error"]


def test_a_game_with_no_food_loop_fails_the_build(run: Any, env: Env, tmp_path: Path) -> None:
    env.edit_decoder_json(
        "decompile-world", lambda document: document.update(source=document["source"].replace("IsFood", "IsDrink"))
    )
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and "IsFood" in payload["error"]


def test_decoder_failures_fail_the_build_and_write_no_pack(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DECODER_EXIT", "3")
    code, payload, _ = build(run, env, tmp_path / "exit")
    assert code == 6 and payload["exit"] == 3 and any("crashed on purpose" in line for line in payload["stderr"])
    assert not (tmp_path / "exit" / "pack").exists()
    monkeypatch.delenv("FAKE_DECODER_EXIT")

    monkeypatch.setenv("FAKE_DECODER_FAIL", "it5644")
    code, payload, _ = build(run, env, tmp_path / "fail")
    assert code == 6 and any("it5644" in line for line in payload["failures"])
    monkeypatch.delenv("FAKE_DECODER_FAIL")
    assert list((paths.cache_dir() / "icons" / "work").iterdir()) == []


def test_a_run_that_is_not_hardened_is_a_build_failure(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from takaro_maint.icons.decoders import container

    original = container.run_argv

    def writable(*args: Any, **kwargs: Any) -> list[str]:
        return [item.replace(":ro", "") for item in original(*args, **kwargs)]

    monkeypatch.setattr(container, "run_argv", writable)
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6 and any("refuses this run" in line for line in payload["stderr"])


# ---------------------------------------------------------------------------- the real connector catalogue


def test_the_real_item_catalogue_parses_to_unique_numeric_codes(run: Any, env: Env, tmp_path: Path) -> None:
    """Through ``icons build`` on a repo holding the real ``itemCatalog.ts``; only a few ids have art."""
    shutil.copyfile(REPO_ROOT / CATALOGUE_PATH, env.repo / CATALOGUE_PATH)
    out = tmp_path / "out"
    code, payload, _ = build(run, env, out)
    assert code == 0, payload
    coverage = json.loads((out / "coverage.json").read_text())
    assert coverage["total"] == 6147
    assert coverage["matched"] == len(index_of(out)) and coverage["unmatchedByReason"]["missing-texture"] > 6000


# ------------------------------------------------------------------------------------ the fixture files


def test_the_committed_fixture_files_equal_the_generators_output(tmp_path: Path) -> None:
    regenerated = maker.all_files(tmp_path)
    committed = {
        str(path.relative_to(FIXTURE)): path.read_bytes()
        for path in sorted(FIXTURE.rglob("*"))
        if path.is_file()
        and path.relative_to(FIXTURE).parts[0] in ("decoder", "connector")
        and "__pycache__" not in path.parts
    }
    assert committed == regenerated


def test_no_game_file_extension_is_committed_under_the_fixture() -> None:
    names = [path.name for path in FIXTURE.rglob("*") if path.is_file()]
    assert not [name for name in names if name.endswith((".xnb", ".exe", ".dll"))]


def test_the_fixture_texts_are_synthetic() -> None:
    for name in ("decompile-world", "decompile-sets"):
        source = json.loads((FIXTURE / "decoder" / "json" / f"{name}.json").read_text())["source"]
        assert "Synth" in source
    assert "Synth" in (FIXTURE / "connector" / "itemCatalog.ts").read_text()
