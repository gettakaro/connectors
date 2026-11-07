"""``icons inputs`` and the catalog-driven ``icons build --game minecraft``, end to end, against fakes.

The three public downloads (the version manifest, ``server.jar`` and ``client.jar``) are served by ``FakeUpstream`` from
the synthetic files ``make_fixture.py`` generates, and the data-generator container is ``fake_icons_decoder.py``
answering from a recorded registry. No game file, game art or game data is involved. Every assertion is on exit codes,
the JSON on stdout and the files the command wrote.
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

import fake_icons_decoder as fake_decoder
from conftest import FIXTURES, REPO_ROOT, point_at
from fake_upstream import FakeUpstream
from takaro_maint import paths
from takaro_maint.games.minecraft import icons as minecraft_icons

FIXTURE = FIXTURES / "icons" / "minecraft"
TARGET = "fabric-26.3"


def _load_maker() -> Any:
    spec = importlib.util.spec_from_file_location("minecraft_make_fixture", FIXTURE / "make_fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


maker = _load_maker()
PUBLIC = FIXTURE / "public"
CLIENT, SERVER, VERSION = (PUBLIC / name for name in ("client.jar", "server.jar", "version.json"))
CLIENT_SHA1, SERVER_SHA1, MANIFEST_SHA1 = (
    hashlib.sha1(path.read_bytes()).hexdigest() for path in (CLIENT, SERVER, VERSION)
)
MANIFEST_PATH = f"/v1/packages/{MANIFEST_SHA1}/26.3.json"
SERVER_PATH = f"/v1/objects/{SERVER_SHA1}/server.jar"
CLIENT_PATH = f"/v1/objects/{CLIENT_SHA1}/client.jar"

CODES = {f"minecraft:{name}" for name in maker.ITEMS}
UNMATCHED = {
    "fixture_special": "entity-rendered",
    "fixture_empty": "no-icon-in-game",
    "fixture_missing_texture": "missing-texture",
    "fixture_missing_model": "missing-model",
    "fixture_cycle": "model-cycle",
    "fixture_tint_unknown": "unsupported-tint",
    "fixture_bad_key": "unsupported-model",
    "fixture_wide_anim": "unsupported-animation",
    "fixture_no_item_file": "missing-item-model",
}
RULES = {
    "flat": 8,
    "flat-tinted": 2,
    "rendered": 9,
    "rendered-tinted": 2,
}


class Env:
    def __init__(self, tmp_path: Path, upstream: FakeUpstream, monkeypatch: pytest.MonkeyPatch) -> None:
        self.tmp_path = tmp_path
        self.upstream = upstream
        self.monkeypatch = monkeypatch
        self.repo = tmp_path / "repo"
        self.decoder = tmp_path / "decoder"
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
        point_at(self.repo, upstream.base_url)
        self.pin(manifest=VERSION.read_bytes(), server=SERVER.read_bytes(), client=CLIENT.read_bytes())
        shutil.copytree(FIXTURE / "decoder", self.decoder)
        monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(Path(fake_decoder.__file__).resolve()))
        monkeypatch.setenv("FAKE_DECODER_ROOT", str(self.decoder))
        monkeypatch.setenv("FAKE_DECODER_STATE", str(self.state))
        monkeypatch.setenv("FAKE_DECODER_LOG", str(self.decoder_log))

    def pin(self, *, manifest: bytes, server: bytes, client: bytes, serve_client: bytes | None = None) -> None:
        """Pin the three downloads the target and the icons block name and serve ``serve_client`` for the client jar."""
        manifest_sha1, server_sha1, client_sha1 = (
            hashlib.sha1(blob).hexdigest() for blob in (manifest, server, client)
        )
        target_path = self.repo / "catalog" / "minecraft" / "targets" / f"{TARGET}.json"
        record = json.loads(target_path.read_text())
        game = record["inputs"]["game"]
        game["manifest"] = {"path": f"/v1/packages/{manifest_sha1}/26.3.json", "sha1": manifest_sha1}
        game["server"].update(
            path=f"/v1/objects/{server_sha1}/server.jar", sha1=server_sha1, size=len(server), installPath="server.jar"
        )
        target_path.write_text(json.dumps(record, indent=2) + "\n")
        self.edit_block(
            lambda block: block["pixels"]["downloads"].update(
                {
                    "26.3": {
                        "source": "mojang-data",
                        "path": f"/v1/objects/{client_sha1}/client.jar",
                        "sha1": client_sha1,
                        "size": len(client),
                    }
                }
            )
        )
        self.upstream.files.clear()
        self.upstream.add(f"/v1/packages/{manifest_sha1}/26.3.json", manifest)
        self.upstream.add(f"/v1/objects/{server_sha1}/server.jar", server)
        self.upstream.add(f"/v1/objects/{client_sha1}/client.jar", serve_client if serve_client is not None else client)

    def edit_block(self, change: Any) -> None:
        path = self.repo / "catalog" / "minecraft" / "game.json"
        record = json.loads(path.read_text())
        change(record["icons"])
        path.write_text(json.dumps(record, indent=2) + "\n")

    def decoder_runs(self) -> list[list[str]]:
        return [argv for argv in fake_decoder.argv_log(self.decoder_log) if argv[:1] == ["run"]]

    def edit_decoder_json(self, change: Any) -> None:
        path = self.decoder / "json" / "registries.json"
        document = json.loads(path.read_text())
        change(document)
        path.write_text(json.dumps(document, indent=2) + "\n")

    def downloads_dir(self, sha1: str) -> Path:
        return paths.cache_dir() / "icons" / "downloads" / "sha1" / sha1


@pytest.fixture
def env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[Env]:
    with FakeUpstream() as upstream:
        yield Env(tmp_path, upstream, monkeypatch)


def build(run: Any, env: Env, out: Path, *extra: str, target: str | None = TARGET) -> tuple[int, Any, str]:
    target_args = ["--target", target] if target else []
    return run("icons", "build", "--game", "minecraft", *target_args, "--out", str(out), *extra, repo=env.repo)


def tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


def index_of(out: Path) -> dict[str, str]:
    return json.loads((out / "pack" / "minecraft" / "index.json").read_text())["icons"]  # type: ignore[no-any-return]


def icon(out: Path, name: str) -> Image.Image:
    return Image.open(out / "pack" / "minecraft" / index_of(out)[f"minecraft:{name}"]).convert("RGBA")


def shaded(colour: tuple[int, int, int], factor: float) -> tuple[int, int, int]:
    return tuple(int(channel * factor + 0.5) for channel in colour)  # type: ignore[return-value]


def multiplied(colour: tuple[int, int, int], argb: int) -> tuple[int, int, int]:
    tint = (argb >> 16 & 255, argb >> 8 & 255, argb & 255)
    return tuple(channel * factor // 255 for channel, factor in zip(colour, tint, strict=True))  # type: ignore[return-value]


@pytest.fixture
def built(run: Any, env: Env, tmp_path: Path) -> Path:
    out = tmp_path / "out"
    code, payload, stderr = build(run, env, out)
    assert code == 0, (payload, stderr)
    return out


def test_the_real_catalog_validates_the_minecraft_block(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")
    assert code == 0, payload
    checks = [
        c for c in payload["checks"] if c["id"].startswith("icons-") and c["file"].endswith("minecraft/game.json")
    ]
    assert {c["id"] for c in checks} >= {
        "icons-data-input",
        "icons-public-download-roles",
        "icons-public-download-revisions",
        "icons-public-download-pins",
    }
    assert all(c["status"] == "pass" for c in checks)


@pytest.mark.parametrize(
    ("change", "check"),
    [
        (lambda block: block["pixels"].update(source="client"), "game-schema"),
        (lambda block: block["pixels"]["downloads"]["26.3"].update(sha1="0" * 40), "icons-public-download-pins"),
        (
            lambda block: block["pixels"]["downloads"].update({"99.9": block["pixels"]["downloads"]["26.3"]}),
            "icons-public-download-revisions",
        ),
        (lambda block: block["pixels"]["downloads"]["26.3"].update(source="nowhere"), "icons-public-download-pins"),
        (lambda block: block["data"].update(input="loader"), "icons-data-input"),
        (
            lambda block: block["pixels"].update(downloads={"99.9": block["pixels"]["downloads"]["26.3"]}),
            "icons-data-input",
        ),
        (
            lambda block: block["data"].update(source="connector", files=["games/minecraft/mod/README.md"]),
            "icons-public-download-roles",
        ),
    ],
)
def test_a_bad_minecraft_block_fails_validation(run: Any, catalog_copy: Path, change: Any, check: str) -> None:
    path = catalog_copy / "catalog" / "minecraft" / "game.json"
    record = json.loads(path.read_text())
    change(record["icons"])
    path.write_text(json.dumps(record, indent=2) + "\n")
    code, payload, _ = run("catalog", "validate", repo=catalog_copy)
    assert code == 2
    assert check in {entry["id"] for entry in payload["checks"] if entry["status"] == "fail"}


def test_inputs_shows_the_data_and_pixels_roles_and_downloads_nothing(run: Any, env: Env) -> None:
    code, payload, _ = run("icons", "inputs", "--game", "minecraft", "--target", TARGET, repo=env.repo)
    assert code == 0, payload
    data, pixels = payload["inputs"]
    assert data["role"] == "data" and data["source"] == "public-download" and data["version"] == "26.3"
    assert data["manifest"] == {"url": f"{env.upstream.base_url}{MANIFEST_PATH}", "sha1": MANIFEST_SHA1}
    assert data["url"] == f"{env.upstream.base_url}{SERVER_PATH}" and data["sha1"] == SERVER_SHA1
    assert data["size"] == SERVER.stat().st_size and data["cached"] is False
    assert data["decoder"] == "minecraft-datagen" and data["reports"] == ["registries.json"]
    assert pixels["role"] == "pixels" and pixels["source"] == "public-download" and pixels["artifact"] == "client"
    assert pixels["url"] == f"{env.upstream.base_url}{CLIENT_PATH}" and pixels["sha1"] == CLIENT_SHA1
    assert pixels["size"] == CLIENT.stat().st_size and pixels["cached"] is False
    assert list(payload["decoders"]) == ["minecraft-datagen"]
    assert payload["decoders"]["minecraft-datagen"]["imageBuilt"] is False
    assert env.upstream.requested == [] and env.decoder_runs() == []


def test_build_covers_the_registry_without_air(run: Any, env: Env, built: Path) -> None:
    assert set(index_of(built)) == CODES - {f"minecraft:{name}" for name in UNMATCHED}
    coverage = json.loads((built / "coverage.json").read_text())
    assert coverage["total"] == len(CODES) == 30
    assert coverage["matched"] == 21 and coverage["unmatched"] == 9
    assert {rule: counts["matched"] for rule, counts in coverage["byRule"].items()} == RULES
    assert coverage["byKind"]["block"]["total"] == len(maker.BLOCKS)
    assert coverage["unmatchedByReason"] == {
        reason: list(UNMATCHED.values()).count(reason) for reason in sorted(set(UNMATCHED.values()))
    }
    unmatched = {entry["code"]: entry["reason"] for entry in coverage["unmatchedCodes"]}
    assert unmatched == {f"minecraft:{name}": reason for name, reason in UNMATCHED.items()}
    assert coverage["flagsByName"] == {"gui-case": 1, "lowest-threshold": 1, "render-fitted": 1}
    flagged = {row["code"]: row["flag"] for row in coverage["flaggedCodes"]}
    assert flagged == {
        "minecraft:fixture_select_gui": "gui-case",
        "minecraft:fixture_range": "lowest-threshold",
        "minecraft:fixture_overflow": "render-fitted",
    }
    assert "minecraft:air" not in index_of(built) and "minecraft:fixture_not_registered" not in index_of(built)
    assert "26.3" in coverage["codesFrom"] and SERVER_SHA1 in coverage["codesFrom"] and "#811" in coverage["codesFrom"]
    assert coverage["measuredAgainst"] == "game-data"


def test_resolution_follows_the_default_case_of_each_definition(built: Path) -> None:
    def centre(name: str) -> tuple[int, int, int, int]:
        return icon(built, name).getpixel((8, 8))  # type: ignore[return-value]

    assert centre("fixture_condition") == (10, 20, 30, 255)  # on_false
    assert centre("fixture_select_gui") == (90, 90, 90, 255)  # the gui case, not the fallback
    assert centre("fixture_select_trim") == (90, 90, 90, 255)  # the fallback
    assert centre("fixture_range") == (10, 20, 30, 255)  # the lowest threshold
    assert centre("fixture_composite") == (200, 210, 220, 255)  # the first model


def test_the_full_cube_has_its_top_and_two_side_faces_shaded(built: Path) -> None:
    cube = icon(built, "fixture_block")
    assert cube.size == (128, 128)
    assert cube.getpixel((64, 29)) == (255, 0, 0, 255)
    assert cube.getpixel((36, 78)) == (*shaded((0, 255, 255), 0.6), 255)  # east on the left
    assert cube.getpixel((92, 78)) == (*shaded((0, 0, 255), 0.8), 255)  # north on the right
    assert cube.getpixel((2, 2))[3] == 0


def test_the_wall_shows_its_post_and_both_arms(built: Path) -> None:
    wall = icon(built, "fixture_wall")
    stone = (150, 150, 150)
    assert wall.getpixel((64, 64)) in {(*shaded(stone, factor), 255) for factor in (0.6, 0.8, 1.0)}
    assert wall.getpixel((36, 84)) == (
        *shaded(stone, 0.8),
        255,
    )  # the arm's north end, drawn because cullface is dropped
    assert wall.getpixel((75, 75)) == (*shaded(stone, 0.6), 255)  # the arm's west side
    assert wall.getbbox() is not None and wall.getbbox()[2] - wall.getbbox()[0] > 60


def test_leather_like_item_keeps_its_untinted_overlay(built: Path) -> None:
    leather = icon(built, "fixture_leather")
    assert leather.size == (16, 16)
    assert leather.getpixel((12, 12)) == (*multiplied((200, 100, 50), maker.LEATHER_DEFAULT), 255)
    assert leather.getpixel((2, 2)) == (10, 200, 10, 255)


def test_potion_like_item_is_tinted_on_its_tinted_layer_only(built: Path) -> None:
    potion = icon(built, "fixture_potion")
    assert potion.getpixel((12, 12)) == (*multiplied((200, 200, 200), maker.POTION_DEFAULT), 255)
    assert potion.getpixel((2, 2)) == (10, 200, 10, 255)


def test_grass_like_block_draws_the_tinted_overlay_over_its_base(built: Path) -> None:
    grass = icon(built, "fixture_grass")
    colormap = (127, 127, 128)  # temperature 0.5, downfall 1.0: x = int(0.5 * 255), y = int((1 - 0.5) * 255)
    tinted = tuple(channel * factor // 255 for channel, factor in zip((200, 200, 200), colormap, strict=True))
    assert grass.getpixel((92, 62)) == (*shaded(tinted, 0.8), 255)  # the overlay's opaque half, on top
    assert grass.getpixel((92, 94)) == (*shaded((120, 80, 40), 0.8), 255)  # the base shows where the overlay is clear


def test_a_model_wider_than_the_canvas_is_fitted_not_clipped(built: Path) -> None:
    box = icon(built, "fixture_overflow").getbbox()
    assert box is not None and box[0] >= 1 and box[1] >= 1 and box[2] <= 127 and box[3] <= 127


def test_leaves_like_block_takes_its_constant_tint(built: Path) -> None:
    leaves = icon(built, "fixture_leaves")
    assert leaves.getpixel((64, 29)) == (*multiplied((220, 220, 220), maker.LEAVES_CONSTANT), 255)


def test_cutout_shows_the_far_face_and_a_translucent_face_is_composited_over(built: Path) -> None:
    glass = icon(built, "fixture_glass")
    assert glass.getpixel((78, 85)) == (*shaded((150, 150, 150), 0.8), 255)  # the hole shows the stone behind
    assert glass.getpixel((106, 71)) == (*shaded((255, 128, 0), 0.8), 255)  # the opaque half
    alpha = 128 / 255
    over = tuple(
        int(front * alpha + behind * (1 - alpha) + 0.5)
        for front, behind in zip(shaded((0, 128, 255), 0.6), shaded((150, 150, 150), 0.6), strict=True)
    )
    assert glass.getpixel((36, 78)) == (*over, 255)


def test_shading_modes(built: Path) -> None:
    assert icon(built, "fixture_front").getpixel((36, 78)) == (150, 150, 150, 255)  # gui_light front: unshaded
    assert icon(built, "fixture_shadedir").getpixel((36, 78)) == (150, 150, 150, 255)  # shaded as the top face
    assert icon(built, "fixture_rescale").getpixel((64, 64))[3] == 255


def test_an_animated_texture_contributes_only_its_first_listed_frame(built: Path) -> None:
    strip = icon(built, "fixture_strip_item")
    assert strip.size == (16, 16) and strip.getpixel((8, 8)) == (0, 0, 255, 255)  # frames [2, 0, 1]: index 2
    flow = icon(built, "fixture_flow_block")
    assert flow.getpixel((64, 29)) == (10, 10, 10, 255)  # the first of two frames, with no frames list


def test_identical_art_is_one_object(built: Path) -> None:
    icons = index_of(built)
    assert icons["minecraft:fixture_twin_a"] == icons["minecraft:fixture_twin_b"]


def test_the_build_record_names_what_was_built_from(run: Any, env: Env, tmp_path: Path) -> None:
    out = tmp_path / "out"
    code, payload, _ = build(run, env, out)
    assert code == 0, payload
    record = json.loads((out / "build-record.json").read_text())
    data = record["data"]
    assert data["source"] == "public-download" and data["version"] == "26.3" and data["input"] == "game"
    assert data["manifest"]["sha1"] == MANIFEST_SHA1
    assert data["server"]["sha1"] == SERVER_SHA1
    assert data["server"]["sha256"] == hashlib.sha256(SERVER.read_bytes()).hexdigest()
    registry = (FIXTURE / "decoder" / "json" / "registries.json").read_bytes()
    assert data["reports"] == {"registries.json": hashlib.sha256(registry).hexdigest()}
    assert record["pixels"]["client"]["sha1"] == CLIENT_SHA1 and record["pixels"]["source"] == "public-download"
    assert record["decoders"][0]["name"] == "minecraft-datagen"
    assert record["decoders"][0]["imageId"].startswith("sha256:")
    assert record["decoders"][0]["tool"]["name"] == "fake-minecraft-datagen"
    details = record["mapper"]
    assert details["name"] == "minecraft" and details["version"] == 1 and details["gameVersion"] == "26.3"
    assert details["registry"] == {"items": 31, "blocks": 8, "excluded": ["minecraft:air"]}
    assert details["definitions"]["selectGuiCase"] == 1 and details["definitions"]["rangeLowestThreshold"] == 1
    assert details["models"]["generated"] == 12 and details["models"]["elements"] == 12
    assert details["animated"] == {"flat": 1, "model": 1}
    assert details["ignoredKeys"]["face.cullface"] == 4
    assert details["itemDefinitionsWithoutCode"] == 1
    assert "client" not in record
    assert "clientBuild" not in payload and payload["inputs"]["pixels"] == {
        "source": "public-download",
        "clientSha1": CLIENT_SHA1,
    }
    assert payload["inputs"]["data"]["serverSha1"] == SERVER_SHA1


def test_the_decoder_runs_hardened_over_a_mount_that_holds_only_the_server_jar(run: Any, env: Env, built: Path) -> None:
    runs = env.decoder_runs()
    assert len(runs) == 1
    argv = runs[0]
    assert "--read-only" in argv and "--network" in argv and argv[argv.index("--network") + 1] == "none"
    mounts = [argv[index + 1] for index, item in enumerate(argv) if item == "-v"]
    game = [mount for mount in mounts if mount.endswith(":/game:ro")]
    assert game == [f"{env.downloads_dir(SERVER_SHA1)}:/game:ro"]
    held = {path.name for path in env.downloads_dir(SERVER_SHA1).iterdir()}
    assert held == {"server.jar", ".takaro"}


def test_a_second_build_downloads_and_decodes_nothing_and_is_byte_identical(run: Any, env: Env, tmp_path: Path) -> None:
    first, second = tmp_path / "first", tmp_path / "second"
    assert build(run, env, first)[0] == 0
    requests, runs = list(env.upstream.requested), len(env.decoder_runs())
    assert len(requests) == 3 and runs == 1
    assert build(run, env, second)[0] == 0
    assert env.upstream.requested == requests and len(env.decoder_runs()) == runs
    assert tree(first / "pack") == tree(second / "pack")
    assert (first / "build-record.json").read_bytes() == (second / "build-record.json").read_bytes()


def test_a_corrupt_cached_download_is_fetched_once_more(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "first")[0] == 0
    (env.downloads_dir(CLIENT_SHA1) / "client.jar").write_bytes(b"corrupt")
    before = env.upstream.requested.count(CLIENT_PATH)
    code, payload, stderr = build(run, env, tmp_path / "second")
    assert code == 0, payload
    assert "corrupt cached download client.jar" in stderr
    assert env.upstream.requested.count(CLIENT_PATH) == before + 1


def test_inputs_reports_cached_downloads(run: Any, env: Env, tmp_path: Path) -> None:
    assert build(run, env, tmp_path / "out")[0] == 0
    _, payload, _ = run("icons", "inputs", "--game", "minecraft", "--target", TARGET, repo=env.repo)
    assert [entry["cached"] for entry in payload["inputs"]] == [True, True]
    assert payload["decoders"]["minecraft-datagen"]["imageBuilt"] is True


@pytest.mark.parametrize("plant", ["symlink", "hardlink"])
def test_a_hostile_decoder_output_fails_the_build_and_leaves_no_pack(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, plant: str
) -> None:
    monkeypatch.setenv("FAKE_DECODER_PLANT", plant)
    out = tmp_path / "out"
    code, _, _ = build(run, env, out)
    assert code == 6 and not (out / "pack").exists()


def test_no_target_means_the_default_revision_which_has_no_client_pin(run: Any, env: Env, tmp_path: Path) -> None:
    code, _, stderr = build(run, env, tmp_path / "out", target=None)
    assert code == 2 and "no client.jar pin for revision 26.2" in stderr and "pinned revision (26.3)" in stderr
    assert env.upstream.requested == []


def test_client_is_refused_before_anything_is_downloaded(run: Any, env: Env, tmp_path: Path) -> None:
    code, _, stderr = build(run, env, tmp_path / "out", "--client", str(tmp_path))
    assert code == 2 and "public download" in stderr
    assert env.upstream.requested == [] and env.decoder_runs() == []


def test_missing_docker_is_refused_before_anything_is_downloaded(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("TAKARO_MAINT_ICONS_DECODER", str(tmp_path / "no-docker"))
    code, _, stderr = build(run, env, tmp_path / "out")
    assert code == 2 and "docker is required" in stderr
    assert env.upstream.requested == []


def test_an_unreachable_client_jar_is_exit_4(run: Any, env: Env, tmp_path: Path) -> None:
    env.upstream.status_overrides[CLIENT_PATH] = 404
    code, _, _ = build(run, env, tmp_path / "out")
    assert code == 4


def test_wrong_client_bytes_are_exit_5_and_leave_no_pack(run: Any, env: Env, tmp_path: Path) -> None:
    env.pin(
        manifest=VERSION.read_bytes(), server=SERVER.read_bytes(), client=CLIENT.read_bytes(), serve_client=b"x" * 10
    )
    out = tmp_path / "out"
    code, _, _ = build(run, env, out)
    assert code == 5 and not (out / "pack").exists()


def test_a_manifest_that_names_another_client_than_the_pin_is_exit_5(run: Any, env: Env, tmp_path: Path) -> None:
    manifest = json.loads(VERSION.read_text())
    manifest["downloads"]["client"]["sha1"] = "1" * 40
    env.pin(manifest=json.dumps(manifest).encode(), server=SERVER.read_bytes(), client=CLIENT.read_bytes())
    code, _, stderr = build(run, env, tmp_path / "out")
    assert code == 5 and "client sha1" in stderr


def test_a_manifest_for_another_version_is_exit_5(run: Any, env: Env, tmp_path: Path) -> None:
    manifest = json.loads(VERSION.read_text())
    manifest["id"] = "26.4"
    env.pin(manifest=json.dumps(manifest).encode(), server=SERVER.read_bytes(), client=CLIENT.read_bytes())
    code, _, stderr = build(run, env, tmp_path / "out")
    assert code == 5 and "version" in stderr


def test_a_decoder_crash_and_a_failed_report_are_exit_6(
    run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DECODER_EXIT", "3")
    assert build(run, env, tmp_path / "crash")[0] == 6
    monkeypatch.delenv("FAKE_DECODER_EXIT")
    monkeypatch.setenv("FAKE_DECODER_FAIL", "r-registries")
    code, _, stderr = build(run, env, tmp_path / "failed")
    assert code == 6 and "data generator failed" in stderr


def test_a_registry_of_another_shape_is_exit_6(run: Any, env: Env, tmp_path: Path) -> None:
    env.edit_decoder_json(lambda document: document.pop("minecraft:block"))
    code, _, stderr = build(run, env, tmp_path / "out")
    assert code == 6 and "data report format changed" in stderr


def test_an_oversized_jar_entry_is_exit_6(run: Any, env: Env, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(minecraft_icons, "MAX_ENTRY_BYTES", 1000)
    code, _, stderr = build(run, env, tmp_path / "out")
    assert code == 6 and "over the 1000 cap" in stderr


def test_the_committed_fixture_is_what_the_generator_writes(tmp_path: Path) -> None:
    maker.write(tmp_path)
    expected = {
        path.relative_to(tmp_path).as_posix(): path.read_bytes() for path in tmp_path.rglob("*") if path.is_file()
    }
    committed = {name: (FIXTURE / name).read_bytes() for name in expected}
    assert committed == expected
