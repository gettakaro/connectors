"""``icons inputs`` and the catalog-driven ``icons build --game conan-exiles``, end to end, against fakes.

The server depot is the DepotDownloader stand-in serving ``fixtures/icons/conan-exiles/depots``; the Unreal
decoder container is ``fake_icons_decoder.py`` answering from recorded outputs; the client is a synthetic
IoStore install. No real game art is involved. Every assertion is on exit codes, the JSON on stdout and the
files the command wrote.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest
from PIL import Image

import fake_depotdownloader as fake_dd
import fake_icons_decoder as fake_decoder
from conftest import FIXTURES, REPO_ROOT
from fake_upstream import FakeUpstream
from takaro_maint import paths

FIXTURE = FIXTURES / "icons" / "conan-exiles"
MANIFEST = "7440000000000000043"
TARGET = "linux-25792439"
PAKS = "ConanSandbox/Content/Paks"
OODLE_BLOB = b"dummy oodle library bytes"
BORDER = (255, 215, 0, 255)


def _load_maker() -> Any:
    spec = importlib.util.spec_from_file_location("conan_make_fixture", FIXTURE / "make_fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


maker = _load_maker()
SELECTION = json.loads((FIXTURE / "selection.json").read_text())["roles"]
ROWS = maker.item_rows()
INDEX_ENTRIES = json.loads((FIXTURE / "decoder" / "index.json").read_text())


BASE_NAMES = json.loads((FIXTURE / "decoder" / "json" / "name-table.json").read_text())[0]["Rows"]
DLC_NAMES = json.loads((FIXTURE / "decoder" / "json" / "dlc-name-table.json").read_text())[0]["Rows"]


def code_of(row: str) -> str:
    """The connector's code for a template id: its first base name-table row name, else the id itself."""
    if row.isdigit():
        for name, value in BASE_NAMES.items():
            if next(iter(value.values())) == int(row):
                return name
    return row


def codes(rows: list[str]) -> list[str]:
    return [code_of(row) for row in rows]


def package_of(row: str) -> str:
    return maker.soft_packages(ROWS[row])[0]


class Env:
    def __init__(self, tmp_path: Path, upstream: FakeUpstream, monkeypatch: pytest.MonkeyPatch) -> None:
        self.tmp_path = tmp_path
        self.upstream = upstream
        self.monkeypatch = monkeypatch
        self.repo = tmp_path / "repo"
        self.client = tmp_path / "client"
        self.dd_log = tmp_path / "dd.jsonl"
        self.decoder_log = tmp_path / "decoder.jsonl"
        self.state = tmp_path / "decoder-state"
        shutil.copytree(REPO_ROOT / "catalog", self.repo / "catalog")
        (self.repo / "maintenance").mkdir()
        shutil.copytree(REPO_ROOT / "maintenance" / "decoders", self.repo / "maintenance" / "decoders")
        lock = json.loads((REPO_ROOT / "maintenance" / "tools.lock.json").read_text())
        lock["tools"]["oodle"].update(
            url=f"{upstream.base_url}/oodle.so", sha256=hashlib.sha256(OODLE_BLOB).hexdigest(), size=len(OODLE_BLOB)
        )
        (self.repo / "maintenance" / "tools.lock.json").write_text(json.dumps(lock, indent=2) + "\n")
        target = self.repo / "catalog" / "conan-exiles" / "targets" / f"{TARGET}.json"
        record = json.loads(target.read_text())
        record["inputs"]["server"]["depots"]["443032"]["manifest"] = MANIFEST
        target.write_text(json.dumps(record, indent=2) + "\n")
        shutil.copytree(FIXTURE / "client", self.client)
        self.edit_block(
            lambda block: block["pixels"].update(
                files=[f"{PAKS}/global.utoc", f"{PAKS}/pakchunk1000-Windows.utoc", f"{PAKS}/pakchunk1010-Windows.utoc"]
            )
        )
        upstream.add("/oodle.so", OODLE_BLOB)
        monkeypatch.setenv("TAKARO_MAINT_DEPOTDOWNLOADER", str(Path(fake_dd.__file__).resolve()))
        monkeypatch.setenv("FAKE_DD_ROOT", str(FIXTURE / "depots"))
        monkeypatch.setenv("FAKE_DD_LOG", str(self.dd_log))
        monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(Path(fake_decoder.__file__).resolve()))
        monkeypatch.setenv("FAKE_DECODER_ROOT", str(FIXTURE / "decoder"))
        monkeypatch.setenv("FAKE_DECODER_STATE", str(self.state))
        monkeypatch.setenv("FAKE_DECODER_LOG", str(self.decoder_log))

    def edit_block(self, change: Any) -> None:
        path = self.repo / "catalog" / "conan-exiles" / "game.json"
        record = json.loads(path.read_text())
        change(record["icons"])
        path.write_text(json.dumps(record, indent=2) + "\n")

    def downloads(self) -> list[list[str]]:
        return [argv for argv in fake_dd.argv_log(self.dd_log) if "-filelist" in argv]

    def decoder_calls(self, command: str) -> list[list[str]]:
        calls = fake_decoder.argv_log(self.decoder_log)
        return [argv for argv in calls if argv[:1] == [command] or argv[:2] == [command, "inspect"]]


@pytest.fixture
def env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[Env]:
    with FakeUpstream() as upstream:
        yield Env(tmp_path, upstream, monkeypatch)


def build(run: Any, env: Env, out: Path, *extra: str, client: Path | None = None) -> tuple[int, Any, str]:
    return run(
        "icons",
        "build",
        "--game",
        "conan-exiles",
        "--client",
        str(client or env.client),
        "--out",
        str(out),
        *extra,
        repo=env.repo,
    )


def tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def icon(out: Path, code: str) -> Image.Image:
    index = json.loads((out / "pack" / "conan-exiles" / "index.json").read_text())
    return Image.open(out / "pack" / "conan-exiles" / index["icons"][code]).convert("RGBA")


def index_of(out: Path) -> dict[str, str]:
    return json.loads((out / "pack" / "conan-exiles" / "index.json").read_text())["icons"]  # type: ignore[no-any-return]


# ------------------------------------------------------------------------------- catalog and inputs


def test_the_real_catalog_validates(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")
    assert code == 0, payload
    ids = {check["id"] for check in payload["checks"]}
    assert {"icons-unreal-settings", "icons-aes-key-declared", "icons-iostore-files"} <= ids


@pytest.mark.parametrize(
    ("change", "check"),
    [
        (lambda block: block.pop("unreal"), "icons-unreal-settings"),
        (lambda block: block["unreal"].update(aesKeyEnv="NOT_DECLARED"), "icons-aes-key-declared"),
        (lambda block: block["pixels"]["files"].append("ConanSandbox/Content/Paks/x.ucas"), "icons-iostore-files"),
        (lambda block: block["pixels"].update(access="mirror"), "game-schema"),
        (lambda block: block["pixels"].update(decoder="unity"), "game-schema"),
        (lambda block: block["data"].update(decoder="unity"), "game-schema"),
    ],
    ids=["unreal-missing", "key-undeclared", "not-utoc", "unknown-access", "unknown-decoder", "unknown-data-decoder"],
)
def test_a_bad_unreal_block_fails_validation(run: Any, catalog_copy: Path, change: Any, check: str) -> None:
    path = catalog_copy / "catalog" / "conan-exiles" / "game.json"
    record = json.loads(path.read_text())
    change(record["icons"])
    path.write_text(json.dumps(record, indent=2) + "\n")
    code, payload, _ = run("catalog", "validate", repo=catalog_copy)
    assert code == 2
    assert check in {entry["id"] for entry in payload["checks"] if entry["status"] == "fail"}


def test_inputs_shows_both_roles_the_decoder_and_oodle_without_running_anything(run: Any, env: Env) -> None:
    code, payload, _ = run("icons", "inputs", "--game", "conan-exiles", repo=env.repo)
    assert code == 0, payload
    data, pixels = payload["inputs"]
    assert data["role"] == "data" and data["buildid"] == 25792439 and list(data["depots"]) == ["443032"]
    assert pixels["access"] == "iostore-ranges" and pixels["decoder"] == "unreal"
    assert pixels["containers"] == pixels["files"] and pixels["files"][0] == f"{PAKS}/global.utoc"
    assert pixels["expectedClientBuilds"] == [25791535]
    unreal = payload["unreal"]
    assert unreal["engine"] == "GAME_ConanExilesEnhanced" and unreal["aesKeyEnv"] is None
    assert len(unreal["decoder"]["context"]) == 64 and unreal["decoder"]["imageBuilt"] is False
    assert unreal["oodle"]["sha256"] == hashlib.sha256(OODLE_BLOB).hexdigest() and unreal["oodle"]["cached"] is False
    assert env.downloads() == [] and env.decoder_calls("build") == [] and env.decoder_calls("run") == []
    assert not [argv for argv in fake_decoder.argv_log(env.decoder_log) if argv[:1] == ["run"]]


# ----------------------------------------------------------------------------------- the build


def test_the_build_maps_game_data_to_a_pack(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    code, payload, stderr = build(run, env, out)
    assert code == 0, (payload, stderr)
    assert payload["clientBuild"] == {"id": 25791535, "status": "match", "expected": [25791535]}
    coverage = json.loads((out / "coverage.json").read_text())
    icons = index_of(out)
    plain, layered = SELECTION["icon"], SELECTION["layers"]
    for row in [*plain, *layered]:
        assert code_of(row) in icons
    # Internal rows and the non-integer row are in neither the index nor the report.
    listed = set(icons) | {entry["code"] for entry in coverage["unmatchedCodes"]}
    for hidden in (SELECTION["internal-name"], SELECTION["empty-name"], SELECTION["non-integer"]):
        assert not set(codes(hidden)) & listed
    assert "Dev_Probe" not in listed and "NotATemplateId" not in listed
    assert coverage["measuredAgainst"] == "game-data"
    assert "ItemNameToTemplateID" in coverage["codesFrom"] and "#811" in coverage["codesFrom"]
    assert set(coverage["byRule"]) == {"icon", "icon-layers", "none"}
    assert coverage["unmatchedByReason"] == {
        "missing-texture": 1,
        "no-icon-in-game": 1,
        "unsupported-pixel-format": 1,
    }
    details = {entry["code"]: entry["reason"] for entry in coverage["unmatchedCodes"]}
    assert "no-icon-in-game" in details.values()
    assert coverage["total"] == coverage["matched"] + coverage["unmatched"]
    assert list((paths.cache_dir() / "icons" / "work").iterdir()) == []


def test_first_name_wins_and_ids_without_a_name_get_numeric_codes(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    icons = index_of(out)
    first_id = SELECTION["icon"][0]
    first_name = code_of(first_id)
    assert first_name != "Duplicate_Name_For_First_Icon" and first_name in icons
    assert "Duplicate_Name_For_First_Icon" not in icons and first_id not in icons
    assert "6364" in icons  # no name table names it: the template id is the code


def test_dlc_items_carry_a_primary_and_a_candidate_for_one_object(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    icons = index_of(out)
    coverage = json.loads((out / "coverage.json").read_text())
    dlc_a, dlc_b, dlc_c = SELECTION["dlc"]
    candidates = {row["code"]: row["candidateOf"] for row in coverage["candidates"]["codes"]}
    name_a = next(name for name, row in DLC_NAMES.items() if next(iter(row.values())) == int(dlc_a))
    assert candidates[name_a] == dlc_a and icons[name_a] == icons[dlc_a]
    # dlc_b is named by the base name table: the name is the primary and the id the candidate.
    assert candidates[dlc_b] == "Base_Table_Name_For_Dlc_Item" and icons[dlc_b] == icons["Base_Table_Name_For_Dlc_Item"]
    assert dlc_c in icons and dlc_c not in candidates.values() and dlc_c not in candidates
    assert coverage["candidates"]["count"] == len(candidates)
    # Candidates are not counted anywhere else.
    assert coverage["total"] == coverage["matched"] + coverage["unmatched"]
    assert set(candidates) <= set(icons) and not set(candidates) & {e["code"] for e in coverage["unmatchedCodes"]}
    assert coverage["matched"] == len(icons) - len(candidates)


def test_pixels_follow_the_icon_and_icon_layers_rules(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    for row in SELECTION["icon"]:
        image = icon(out, code_of(row))
        assert image.getpixel((10, 10)) == maker.color_for(package_of(row))
    for row in SELECTION["layers"]:
        image = icon(out, code_of(row))
        assert image.size == (64, 64)
        assert image.getpixel((2, 2)) == BORDER  # the border is the top layer
        assert image.getpixel((32, 32)) == maker.color_for(maker.soft_packages(ROWS[row])[0])
        assert image.getpixel((6, 32))[3] == 0  # the base art's transparent margin shows through the ring's gap
    assert icon(out, code_of(SELECTION["battlepass"][0])).size == (128, 128)
    assert icon(out, code_of(SELECTION["small"][0])).size == (127, 128)


def test_a_mask_texture_is_flagged_and_failed_textures_are_unmatched(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    coverage = json.loads((out / "coverage.json").read_text())
    mask = code_of(SELECTION["mask"][0])
    mask_package = package_of(SELECTION["mask"][0])
    assert coverage["flagsByName"] == {"mask-texture": 1}
    assert coverage["flaggedCodes"] == [{"code": mask, "flag": "mask-texture", "detail": f"{mask_package} PF_BC4"}]
    reasons = {entry["code"]: entry for entry in coverage["unmatchedCodes"]}
    unsupported, missing = code_of(SELECTION["unsupported"][0]), code_of(SELECTION["missing"][0])
    assert reasons[unsupported]["reason"] == "unsupported-pixel-format"
    assert "PF_ASTC_4x4" in reasons[unsupported]["detail"]
    assert reasons[missing]["reason"] == "missing-texture"
    assert reasons[missing]["detail"] == package_of(SELECTION["missing"][0])
    assert reasons[code_of(SELECTION["none"][0])]["reason"] == "no-icon-in-game"


def test_the_build_record_names_what_was_built_from(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    code, payload, _ = build(run, env, out)
    assert code == 0, payload
    record = json.loads((out / "build-record.json").read_text())
    image = json.loads(env.state.joinpath("images.json").read_text())
    stored = next(value for key, value in image.items() if key != "_builds")
    assert record["decoder"]["imageId"] == stored["Id"]
    assert record["decoder"]["contextSha256"] == stored["label"] and len(stored["label"]) == 64
    assert record["decoder"]["oodleSha256"] == hashlib.sha256(OODLE_BLOB).hexdigest()
    assert record["decoder"]["engine"] == "GAME_ConanExilesEnhanced"
    assert record["decoder"]["tool"]["name"] == "fake-ue-export"
    assert record["client"] == {
        "app": 440900,
        "depot": 440901,
        "buildid": 25791535,
        "manifest": "2054081959462269381",
        "status": "match",
        "expected": [25791535],
    }
    assert record["data"]["buildid"] == 25792439 and record["data"]["depots"] == {"443032": MANIFEST}
    pixels = record["pixels"]
    assert pixels["access"] == "iostore-ranges"
    assert list(pixels["files"]) == [
        f"{PAKS}/global.utoc",
        f"{PAKS}/pakchunk1000-Windows.utoc",
        f"{PAKS}/pakchunk1010-Windows.utoc",
    ]
    for name, entry in pixels["files"].items():
        raw = (env.client / name).read_bytes()
        assert (
            entry["sha256"] == hashlib.sha256(raw).hexdigest() and entry["size"] == len(raw) and entry["version"] == 8
        )
    assert pixels["files"][f"{PAKS}/pakchunk1010-Windows.utoc"]["partitions"] == 2
    ranges = json.loads((out / "client-ranges.json").read_text())
    assert pixels["ranges"]["ranges"] == len(ranges) and pixels["ranges"]["bytes"] == sum(r["length"] for r in ranges)
    assert len(pixels["ranges"]["planSha256"]) == 64
    tables = record["mapper"]["tables"]
    assert {table["package"] for table in tables if table.get("role") != "name"} >= {
        "/Game/Items/ItemTable",
        "/Game/DLC/Special/Special_ItemTable_Composite",
    }
    composite = next(table for table in tables if table["class"] == "CompositeDataTable")
    assert composite["parents"] == ["/Game/DLC/Special/Special_ItemTable"]
    assert "timestamp" not in json.dumps(record).lower()


def test_the_sparse_mirror_holds_exactly_the_planned_bytes(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    ranges = json.loads((out / "client-ranges.json").read_text())
    assert ranges == sorted(ranges, key=lambda r: (r["file"], r["offset"]))
    [mirror] = (paths.cache_dir() / "icons" / "clients" / "conan-exiles" / "440900" / "25791535").iterdir()
    for name in {r["file"] for r in ranges}:
        source = (env.client / name).read_bytes()
        mirrored = (mirror / name).read_bytes()
        assert len(mirrored) == len(source)
        planned = bytearray(len(source))
        for entry in (r for r in ranges if r["file"] == name):
            start, end = entry["offset"], entry["offset"] + entry["length"]
            assert mirrored[start:end] == source[start:end]
            assert hashlib.sha256(source[start:end]).hexdigest() == entry["sha256"]
            planned[start:end] = b"\x01" * (end - start)
        assert all(byte == 0 for byte, mark in zip(mirrored, planned, strict=True) if not mark)
    unread = (mirror / f"{PAKS}/pakchunk1000-Windows.ucas").read_bytes()
    filler = maker.pattern("filler", 17 * 4096 - 100)[:64]
    assert filler not in unread  # the filler package was never needed, so it was never read


def test_two_builds_are_byte_identical_and_the_second_hits_every_cache(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "one")[0] == 0
    runs_before = len(env.decoder_calls("run"))
    assert build(run, env, tmp_path / "two")[0] == 0
    assert tree(tmp_path / "one") == tree(tmp_path / "two")
    assert len(env.downloads()) == 1
    assert len(env.decoder_calls("run")) == runs_before  # decoded outputs came from the decode cache


def test_a_tampered_mirror_range_is_read_again_and_changed_toc_bytes_plan_afresh(
    run: Any, env: Env, tmp_path: Path
) -> None:
    assert build(run, env, tmp_path / "one")[0] == 0
    ranges = json.loads((tmp_path / "one" / "client-ranges.json").read_text())
    [mirror] = (paths.cache_dir() / "icons" / "clients" / "conan-exiles" / "440900" / "25791535").iterdir()
    target = mirror / ranges[0]["file"]
    data = bytearray(target.read_bytes())
    data[ranges[0]["offset"]] ^= 0xFF
    target.write_bytes(bytes(data))
    code, _, stderr = build(run, env, tmp_path / "two")
    assert code == 0 and "corrupt client mirror" in stderr
    assert tree(tmp_path / "one") == tree(tmp_path / "two")
    # A client whose table of contents changed is a different plan and gets a directory of its own.
    toc = env.client / PAKS / "pakchunk1000-Windows.utoc"
    toc.write_bytes(toc.read_bytes() + b"\0")
    assert build(run, env, tmp_path / "three")[0] == 0
    assert len(list(mirror.parent.iterdir())) == 2


def test_a_client_of_another_build_warns_and_still_builds(run: Any, env: Env, tmp_path: Path) -> None:
    manifest = env.client / "appmanifest_440900.acf"
    manifest.write_text(manifest.read_text().replace("25791535", "25791600"))
    code, payload, stderr = build(run, env, tmp_path / "out")
    assert code == 0
    assert payload["clientBuild"]["status"] == "newer" and "warning:" in stderr


# ---------------------------------------------------------------------------------------- hardening


def test_the_decoder_runs_hardened_with_read_only_game_mounts(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "out")[0] == 0
    runs = [argv for argv in fake_decoder.argv_log(env.decoder_log) if argv[:1] == ["run"]]
    assert runs
    for argv in runs:
        assert ["--network", "none"] == argv[argv.index("--network") : argv.index("--network") + 2]
        assert "--read-only" in argv and "no-new-privileges" in argv and "--cap-drop" in argv
        mounts = [argv[i + 1] for i, item in enumerate(argv) if item == "-v"]
        assert [m for m in mounts if not m.endswith(":ro")] == [m for m in mounts if m.endswith(":/out")]
        assert any(m.endswith(":/game:ro") for m in mounts) and any(
            m.endswith("liboo2corelinux64.so.9:ro") for m in mounts
        )


def test_an_aes_key_is_passed_by_name_only(run: Any, env: Env, tmp_path: Path) -> None:
    secret = "k3y-value-4f1d9c27"
    env.edit_block(lambda block: (block.update(secrets=["FAKE_AES"]), block["unreal"].update(aesKeyEnv="FAKE_AES")))
    env.monkeypatch.setenv("FAKE_AES", secret)
    code, payload, stderr = build(run, env, tmp_path / "out")
    assert code == 0, payload
    runs = [argv for argv in fake_decoder.argv_log(env.decoder_log) if argv[:1] == ["run"]]
    assert all("FAKE_AES" in argv and "-e" in argv for argv in runs)
    assert secret not in json.dumps(fake_decoder.argv_log(env.decoder_log)) + json.dumps(payload) + stderr
    assert secret not in "".join(path.read_text() for path in (tmp_path / "out").rglob("*.json"))


def test_a_missing_key_variable_is_refused_before_any_download(run: Any, env: Env, tmp_path: Path) -> None:
    env.edit_block(lambda block: (block.update(secrets=["FAKE_AES"]), block["unreal"].update(aesKeyEnv="FAKE_AES")))
    env.monkeypatch.delenv("FAKE_AES", raising=False)
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 2 and "FAKE_AES" in payload["error"]
    assert fake_dd.argv_log(env.dd_log) == [] and env.upstream.requested == []


def test_a_missing_docker_is_refused_before_any_download(run: Any, env: Env, tmp_path: Path) -> None:
    env.monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(tmp_path / "no-such-docker"))
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 2 and "docker is required" in payload["error"]
    assert fake_dd.argv_log(env.dd_log) == [] and env.upstream.requested == []


# --------------------------------------------------------------------------------- refusals and failures


def test_a_missing_container_file_is_refused_before_anything_runs(run: Any, env: Env, tmp_path: Path) -> None:
    (env.client / PAKS / "pakchunk1010-Windows_s1.ucas").unlink()
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 4 and payload["missing"] == [f"{PAKS}/pakchunk1010-Windows_s1.ucas"]
    (env.client / PAKS / "pakchunk1000-Windows.utoc").unlink()
    assert build(run, env, tmp_path / "out")[0] == 4
    assert fake_dd.argv_log(env.dd_log) == [] and fake_decoder.argv_log(env.decoder_log) == []


def test_decoder_failures_fail_the_build_and_write_no_pack(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    out = tmp_path / "exit"
    monkeypatch.setenv("FAKE_DECODER_EXIT", "1")
    assert build(run, env, out)[0] == 6 and not (out / "pack").exists()
    monkeypatch.delenv("FAKE_DECODER_EXIT")

    monkeypatch.setenv("FAKE_DECODER_FAIL", "tx00003")
    code, payload, _ = build(run, env, tmp_path / "fail")
    assert code == 6 and any("tx00003" in line for line in payload["failures"])
    monkeypatch.delenv("FAKE_DECODER_FAIL")

    monkeypatch.setenv("FAKE_DECODER_DROP", "tx00002")
    assert build(run, env, tmp_path / "drop")[0] == 6
    monkeypatch.delenv("FAKE_DECODER_DROP")

    monkeypatch.setenv("FAKE_DECODER_ESCAPE", "tx00001")
    assert build(run, env, tmp_path / "escape")[0] == 6
    monkeypatch.delenv("FAKE_DECODER_ESCAPE")
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


def _rewrite_toc(client: Path, name: str, change: Any) -> None:
    path = client / PAKS / name
    data = bytearray(path.read_bytes())
    change(data)
    path.write_bytes(bytes(data))


@pytest.mark.parametrize(
    ("change", "code", "needle"),
    [
        (lambda data: data.__setitem__(slice(16, 17), bytes([42])), 6, "version 42"),
        (lambda data: data.__setitem__(80, data[80] | 2), 2, "#747"),
        (lambda data: data.__setitem__(80, data[80] & ~8), 6, "directory index"),
    ],
    ids=["version", "encrypted", "not-indexed"],
)
def test_toc_problems_are_refused_before_any_download(
    run: Any, env: Env, tmp_path: Path, change: Any, code: int, needle: str
) -> None:
    _rewrite_toc(env.client, "pakchunk1000-Windows.utoc", change)
    result, payload, _ = build(run, env, tmp_path / "out")
    assert result == code and needle in payload["error"]
    assert fake_dd.argv_log(env.dd_log) == []


def test_a_source_shorter_than_its_toc_implies_is_an_integrity_error(run: Any, env: Env, tmp_path: Path) -> None:
    ucas = env.client / PAKS / "pakchunk1010-Windows_s1.ucas"
    ucas.write_bytes(ucas.read_bytes()[:100])
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 5, payload


def test_an_image_with_a_stale_label_is_rebuilt_once_and_run_by_the_new_id(run: Any, env: Env, tmp_path: Path) -> None:
    from takaro_maint.icons.decoders import container

    context = container.context_sha256("unreal")
    tag = container.tag_for("unreal", context)
    env.state.mkdir()
    (env.state / "images.json").write_text(
        json.dumps({tag: {"Id": "sha256:old", "label": "stale", "labelKey": container.CONTEXT_LABEL}})
    )
    # The fake reads repo-root decoders from the copied repository, whose context hash is the same files.
    assert build(run, env, tmp_path / "out")[0] == 0
    builds = [argv for argv in fake_decoder.argv_log(env.decoder_log) if argv[:1] == ["build"]]
    assert len(builds) == 1
    images = json.loads((env.state / "images.json").read_text())
    runs = [argv for argv in fake_decoder.argv_log(env.decoder_log) if argv[:1] == ["run"]]
    assert images[tag]["Id"] != "sha256:old" and all(argv[-2] == images[tag]["Id"] for argv in runs)


def test_wrong_oodle_bytes_are_an_integrity_error(run: Any, env: Env, tmp_path: Path) -> None:
    env.upstream.add("/oodle.so", b"not the pinned library")
    code, _, _ = build(run, env, tmp_path / "out")
    assert code == 5
    assert fake_dd.argv_log(env.dd_log) == []


def test_the_generated_fixture_files_are_what_is_committed(tmp_path: Path) -> None:
    shutil.copytree(FIXTURE / "decoder" / "json", tmp_path / "decoder" / "json")
    (tmp_path / "decoder" / "registry.json").write_bytes((FIXTURE / "decoder" / "registry.json").read_bytes())
    maker.generate(tmp_path)
    for sub in ("client", "depots", "decoder/png"):
        generated = tree(tmp_path / sub)
        committed = tree(FIXTURE / sub)
        assert generated == committed, sub
    assert (tmp_path / "decoder" / "index.json").read_bytes() == (FIXTURE / "decoder" / "index.json").read_bytes()


@pytest.mark.parametrize("plant", ["symlink", "hardlink"])
def test_a_link_planted_in_the_decoder_output_fails_the_build(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, plant: str
) -> None:
    monkeypatch.setenv("FAKE_DECODER_PLANT", plant)
    out = tmp_path / "out"
    code, payload, _ = build(run, env, out)
    assert code == 6 and plant in payload["error"]
    assert not (out / "pack").exists()
    assert not list((paths.cache_dir() / "icons" / "decoded").glob("*/.takaro/complete.json"))
