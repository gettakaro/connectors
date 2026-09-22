"""`catalog validate` and `catalog record-hash` through the command line."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

import pytest

from conftest import point_at, read_target, write_target
from fake_upstream import FakeUpstream


def failures(payload: dict[str, Any]) -> set[str]:
    return {check["id"] for check in payload["failures"]}


def test_the_repository_catalog_is_valid(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    assert payload["ok"] is True
    assert payload["failures"] == []


def test_every_catalog_dev_server_claim_has_a_registered_rig() -> None:
    repo = Path(__file__).resolve().parents[2]
    script = """
        . dev-servers/lib/common.sh
        for rig in $(ds_game_ids); do
            printf '%s|%s|%s\\n' "$rig" "$(ds_target_game "$rig")" "$(basename "$(ds_compose_file "$rig")")"
        done
    """
    completed = subprocess.run(["bash", "-c", script], cwd=repo, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    rigs = {
        rig: {"game": game, "compose": compose}
        for rig, game, compose in (line.split("|", 2) for line in completed.stdout.splitlines())
    }

    for game_file in sorted((repo / "catalog").glob("*/game.json")):
        game = json.loads(game_file.read_text(encoding="utf-8"))
        dev = game.get("devServers") or {}
        if compose := dev.get("composeFile"):
            assert (repo / "dev-servers" / "compose" / compose).is_file(), game_file
            assert any(row["game"] == game["id"] and row["compose"] == compose for row in rigs.values()), game_file

    for target_file in sorted((repo / "catalog").glob("*/targets/*.json")):
        target = json.loads(target_file.read_text(encoding="utf-8"))
        if rig := (target.get("devServers") or {}).get("gameId"):
            assert rig in rigs, target_file
            assert rigs[rig]["game"] == target_file.parents[1].name, target_file


def test_id_must_match_platform_revision_and_file_name(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["id"] = "fabric-26.3"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "id-matches-stem" in failures(payload)


def test_platform_must_be_declared_by_the_game(run: Any, catalog_copy: Path) -> None:
    game_file = catalog_copy / "catalog/minecraft/game.json"
    game = json.loads(game_file.read_text())
    game["platforms"] = ["paper", "neoforge"]
    game_file.write_text(json.dumps(game, indent=2))

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "platform-declared" in failures(payload)


def test_two_defaults_for_one_platform_are_invalid(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    twin = json.loads(json.dumps(record))
    twin["id"] = "fabric-26.1.2"
    twin["revision"] = "26.1.2"
    write_target(catalog_copy, twin, "fabric-26.1.2")

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "single-default" in failures(payload)


def test_zero_defaults_are_invalid(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["default"] = False
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "single-default" in failures(payload)


def test_an_input_must_match_its_kind_schema(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    del record["inputs"]["loader"]["launcherVersion"]
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "input-kind-schema" in failures(payload)


def test_an_unknown_input_kind_is_rejected(run: Any, catalog_copy: Path) -> None:
    """The target schema does not enumerate input kinds, so the kind gate is the only check on them."""
    record = read_target(catalog_copy)
    record["inputs"]["loader"]["kind"] = "not-a-real-kind"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "input-kind-schema" in failures(payload)
    assert any("not-a-real-kind" in check["detail"] for check in payload["failures"])


def test_an_install_path_that_leaves_the_install_directory_is_rejected(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["inputs"]["fabricApi"]["installPath"] = "../../etc/cron.d/takaro"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "input-kind-schema" in failures(payload)


def test_a_component_install_directory_that_leaves_the_install_directory_is_rejected(
    run: Any, catalog_copy: Path
) -> None:
    record = read_target(catalog_copy)
    record["components"][0]["installDir"] = "/etc/systemd/system"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "target-schema" in failures(payload)


def test_a_maintained_target_may_not_carry_a_null_hash(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["inputs"]["fabricApi"]["sha256"] = None
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "no-null-hash" in failures(payload)


def test_the_minecraft_dependency_is_the_one_allowed_null(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    assert record["build"]["deps"]["minecraft"]["sha256"] is None

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 0, payload


def test_a_floating_image_tag_is_rejected(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["runtime"]["container"]["tag"] = "java25"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "immutable-tag-and-digest" in failures(payload)


def test_a_missing_digest_is_rejected(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    del record["build"]["toolchain"]["digest"]
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "immutable-tag-and-digest" in failures(payload)


def test_a_floating_version_word_anywhere_is_rejected(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["inputs"]["fabricApi"]["version"] = "latest"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "no-floating-words" in failures(payload)


def test_the_java_chain_must_agree(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["build"]["javaRelease"] = 21
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "minecraft-java-chain" in failures(payload)


def test_plugin_versions_must_match_the_version_catalog(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["build"]["plugins"]["shadow"] = "9.9.9"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "plugins-match-toml" in failures(payload)


def test_a_target_without_a_gradle_project_is_rejected(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["build"]["gradleProject"] = "fabric-99.9"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "gradle-project-exists" in failures(payload)


def test_build_deps_must_agree_with_the_pinned_inputs(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["build"]["deps"]["fabric-api"]["sha256"] = "f" * 64
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "deps-consistent" in failures(payload)


def test_a_maven_path_must_be_derivable_from_its_coordinate(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["inputs"]["fabricApi"]["path"] = "/net/fabricmc/fabric-api/fabric-api/other/fabric-api-other.jar"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "maven-path-derivable" in failures(payload)


def test_a_launcher_path_must_be_derivable(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["inputs"]["loader"]["path"] = "/v2/versions/loader/26.2/0.19.9/1.1.2/server/jar"
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    assert "launcher-path-derivable" in failures(payload)


def test_a_separate_verification_claim_must_name_a_real_check(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["verification"]["separate"].append("not-a-verification-check")
    write_target(catalog_copy, record)

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    failure = next(check for check in payload["failures"] if check["id"] == "separate-names-checks")
    assert "not-a-verification-check" in failure["detail"]


def test_online_validation_passes_against_matching_upstream(run: Any, wired: Any) -> None:
    """Whole-catalog online validation covers exactly the targets ``wired.served`` declares."""
    code, payload, _ = run("catalog", "validate", "--online", repo=wired.root)

    assert code == 0, payload
    ids = {check["id"] for check in payload["checks"]}
    assert {"online-manifest", "online-hash"} <= ids


def test_the_wired_fixture_serves_exactly_the_targets_it_says(wired: Any) -> None:
    assert wired.served == ("fabric-26.1.2", "fabric-26.2")
    assert wired.unserved == {
        "carbon-25353106": "no repinner for platform 'carbon'",
        "carbon-25454815": "no repinner for platform 'carbon'",
        "fabric-26.3": "the repinner has no fixture for this target",
        "linux-1.0.15": "no repinner for platform 'linux'",
        "linux-25356024": "no repinner for platform 'linux'",
        "linux-3.2.0.b10": "no repinner for platform 'linux'",
        "linux-42.20.4": "no repinner for platform 'linux'",
        "neoforge-1.21.11": "no repinner for platform 'neoforge'",
        "paper-1.21.11": "no repinner for platform 'paper'",
        "proton-1024233": "no repinner for platform 'proton'",
        "tshock-v6.1.0": "no repinner for platform 'tshock'",
    }
    remaining = sorted(
        json.loads(path.read_text(encoding="utf-8"))["id"] for path in (wired.root / "catalog").glob("*/targets/*.json")
    )
    assert remaining == list(wired.served)


def test_online_validation_reports_a_changed_upstream_hash(run: Any, wired: Any) -> None:
    record = wired.target()
    wired.upstream.files[record["inputs"]["loader"]["path"]] = b"someone republished this jar"

    code, payload, _ = run("catalog", "validate", "--online", repo=wired.root)

    assert code == 5
    assert "online-hash" in failures(payload)


def test_online_validation_reports_a_changed_server_sha1(run: Any, wired: Any) -> None:
    record = wired.target()
    manifest_path = record["inputs"]["game"]["manifest"]["path"]
    manifest = json.loads(wired.upstream.files[manifest_path])
    manifest["downloads"]["server"]["sha1"] = "0" * 40
    payload_bytes = json.dumps(manifest).encode("utf-8")
    record["inputs"]["game"]["manifest"]["sha1"] = __import__("hashlib").sha1(payload_bytes).hexdigest()
    record["inputs"]["game"]["manifest"]["path"] = manifest_path
    wired.save(record)
    wired.upstream.files[manifest_path] = payload_bytes

    code, payload, _ = run("catalog", "validate", "--online", repo=wired.root)

    assert code == 5
    assert "online-manifest" in failures(payload)


def test_online_validation_reports_an_unreachable_input(run: Any, wired: Any) -> None:
    record = wired.target()
    wired.upstream.status_overrides[record["inputs"]["fabricApi"]["path"]] = 503

    code, payload, _ = run("catalog", "validate", "--online", repo=wired.root)

    assert code == 4
    assert "online-hash" in failures(payload)


def test_record_hash_writes_the_value_two_downloads_agree_on(run: Any, catalog_copy: Path) -> None:
    payload_bytes = b"a brand new artifact nobody has hashed yet"
    with FakeUpstream() as upstream:
        record = read_target(catalog_copy)
        record["inputs"]["fabricApi"]["sha256"] = None
        original_text = write_target(catalog_copy, record).read_text()
        point_at(catalog_copy, upstream.base_url)
        expected = upstream.add(record["inputs"]["fabricApi"]["path"], payload_bytes)

        code, payload, stderr = run(
            "catalog",
            "record-hash",
            "--game",
            "minecraft",
            "--target",
            "fabric-26.2",
            "--field",
            "inputs.fabricApi.sha256",
            repo=catalog_copy,
        )

    assert code == 0, payload
    assert payload["sha256"] == expected
    assert payload["downloads"] == 2
    assert stderr.count("download ") == 2
    updated = read_target(catalog_copy)
    assert updated["inputs"]["fabricApi"]["sha256"] == expected
    # Only that one value changed; the file keeps its shape.
    assert original_text.replace("null", f'"{expected}"', 1) != original_text


def test_record_hash_refuses_when_the_two_downloads_disagree(run: Any, catalog_copy: Path) -> None:
    class Flapping(FakeUpstream):
        def add_flapping(self, path: str) -> None:
            self.files[path] = b"first"

    with Flapping() as upstream:
        record = read_target(catalog_copy)
        path = record["inputs"]["fabricApi"]["path"]
        record["inputs"]["fabricApi"]["sha256"] = None
        target_file = write_target(catalog_copy, record)
        before = target_file.read_bytes()
        point_at(catalog_copy, upstream.base_url)
        upstream.files[path] = b"first"

        original_get = upstream.files.get
        state = {"calls": 0}

        def flapping_get(key: str, default: Any = None) -> Any:
            state["calls"] += 1
            if key == path and state["calls"] > 1:
                return b"second, and different"
            return original_get(key, default)

        upstream.files = type("D", (dict,), {"get": staticmethod(flapping_get)})(upstream.files)

        code, payload, _ = run(
            "catalog",
            "record-hash",
            "--game",
            "minecraft",
            "--target",
            "fabric-26.2",
            "--field",
            "inputs.fabricApi.sha256",
            repo=catalog_copy,
        )

    assert code == 5
    assert target_file.read_bytes() == before


@pytest.mark.parametrize("field", ["inputs.game.sha256", "build.plugins.shadow", "nonsense"])
def test_record_hash_rejects_a_field_it_cannot_download(run: Any, catalog_copy: Path, field: str) -> None:
    code, _, _ = run(
        "catalog",
        "record-hash",
        "--game",
        "minecraft",
        "--target",
        "fabric-26.2",
        "--field",
        field,
        repo=catalog_copy,
    )

    assert code == 2
