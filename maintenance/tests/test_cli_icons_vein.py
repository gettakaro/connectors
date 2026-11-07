"""``icons inputs`` and the catalog-driven ``icons build --game vein``, end to end, against fakes.

The server depot is the DepotDownloader stand-in serving a tree ``make_fixture.py`` writes; the Unreal decoder
container is ``fake_icons_decoder.py`` answering from the recorded outputs the same script generates; the client is
a set of synthetic version 11 paks. No real game art is involved. Every assertion is on exit codes, the JSON on
stdout and the files the command wrote.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import struct
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
from takaro_maint.exit_codes import BuildFailed, UsageError
from takaro_maint.icons import pakfile

FIXTURE = FIXTURES / "icons" / "vein"
TARGET = "linux-25969514"
OODLE_BLOB = b"dummy oodle library bytes"
PAKS = "Vein/Content/Paks"


def _load_maker() -> Any:
    spec = importlib.util.spec_from_file_location("vein_make_fixture", FIXTURE / "make_fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


maker = _load_maker()
MATCHED = {"BP_Synth_Plain_C", "BP_Synth_Child_C", "BP_Synth_Soup_C", "BP_Synth_Food_C"}
UNMATCHED = {
    "BP_Synth_NoIcon_C": "no-icon-in-game",
    "BP_Synth_MissingTex_C": "missing-texture",
    "BP_Synth_BadFormat_C": "unsupported-pixel-format",
}
HIDDEN = {"BP_Synth_Abstract_C", "SKEL_BP_Synth_Plain_C", "BP_Synth_Chair_C", "BP_Synth_ClientOnly_C"}


class Env:
    def __init__(self, tmp_path: Path, upstream: FakeUpstream, monkeypatch: pytest.MonkeyPatch) -> None:
        self.tmp_path = tmp_path
        self.upstream = upstream
        self.monkeypatch = monkeypatch
        self.repo = tmp_path / "repo"
        self.client = tmp_path / "client"
        self.depots = tmp_path / "depots"
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
        target = self.repo / "catalog" / "vein" / "targets" / f"{TARGET}.json"
        record = json.loads(target.read_text())
        record["inputs"]["server"]["depots"]["2131402"]["manifest"] = maker.MANIFEST
        target.write_text(json.dumps(record, indent=2) + "\n")
        maker.write_client(self.client)
        maker.write_server(self.depots)
        self.edit_block(lambda block: block["pixels"].update(files=[f"{PAKS}/{name}" for name in maker.CLIENT_PAKS]))
        upstream.add("/oodle.so", OODLE_BLOB)
        monkeypatch.setenv("TAKARO_MAINT_DEPOTDOWNLOADER", str(Path(fake_dd.__file__).resolve()))
        monkeypatch.setenv("FAKE_DD_ROOT", str(self.depots))
        monkeypatch.setenv("FAKE_DD_LOG", str(self.dd_log))
        monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(Path(fake_decoder.__file__).resolve()))
        monkeypatch.setenv("FAKE_DECODER_ROOT", str(FIXTURE / "decoder"))
        monkeypatch.setenv("FAKE_DECODER_STATE", str(self.state))
        monkeypatch.setenv("FAKE_DECODER_LOG", str(self.decoder_log))

    def edit_block(self, change: Any) -> None:
        path = self.repo / "catalog" / "vein" / "game.json"
        record = json.loads(path.read_text())
        change(record["icons"])
        path.write_text(json.dumps(record, indent=2) + "\n")

    def replace_pak(self, name: str, payload: bytes) -> None:
        (self.client / PAKS / name).write_bytes(payload)

    def decoder_runs(self) -> list[list[str]]:
        return [argv for argv in fake_decoder.argv_log(self.decoder_log) if argv[:1] == ["run"]]


@pytest.fixture
def env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[Env]:
    with FakeUpstream() as upstream:
        yield Env(tmp_path, upstream, monkeypatch)


def build(run: Any, env: Env, out: Path, *extra: str) -> tuple[int, Any, str]:
    return run(
        "icons", "build", "--game", "vein", "--client", str(env.client), "--out", str(out), *extra, repo=env.repo
    )


def tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def pack_index(out: Path) -> dict[str, Any]:
    return json.loads((out / "pack" / "vein" / "index.json").read_text())  # type: ignore[no-any-return]


def icon(out: Path, code: str) -> Image.Image:
    return Image.open(out / "pack" / "vein" / pack_index(out)["icons"][code]).convert("RGBA")


# ------------------------------------------------------------------------------- catalog and inputs


def test_the_real_catalog_validates(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")
    assert code == 0, payload
    assert {"icons-pak-files", "icons-iostore-files", "icons-unreal-settings"} <= {c["id"] for c in payload["checks"]}


@pytest.mark.parametrize(
    ("change", "check"),
    [
        (lambda block: block["pixels"]["files"].append("Vein/Content/Paks/x.utoc"), "icons-pak-files"),
        (lambda block: block["pixels"]["files"].append(block["pixels"]["files"][0]), "icons-pak-files"),
        (lambda block: block["pixels"]["files"].append("regex:^Vein/.*\\.pak$"), "icons-client-regex"),
        (lambda block: block["unreal"].update(aesKeyEnv="NOT_DECLARED"), "icons-aes-key-declared"),
    ],
    ids=["not-pak", "listed-twice", "regex", "key-undeclared"],
)
def test_a_bad_pak_block_fails_validation(run: Any, catalog_copy: Path, change: Any, check: str) -> None:
    path = catalog_copy / "catalog" / "vein" / "game.json"
    record = json.loads(path.read_text())
    change(record["icons"])
    path.write_text(json.dumps(record, indent=2) + "\n")
    code, payload, _ = run("catalog", "validate", repo=catalog_copy)
    assert code == 2
    assert check in {entry["id"] for entry in payload["checks"] if entry["status"] == "fail"}


def test_inputs_names_the_pak_route_without_running_anything(run: Any, env: Env) -> None:
    code, payload, _ = run("icons", "inputs", "--game", "vein", repo=env.repo)
    assert code == 0, payload
    data, pixels = payload["inputs"]
    assert data["buildid"] == 25969514 and list(data["depots"]) == ["2131402"]
    assert pixels["access"] == "pak-ranges" and pixels["containers"] == pixels["files"]
    assert pixels["expectedClientBuilds"] == [25970224] and payload["unreal"]["aesKeyEnv"] is None
    assert not fake_dd.argv_log(env.dd_log) and env.decoder_runs() == []


# ----------------------------------------------------------------------------------- the build


def test_the_build_maps_item_blueprints_to_thumbnails(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    code, payload, stderr = build(run, env, out)
    assert code == 0, (payload, stderr)
    assert payload["clientBuild"] == {"id": 25970224, "status": "match", "expected": [25970224]}
    coverage = json.loads((out / "coverage.json").read_text())
    icons = pack_index(out)["icons"]
    assert set(icons) == MATCHED
    listed = set(icons) | {entry["code"] for entry in coverage["unmatchedCodes"]}
    assert not HIDDEN & listed
    assert {entry["code"]: entry["reason"] for entry in coverage["unmatchedCodes"]} == UNMATCHED
    assert coverage["total"] == len(MATCHED) + len(UNMATCHED) and coverage["matched"] == len(MATCHED)
    assert coverage["measuredAgainst"] == "game-data" and "#811" in coverage["codesFrom"]
    assert "ItemType" in coverage["codesFrom"] and "native-only" in coverage["codesFrom"]
    assert list((paths.cache_dir() / "icons" / "work").iterdir()) == []


def test_a_child_blueprint_inherits_its_parents_thumbnail_and_pixels_are_the_decoded_ones(
    run: Any, env: Env, tmp_path: Path
) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    assert icon(out, "BP_Synth_Child_C").tobytes() == icon(out, "BP_Synth_Plain_C").tobytes()
    assert icon(out, "BP_Synth_Plain_C").getpixel((8, 8)) == maker.BLUEPRINTS["BP_Synth_Plain"]["color"]
    assert icon(out, "BP_Synth_Food_C").getpixel((8, 8)) == maker.BLUEPRINTS["BP_Synth_Food"]["color"]


def test_the_build_record_names_the_paks_and_their_encryption_state(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    record = json.loads((out / "build-record.json").read_text())
    pixels = record["pixels"]
    assert pixels["access"] == "pak-ranges" and pixels["decoder"] == "unreal"
    assert set(pixels["files"]) == {f"{PAKS}/{name}" for name in maker.CLIENT_PAKS}
    for name, info in pixels["files"].items():
        data = (env.client / name).read_bytes()
        assert info["size"] == len(data) and info["version"] == 11 and info["encryptedEntries"] == 0
        assert info["entries"] == len(pakfile.read_index(env.client / name, name).files)
        assert len(info["sha256"]) == 64
    assert record["decoder"]["oodleSha256"] == hashlib.sha256(OODLE_BLOB).hexdigest()
    assert record["client"]["buildid"] == 25970224 and record["data"]["buildid"] == 25969514


def test_the_sparse_mirror_holds_exactly_the_planned_bytes(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    ranges = json.loads((out / "client-ranges.json").read_text())
    assert ranges == sorted(ranges, key=lambda r: (r["file"], r["offset"]))
    [mirror] = (paths.cache_dir() / "icons" / "clients" / "vein" / "1857950" / "25970224").iterdir()
    for name in {r["file"] for r in ranges}:
        source = (env.client / name).read_bytes()
        mirrored = (mirror / name).read_bytes()
        assert len(mirrored) == len(source)
        planned = bytearray(len(source))
        for entry in (r for r in ranges if r["file"] == name):
            start, end = entry["offset"], entry["offset"] + entry["length"]
            assert mirrored[start:end] == source[start:end]
            planned[start:end] = b"\x01" * (end - start)
        assert all(byte == 0 for byte, mark in zip(mirrored, planned, strict=True) if not mark)
    unused = maker.entry_payload("BP_Synth_Unused", "package")
    assert all(unused not in (mirror / f"{PAKS}/{name}").read_bytes() for name in maker.CLIENT_PAKS)


def test_a_thumbnail_held_by_two_paks_is_mirrored_from_both(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    files = {r["file"] for r in json.loads((out / "client-ranges.json").read_text())}
    assert {f"{PAKS}/pakchunk1-Windows.pak", f"{PAKS}/pakchunk104-Windows.pak"} <= files


def test_two_builds_are_byte_identical(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "one")[0] == 0
    assert build(run, env, tmp_path / "two")[0] == 0
    assert tree(tmp_path / "one") == tree(tmp_path / "two")


def test_no_key_variable_value_reaches_any_output(run: Any, env: Env, tmp_path: Path) -> None:
    secret = "k3y-value-9a1c77d2"
    env.monkeypatch.setenv("TAKARO_VEIN_AES_KEY", secret)
    code, payload, stderr = build(run, env, tmp_path / "out")
    assert code == 0, payload
    assert secret not in json.dumps(payload) + stderr + json.dumps(fake_decoder.argv_log(env.decoder_log))
    assert secret not in "".join(path.read_text() for path in (tmp_path / "out").rglob("*.json"))
    assert all("TAKARO_VEIN_AES_KEY" not in argv for argv in env.decoder_runs())


def test_the_decoder_mounts_the_mirror_read_only(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "out")[0] == 0
    runs = env.decoder_runs()
    assert runs
    for argv in runs:
        mounts = [argv[i + 1] for i, item in enumerate(argv) if item == "-v"]
        assert [m for m in mounts if not m.endswith(":ro")] == [m for m in mounts if m.endswith(":/out")]


# --------------------------------------------------------------------------------- pak refusals


def _footer_patched(payload: bytes, **fields: Any) -> bytes:
    """``payload`` with its footer's encrypted flag, key GUID or version replaced."""
    head, footer = payload[: -maker.FOOTER_SIZE], bytearray(payload[-maker.FOOTER_SIZE :])
    if "guid" in fields:
        footer[0:16] = fields["guid"]
    if "encrypted" in fields:
        footer[16] = fields["encrypted"]
    if "version" in fields:
        footer[21:25] = struct.pack("<I", fields["version"])
    return head + bytes(footer)


@pytest.mark.parametrize(
    ("fields", "needle"),
    [
        ({"encrypted": 1}, "encrypted pak"),
        ({"guid": bytes(range(1, 17))}, "encrypted pak"),
        ({"version": 7}, "version 7 is not supported"),
        ({"version": 12}, "version 12 is not supported"),
    ],
    ids=["encrypted-index", "key-guid", "old-version", "new-version"],
)
def test_an_encrypted_or_unknown_pak_is_refused_before_any_download(
    run: Any, env: Env, tmp_path: Path, fields: dict[str, Any], needle: str
) -> None:
    name = "pakchunk1-Windows.pak"
    env.replace_pak(name, _footer_patched((env.client / PAKS / name).read_bytes(), **fields))
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 2 and needle in payload["error"] and name in payload["error"]
    assert fake_dd.argv_log(env.dd_log) == [] and fake_decoder.argv_log(env.decoder_log) == []
    assert env.upstream.requested == []


def test_an_encrypted_entry_that_the_build_needs_is_refused(run: Any, env: Env, tmp_path: Path) -> None:
    name = "pakchunk0-Windows.pak"
    files = maker.pak_files(maker.CLIENT_PAKS[name])
    stem = f"{maker.thumb_stem('BP_Synth_Food')}.uexp"
    env.replace_pak(name, maker.build_pak(files, encrypted_entries=frozenset({stem})))
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 2 and "encrypted" in payload["error"] and stem in payload["error"]


def test_an_encrypted_entry_that_nothing_needs_is_only_counted(run: Any, env: Env, tmp_path: Path) -> None:
    name = "pakchunk0-Windows.pak"
    files = maker.pak_files(maker.CLIENT_PAKS[name])
    unrelated = f"{maker.ITEMS}/BP_Synth_Elsewhere.uasset"
    env.replace_pak(name, maker.build_pak(files, encrypted_entries=frozenset({unrelated})))
    out = tmp_path / "out"
    assert build(run, env, out)[0] == 0
    record = json.loads((out / "build-record.json").read_text())
    assert record["pixels"]["files"][f"{PAKS}/{name}"]["encryptedEntries"] == 1


def test_a_missing_pak_is_refused_before_any_download(run: Any, env: Env, tmp_path: Path) -> None:
    (env.client / PAKS / "pakchunk104-Windows.pak").unlink()
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 4 and payload["missing"] == [f"{PAKS}/pakchunk104-Windows.pak"]
    assert fake_dd.argv_log(env.dd_log) == []


def test_a_truncated_pak_is_a_build_failure(run: Any, env: Env, tmp_path: Path) -> None:
    name = "pakchunk1-Windows.pak"
    env.replace_pak(name, (env.client / PAKS / name).read_bytes()[:100])
    code, payload, _ = build(run, env, tmp_path / "out")
    assert code == 6, payload
    assert fake_dd.argv_log(env.dd_log) == []


# ------------------------------------------------------------------------------- the planner itself


def test_the_planner_reads_entries_the_way_the_paks_were_written(tmp_path: Path) -> None:
    maker.write_client(tmp_path)
    name = f"{PAKS}/pakchunk1-Windows.pak"
    index = pakfile.read_index(tmp_path / name, name)
    assert index.version == 11 and index.mount_point == maker.MOUNT and index.encrypted_entries == 0
    soup = index.files[f"{maker.thumb_stem('BP_Synth_Soup')}.uexp".lower()]
    assert soup.method == 1 and soup.blocks == 1 and soup.size == soup.uncompressed_size
    plain = index.files[f"{maker.thumb_stem('BP_Synth_Plain')}.uasset".lower()]
    assert plain.method == 0 and plain.span == 53 + plain.size
    data = (tmp_path / name).read_bytes()
    assert data[plain.offset + 53 : plain.offset + plain.span] == maker.entry_payload("BP_Synth_Plain", "package")
    assert data[soup.offset + 53 + 20 : soup.offset + soup.span] == maker.entry_payload("BP_Synth_Soup", "exports")


def test_the_planner_reports_missing_stems_and_always_plans_the_metadata(tmp_path: Path) -> None:
    maker.write_client(tmp_path)
    name = f"{PAKS}/pakchunk0-Windows.pak"
    index = pakfile.read_index(tmp_path / name, name)
    ranges, missing = pakfile.plan([index], [maker.thumb_stem("BP_Synth_Food"), f"{maker.ITEMS}/Nope"])
    assert missing == [f"{maker.ITEMS}/Nope"]
    size = len((tmp_path / name).read_bytes())
    assert any(r.offset + r.length == size for r in ranges)  # the footer is the last planned range
    assert sum(r.length for r in ranges) < size


def test_the_planner_refuses_garbage_and_an_unreadable_directory(tmp_path: Path) -> None:
    junk = tmp_path / "junk.pak"
    junk.write_bytes(b"x" * 1000)
    with pytest.raises(BuildFailed, match="no pak footer"):
        pakfile.read_index(junk, "junk.pak")
    tiny = tmp_path / "tiny.pak"
    tiny.write_bytes(b"x")
    with pytest.raises(BuildFailed, match="too small"):
        pakfile.read_index(tiny, "tiny.pak")
    encrypted = tmp_path / "enc.pak"
    encrypted.write_bytes(maker.build_pak({"a/b.uasset": b"x"}, encrypted_index=True))
    with pytest.raises(UsageError, match="encrypted pak"):
        pakfile.read_index(encrypted, "enc.pak")


# ------------------------------------------------------------------------------------ the fixture files


def test_the_committed_fixture_files_equal_the_generators_output() -> None:
    regenerated = maker.all_files()
    committed = {
        str(path.relative_to(FIXTURE)): path.read_bytes()
        for path in sorted(FIXTURE.rglob("*"))
        if path.is_file() and path.relative_to(FIXTURE).parts[0] == "decoder" and "__pycache__" not in path.parts
    }
    assert committed == regenerated


def test_no_game_file_extension_is_committed_under_the_fixture() -> None:
    names = [path.name for path in FIXTURE.rglob("*") if path.is_file() and "__pycache__" not in path.parts]
    assert not [name for name in names if name.endswith((".pak", ".utoc", ".ucas", ".uasset", ".uexp", ".ubulk"))]
