"""`build`: artifacts are collected by exact name, validated, hashed and described."""

from __future__ import annotations

import json
import os
import stat
from pathlib import Path
from typing import Any

import pytest

from conftest import make_jar


@pytest.fixture
def gradle_stub(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Any:
    """A stand-in for ./gradlew that writes whatever files a test asks it to."""

    def _stub(repo: Path, files: dict[str, dict[str, Any]], *, exit_code: int = 0) -> Path:
        plan = tmp_path / "plan.json"
        plan.write_text(json.dumps({"root": str(repo), "files": files, "exit": exit_code}))
        script = tmp_path / "gradle-stub.py"
        script.write_text(
            "import json, os, sys, zipfile\n"
            f"plan = json.loads(open({str(plan)!r}).read())\n"
            "root = plan['root']\n"
            "for relative, spec in plan['files'].items():\n"
            "    path = os.path.join(root, relative)\n"
            "    os.makedirs(os.path.dirname(path), exist_ok=True)\n"
            "    with zipfile.ZipFile(path, 'w') as archive:\n"
            "        archive.writestr('META-INF/MANIFEST.MF',\n"
            "            ''.join(f'{k}: {v}\\n' for k, v in spec['manifest'].items()) + '\\n')\n"
            "        if spec.get('stamp'):\n"
            "            archive.writestr('META-INF/takaro-target.json', json.dumps(spec['stamp']))\n"
            "        archive.writestr('marker.txt', relative)\n"
            "sys.exit(plan['exit'])\n"
        )
        monkeypatch.setenv("TAKARO_MAINT_GRADLE", f"{os.sys.executable} {script}")
        return script

    return _stub


def artifact_spec(
    fingerprint: str,
    *,
    version: str = "0.1.1",
    target: str = "fabric-26.2",
    revision: str = "26.2",
) -> dict[str, Any]:
    return {
        "manifest": {
            "Manifest-Version": "1.0",
            "Takaro-Target": target,
            "Takaro-Target-Fingerprint": fingerprint,
            "Takaro-Connector-Version": version,
            "Takaro-Source-Revision": "deadbeef",
            "Takaro-Game-Version": revision,
        },
        "stamp": {"target": target, "fingerprint": fingerprint},
    }


def fingerprint_of(run: Any, repo: Path) -> str:
    _, payload, _ = run("targets", "resolve", "--game", "minecraft", "--target", "fabric-26.2", repo=repo)
    return str(payload["fingerprint"])


LIBS = "games/minecraft/mod/targets/fabric-26.2/build/libs"


def test_only_the_exact_catalog_named_artifact_is_collected(
    run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path
) -> None:
    fingerprint = fingerprint_of(run, catalog_copy)
    spec = artifact_spec(fingerprint)
    decoys = {
        f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1.jar": spec,
        f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1-dev-shadow.jar": spec,
        f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1-sources.jar": spec,
        f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.0.9.jar": artifact_spec(fingerprint, version="0.0.9"),
        f"{LIBS}/TakaroMinecraft.jar": spec,
    }
    gradle_stub(catalog_copy, decoys)
    out = tmp_path / "dist"

    code, payload, _ = run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(out),
        repo=catalog_copy,
    )

    assert code == 0, payload
    assert [row["file"] for row in payload["artifacts"]] == ["takaro-minecraft-mod-fabric-26.2-0.1.1.jar"]
    assert sorted(p.name for p in out.glob("*.jar")) == ["takaro-minecraft-mod-fabric-26.2-0.1.1.jar"]


def test_the_manifest_checksums_and_meta_file_are_written(
    run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path, repo_root: Path
) -> None:
    from jsonschema import Draft202012Validator

    fingerprint = fingerprint_of(run, catalog_copy)
    gradle_stub(
        catalog_copy,
        {f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1.jar": artifact_spec(fingerprint)},
    )
    out = tmp_path / "dist"

    code, payload, _ = run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(out),
        repo=catalog_copy,
    )

    assert code == 0, payload
    manifest = json.loads((out / "build-manifest.json").read_text())
    schema = json.loads((repo_root / "catalog/schema/v1/build-manifest.schema.json").read_text())
    assert list(Draft202012Validator(schema).iter_errors(manifest)) == []
    assert manifest["connector"] == "minecraft"
    assert manifest["artifacts"][0]["fingerprint"] == fingerprint

    sums = (out / "SHA256SUMS").read_text().strip().splitlines()
    assert len(sums) == 1
    digest, name = sums[0].split("  ")
    assert name == "takaro-minecraft-mod-fabric-26.2-0.1.1.jar"
    assert digest == manifest["artifacts"][0]["sha256"]

    meta = json.loads((out / "takaro-minecraft-mod-fabric-26.2-0.1.1.jar.meta.json").read_text())
    assert meta["target"] == "fabric-26.2"
    assert meta["connector"] == "minecraft"
    assert meta["version"] == "0.1.1"


def test_a_failing_build_exits_six(run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path) -> None:
    gradle_stub(catalog_copy, {}, exit_code=1)

    code, payload, _ = run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(tmp_path / "dist"),
        repo=catalog_copy,
    )

    assert code == 6
    assert "exited 1" in payload["error"]


def test_a_build_that_produces_nothing_is_a_conflict(
    run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path
) -> None:
    gradle_stub(catalog_copy, {f"{LIBS}/something-else.jar": artifact_spec("0" * 64)})

    code, payload, _ = run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(tmp_path / "dist"),
        repo=catalog_copy,
    )

    assert code == 7
    assert "never a glob" in payload["error"]


def test_an_unstamped_artifact_is_a_conflict(run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path) -> None:
    spec = artifact_spec("0" * 64)
    gradle_stub(catalog_copy, {f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1.jar": spec})

    code, payload, _ = run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(tmp_path / "dist"),
        repo=catalog_copy,
    )

    assert code == 7
    assert "identity" in payload["error"]


def test_all_targets_covers_every_non_retired_target(
    run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path
) -> None:
    from test_cli_targets import second_target

    second_target(catalog_copy)
    _, listing, _ = run(
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
    files = {}
    for row in listing["targets"]:
        target = str(row["id"])
        _, resolved, _ = run("targets", "resolve", "--game", "minecraft", "--target", target, repo=catalog_copy)
        record = json.loads((catalog_copy / f"catalog/minecraft/targets/{target}.json").read_text())
        artifact = record["components"][0]["artifact"].replace("{version}", "0.1.1")
        project = record["build"]["gradleProject"]
        files[f"games/minecraft/mod/targets/{project}/build/libs/{artifact}"] = artifact_spec(
            resolved["fingerprint"], target=target, revision=record["revision"]
        )
    gradle_stub(catalog_copy, files)
    out = tmp_path / "dist"

    code, payload, _ = run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--all-targets",
        "--version",
        "0.1.1",
        "--out",
        str(out),
        repo=catalog_copy,
    )

    assert code == 0, payload
    expected = {str(row["id"]) for row in listing["targets"]}
    assert {row["target"] for row in payload["artifacts"]} == expected
    assert len((out / "SHA256SUMS").read_text().strip().splitlines()) == len(expected)


def test_the_output_directory_is_created_when_missing(
    run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path
) -> None:
    gradle_stub(
        catalog_copy,
        {f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1.jar": artifact_spec(fingerprint_of(run, catalog_copy))},
    )
    out = tmp_path / "deep" / "dist"

    code, _, _ = run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(out),
        repo=catalog_copy,
    )

    assert code == 0
    assert stat.S_ISDIR(os.stat(out).st_mode)


def test_a_built_artifact_validates_against_its_own_target(
    run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path
) -> None:
    gradle_stub(
        catalog_copy,
        {f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1.jar": artifact_spec(fingerprint_of(run, catalog_copy))},
    )
    out = tmp_path / "dist"
    run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(out),
        repo=catalog_copy,
    )

    code, _, _ = run(
        "artifact",
        "validate",
        "--game",
        "minecraft",
        "--target",
        "fabric-26.2",
        str(out / "takaro-minecraft-mod-fabric-26.2-0.1.1.jar"),
        repo=catalog_copy,
    )

    assert code == 0


def test_a_decoy_jar_never_reaches_the_manifest(run: Any, catalog_copy: Path, gradle_stub: Any, tmp_path: Path) -> None:
    fingerprint = fingerprint_of(run, catalog_copy)
    gradle_stub(
        catalog_copy,
        {
            f"{LIBS}/takaro-minecraft-mod-fabric-26.2-0.1.1.jar": artifact_spec(fingerprint),
            f"{LIBS}/takaro-fabric-0.1.1.jar": artifact_spec(fingerprint),
        },
    )
    out = tmp_path / "dist"
    run(
        "build",
        "--game",
        "minecraft",
        "--platform",
        "fabric",
        "--version",
        "0.1.1",
        "--out",
        str(out),
        repo=catalog_copy,
    )

    manifest = json.loads((out / "build-manifest.json").read_text())

    assert [row["file"] for row in manifest["artifacts"]] == ["takaro-minecraft-mod-fabric-26.2-0.1.1.jar"]
    assert not (out / "takaro-fabric-0.1.1.jar").exists()


def test_make_jar_helper_and_command_agree_on_what_valid_means(run: Any, catalog_copy: Path, tmp_path: Path) -> None:
    jar = make_jar(
        tmp_path / "takaro-minecraft-mod-fabric-26.2-0.1.1.jar",
        target="fabric-26.2",
        fingerprint=fingerprint_of(run, catalog_copy),
        revision="26.2",
    )

    code, _, _ = run(
        "artifact", "validate", "--game", "minecraft", "--target", "fabric-26.2", str(jar), repo=catalog_copy
    )

    assert code == 0
