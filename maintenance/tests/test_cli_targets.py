"""`targets list` and `targets resolve`: selection, output shapes and env contracts."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from conftest import read_target, write_target

EXPECTED_ENV_KEYS = {
    "MC_FABRIC_TARGET",
    "MC_FABRIC_FINGERPRINT",
    "MC_FABRIC_FP16",
    "MC_FABRIC_IMAGE",
    "MC_FABRIC_JAVA",
    "MC_FABRIC_VERSION",
    "MC_FABRIC_LOADER_VERSION",
    "MC_FABRIC_LAUNCHER_VERSION",
    "MC_FABRIC_LAUNCHER",
}


def second_target(root: Path, *, default: bool = False, revision: str = "26.1.2") -> str:
    record = read_target(root)
    twin = json.loads(json.dumps(record))
    twin["id"] = f"fabric-{revision}"
    twin["revision"] = revision
    twin["default"] = default
    twin["build"]["gradleProject"] = f"fabric-{revision}"
    twin["components"] = [
        {**component, "artifact": component["artifact"].replace("fabric-26.2", twin["id"])}
        for component in twin["components"]
    ]
    write_target(root, twin, twin["id"])
    project = root / "games/minecraft/mod/targets" / twin["build"]["gradleProject"]
    project.mkdir(parents=True, exist_ok=True)
    (project / "build.gradle.kts").write_text('plugins { id("takaro.fabric-target") }\n')
    return str(twin["id"])


def test_list_json_carries_every_summary_field(run: Any) -> None:
    code, payload, _ = run("targets", "list", "--game", "minecraft")

    assert code == 0
    assert payload["count"] >= 1
    row = next(r for r in payload["targets"] if r["id"] == "fabric-26.2")
    assert row["platform"] == "fabric"
    assert row["revision"] == "26.2"
    assert row["status"] == "maintained"
    assert len(row["fp16"]) == 16


def test_list_table_is_human_readable(run: Any) -> None:
    code, payload, _ = run("targets", "list", "--format", "table")

    assert code == 0
    assert "TARGET" in payload
    assert "fabric-26.2" in payload


def test_list_gha_emits_a_matrix_array(run: Any) -> None:
    code, payload, _ = run("targets", "list", "--game", "minecraft", "--format", "gha")

    assert code == 0
    assert payload.startswith("targets=")
    rows = json.loads(payload.split("=", 1)[1])
    assert "fabric-26.2" in [row["id"] for row in rows]


def test_list_filters_by_status(run: Any, catalog_copy: Path) -> None:
    second_target(catalog_copy)
    record = read_target(catalog_copy, "fabric-26.1.2")
    record["support"]["status"] = "retired"
    write_target(catalog_copy, record, "fabric-26.1.2")

    code, payload, _ = run(
        "targets",
        "list",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--status",
        "candidate,maintained",
        repo=catalog_copy,
    )

    assert code == 0
    assert [row["id"] for row in payload["targets"]] == ["fabric-26.2", "fabric-26.3"]


def test_list_filters_by_rig_game(run: Any) -> None:
    code, payload, _ = run("targets", "list", "--rig-game", "minecraft-fabric")

    assert code == 0
    assert [row["id"] for row in payload["targets"]] == ["fabric-26.2"]


def test_list_rig_game_that_matches_nothing_is_empty(run: Any) -> None:
    code, payload, _ = run("targets", "list", "--rig-game", "minecraft-bedrock")

    assert code == 0
    assert payload["targets"] == []


def test_explicit_target_beats_the_declared_default(run: Any, catalog_copy: Path) -> None:
    other = second_target(catalog_copy)

    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--target", other, repo=catalog_copy)

    assert code == 0
    assert payload["id"] == other
    assert payload["default"] is False


def test_no_target_uses_the_single_default(run: Any, catalog_copy: Path) -> None:
    second_target(catalog_copy)

    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--platform", "fabric", repo=catalog_copy)

    assert code == 0
    assert payload["id"] == "fabric-26.2"


def test_an_unknown_target_exits_three(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--target", "fabric-99")

    assert code == 3
    assert "fabric-99" in payload["error"]


def test_two_defaults_exit_three(run: Any, catalog_copy: Path) -> None:
    second_target(catalog_copy, default=True)

    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--platform", "fabric", repo=catalog_copy)

    assert code == 3
    assert "2 default targets" in payload["error"]


def test_a_default_per_platform_and_no_default_platform_is_ambiguous(run: Any, catalog_copy: Path) -> None:
    """Without ``defaultPlatform`` there is nothing to choose with, and the error names the options."""
    game_file = catalog_copy / "catalog" / "minecraft" / "game.json"
    game = json.loads(game_file.read_text())
    del game["defaultPlatform"]
    game_file.write_text(json.dumps(game, indent=2))

    code, payload, _ = run("targets", "resolve", "--game", "minecraft", repo=catalog_copy)
    assert code == 3
    assert "3 default targets" in payload["error"]
    assert "--platform (fabric, neoforge, paper)" in payload["error"]

    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--platform", "fabric", repo=catalog_copy)
    assert code == 0
    assert payload["id"] == "fabric-26.2"


def test_zero_defaults_exit_three(run: Any, catalog_copy: Path) -> None:
    record = read_target(catalog_copy)
    record["default"] = False
    write_target(catalog_copy, record)

    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--platform", "fabric", repo=catalog_copy)

    assert code == 3
    assert "no default target" in payload["error"]


def test_an_unknown_game_exits_two(run: Any) -> None:
    code, _, _ = run("targets", "resolve", "--game", "nosuchgame")

    assert code == 2


def test_resolve_json_carries_the_derived_deployment_facts(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--target", "fabric-26.2")

    assert code == 0
    assert payload["fingerprint"][:16] == payload["fp16"]
    assert payload["containerRef"].startswith("itzg/minecraft-server:2026.9.1-java25@sha256:")
    assert payload["toolchainRef"].startswith("eclipse-temurin:25-jdk@sha256:")
    assert payload["artifactFileNames"]["server-mod"] == "takaro-minecraft-mod-fabric-26.2-{version}.jar"
    assert payload["resolvedUrls"]["loader"].startswith("https://meta.fabricmc.net/v2/versions/loader/26.2/")
    assert payload["resolvedUrls"]["game.server"].startswith("https://piston-data.mojang.com/")


def test_resolve_env_prints_exactly_the_documented_keys(run: Any) -> None:
    code, payload, _ = run(
        "targets",
        "resolve",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--format",
        "env",
        "--prefix",
        "MC_FABRIC",
    )

    assert code == 0
    keys = {line.split("=", 1)[0] for line in payload.strip().splitlines()}
    assert keys == EXPECTED_ENV_KEYS
    values = dict(line.split("=", 1) for line in payload.strip().splitlines())
    assert values["MC_FABRIC_VERSION"] == "26.2"
    assert values["MC_FABRIC_LOADER_VERSION"] == "0.19.5"
    assert values["MC_FABRIC_LAUNCHER"] == "fabric-server-mc.26.2-loader.0.19.5-launcher.1.1.2.jar"


def test_resolve_env_out_file_is_private(run: Any, tmp_path: Path) -> None:
    out = tmp_path / "targets" / "minecraft-fabric.env"

    code, _, _ = run(
        "targets",
        "resolve",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--format",
        "env",
        "--prefix",
        "MC_FABRIC",
        "--out",
        str(out),
    )

    assert code == 0
    assert oct(os.stat(out).st_mode)[-3:] == "600"
    assert "MC_FABRIC_TARGET=fabric-26.2" in out.read_text()


def test_resolve_gha_carries_one_key_per_line_plus_env(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--platform", "fabric", "--format", "gha")

    assert code == 0
    lines = dict(line.split("=", 1) for line in payload.strip().splitlines())
    assert lines["target"] == "fabric-26.2"
    assert lines["java"] == "25"
    assert json.loads(lines["env"])["TAKARO_TARGET_TARGET"] == "fabric-26.2"


def test_the_fingerprint_matches_the_shared_fixture(run: Any, repo_root: Path) -> None:
    fixture = json.loads((repo_root / "maintenance/tests/fixtures/fingerprints.json").read_text())
    real = next(case for case in fixture if case["record"].get("id") == "fabric-26.2" and "real" in case["name"])

    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--target", "fabric-26.2")

    assert code == 0
    assert payload["fingerprint"] == real["fingerprint"]


def test_game_alone_resolves_the_declared_default_platform(run: Any) -> None:
    """Three platforms each declare a default; ``defaultPlatform`` says which one ``--game`` means."""
    code, payload, err = run("targets", "resolve", "--game", "minecraft")

    assert code == 0, err
    assert payload["id"] == "fabric-26.2"
    assert payload["platform"] == "fabric"


def test_platform_still_takes_that_platforms_default(run: Any) -> None:
    code, payload, err = run("targets", "resolve", "--game", "minecraft", "--platform", "paper")

    assert code == 0, err
    assert payload["id"] == "paper-1.21.11"


def test_a_default_platform_that_names_no_platform_fails_validation(run: Any, catalog_copy: Path) -> None:
    game_file = catalog_copy / "catalog" / "minecraft" / "game.json"
    game = json.loads(game_file.read_text())
    game["defaultPlatform"] = "forge"
    game_file.write_text(json.dumps(game, indent=2))

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code != 0
    assert any(failure["id"] == "default-platform" for failure in payload["failures"])
