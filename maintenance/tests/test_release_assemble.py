"""`release assemble`: a connector ships as a complete, verified set or it does not ship.

The fixtures here build what CI hands the publisher — one ``dist/<target>/`` per build job and
one ``reports/<target>/`` per verification job — so every assertion below is about the command's
observable behaviour on the real shape of those directories.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

from conftest import make_jar
from takaro_maint.publish.manifest import artifact_row, write_manifest, write_meta

REPO_ROOT = Path(__file__).resolve().parents[2]
VERSION = "0.1.1"

# The old, unversioned-per-target names Minecraft released under before the catalog. They stay
# on the release for two stable versions (root README), one per platform's default target.
LEGACY_ALIASES = {
    f"takaro-fabric-{VERSION}.jar": "fabric-26.2",
    f"takaro-neoforge-{VERSION}.jar": "neoforge-1.21.11",
    f"takaro-paper-{VERSION}.jar": "paper-1.21.11",
}
CONNECTOR = "minecraft"
REPO = "o/r"


def git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=repo, capture_output=True, text=True, check=True, env=_git_env()
    ).stdout.strip()


def _git_env() -> dict[str, str]:
    return {
        **os.environ,
        "GIT_AUTHOR_NAME": "t",
        "GIT_AUTHOR_EMAIL": "t@example.invalid",
        "GIT_COMMITTER_NAME": "t",
        "GIT_COMMITTER_EMAIL": "t@example.invalid",
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_CONFIG_SYSTEM": "/dev/null",
    }


@dataclass
class Inputs:
    """One CI run's worth of build and verification output."""

    root: Path
    dist: Path
    reports: Path
    commit: str
    targets: dict[str, dict[str, Any]]

    def jar(self, target_id: str) -> Path:
        return self.dist / target_id / self.targets[target_id]["file"]

    def report(self, target_id: str) -> Path:
        return self.reports / target_id / "report.json"

    def rewrite_report(self, target_id: str, **fields: Any) -> None:
        path = self.report(target_id)
        document = json.loads(path.read_text())
        document.update(fields)
        path.write_text(json.dumps(document, indent=2))

    def rewrite_manifest(self, target_id: str, mutate: Any) -> None:
        path = self.dist / target_id / "build-manifest.json"
        document = json.loads(path.read_text())
        mutate(document)
        path.write_text(json.dumps(document, indent=2))


def resolve(run: Any, root: Path, target_id: str) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", CONNECTOR, "--target", target_id, repo=root)
    assert code == 0, err
    return dict(payload)


def target_ids(root: Path) -> list[str]:
    return sorted(path.stem for path in (root / "catalog" / CONNECTOR / "targets").glob("*.json"))


def write_report(
    path: Path,
    *,
    target_id: str,
    fingerprint: str,
    role: str,
    file: str,
    sha256: str,
    commit: str,
    level: str = "protocol",
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "kind": "runtime-verification",
                "tool": {"name": "takaro-maint", "version": "0.1.0"},
                "source": {"repo": REPO, "revision": commit, "dirty": False},
                "target": {"game": CONNECTOR, "id": target_id, "fingerprint": fingerprint, "inputs": {}},
                "artifacts": [{"role": role, "file": file, "sha256": sha256, "connectorVersion": VERSION}],
                "runtime": {
                    "image": {"ref": "itzg/minecraft-server:pinned", "digest": "sha256:" + "0" * 64},
                    "gameVersion": "26.2",
                    "loader": "fabric",
                    "loaderVersion": "0.19.5",
                    "java": 25,
                },
                "takaro": "local",
                "level": level,
                "checks": [{"id": "startup", "status": "pass", "durationMs": 1, "detail": {}}],
                "outcome": "pass",
                "startedAt": "2026-09-21T00:00:00.000000Z",
                "finishedAt": "2026-09-21T00:00:01.000000Z",
                "logs": [],
                "coverage": {"gameplay": "not covered by this harness"},
            },
            indent=2,
        )
    )


def build_inputs(run: Any, catalog_copy: Path, tmp_path: Path) -> Inputs:
    """A committed checkout plus the dist and report trees a full CI run produces."""
    root = catalog_copy
    git(root, "init", "-q", "-b", "main")
    git(root, "add", "-A")
    git(root, "commit", "-qm", "fixture")
    commit = git(root, "rev-parse", "HEAD")

    dist = tmp_path / "dist"
    reports = tmp_path / "reports"
    described: dict[str, dict[str, Any]] = {}
    for target_id in target_ids(root):
        resolved = resolve(run, root, target_id)
        role, pattern = next(iter(resolved["artifactFileNames"].items()))
        file = pattern.replace("{version}", VERSION)
        directory = dist / target_id
        directory.mkdir(parents=True)
        make_jar(
            directory / file,
            target=target_id,
            fingerprint=resolved["fingerprint"],
            revision=resolved["revision"],
            version=VERSION,
            source_revision=commit,
        )
        row = artifact_row(role, target_id, resolved["fingerprint"], directory / file)
        write_manifest(
            directory,
            connector=CONNECTOR,
            version=VERSION,
            revision=commit,
            dirty=False,
            toolchain=resolved["build"]["toolchain"],
            mode="container",
            artifacts=[row],
        )
        write_meta(directory, row, connector=CONNECTOR, version=VERSION, revision=commit)
        write_report(
            reports / target_id / "report.json",
            target_id=target_id,
            fingerprint=resolved["fingerprint"],
            role=role,
            file=file,
            sha256=row["sha256"],
            commit=commit,
        )
        described[target_id] = {
            "file": file,
            "role": role,
            "fingerprint": resolved["fingerprint"],
            "sha256": row["sha256"],
        }
    return Inputs(root=root, dist=dist, reports=reports, commit=commit, targets=described)


def assemble(run: Any, inputs: Inputs, out: Path, *extra: str, channel: str = "stable") -> tuple[int, Any, str]:
    return run(
        "release",
        "assemble",
        "--connector",
        CONNECTOR,
        "--version",
        VERSION,
        "--channel",
        channel,
        "--tag",
        f"{CONNECTOR}-v{VERSION}",
        "--dist",
        str(inputs.dist),
        "--reports",
        str(inputs.reports),
        "--out",
        str(out),
        "--repo",
        REPO,
        *extra,
        repo=inputs.root,
    )


def assemble_default(run: Any, built: Inputs, tmp_path: Path) -> tuple[Path, dict[str, Any], Inputs]:
    """The happy path, ready for the publish and verify suites to work against."""
    out = tmp_path / "assembled"
    code, payload, err = assemble(run, built, out)
    assert code == 0, f"{err}\n{payload}"
    return out, dict(payload), built


@pytest.fixture
def inputs(run: Any, catalog_copy: Path, tmp_path: Path) -> Inputs:
    return build_inputs(run, catalog_copy, tmp_path)


@pytest.fixture
def assembled(run: Any, inputs: Inputs, tmp_path: Path) -> tuple[Path, dict[str, Any], Inputs]:
    return assemble_default(run, inputs, tmp_path)


# -- the complete set --------------------------------------------------------------------


def test_a_complete_catalog_set_assembles_checksums_record_and_reports(
    assembled: tuple[Path, dict[str, Any], Inputs],
) -> None:
    out, payload, inputs = assembled
    ids = sorted(inputs.targets)

    expected = (
        {inputs.targets[t]["file"] for t in ids}
        | {f"takaro-{CONNECTOR}-{VERSION}.verify-{t}.json" for t in ids}
        | set(LEGACY_ALIASES)
        | {f"takaro-{CONNECTOR}-{VERSION}.compat.json", "SHA256SUMS"}
    )
    assert {path.name for path in out.iterdir()} == expected

    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["kind"] == "compat-record"
    assert sorted(record["targets"]) == ids
    assert record["source"]["commit"] == inputs.commit
    assert record["source"]["tag"] == f"{CONNECTOR}-v{VERSION}"

    for target_id in ids:
        entry = record["targets"][target_id]
        assert entry["verification"]["executed"] == "protocol"
        assert entry["verification"]["outcome"] == "pass"
        artifact = entry["artifacts"][0]
        assert artifact["url"] == (
            f"https://github.com/{REPO}/releases/download/{CONNECTOR}-v{VERSION}/{artifact['name']}"
        )

    sums = dict(reversed(line.split("  ", 1)) for line in (out / "SHA256SUMS").read_text().splitlines() if line)
    assert set(sums) == expected - {"SHA256SUMS"}
    for name, digest in sums.items():
        assert hashlib.sha256((out / name).read_bytes()).hexdigest() == digest

    assert payload["compatRecord"] == f"takaro-{CONNECTOR}-{VERSION}.compat.json"
    assert {asset["name"] for asset in payload["assets"]} == expected - {
        "SHA256SUMS",
        f"takaro-{CONNECTOR}-{VERSION}.compat.json",
    }


def test_the_record_carries_inputs_source_and_download_links(
    run: Any, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    out, _, inputs = assembled
    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())

    for target_id, entry in record["targets"].items():
        resolved = resolve(run, inputs.root, target_id)
        assert {name: item["url"] for name, item in entry["inputs"].items()} == resolved["resolvedUrls"]
        assert entry["fingerprint"] == resolved["fingerprint"]
        assert entry["runtime"]["digest"] == resolved["runtime"]["container"]["digest"]

    loader = record["targets"]["fabric-26.2"]["inputs"]["loader"]
    assert loader["kind"] == "fabric-launcher"
    assert len(loader["sha256"]) == 64
    assert record["source"]["catalogSha256"] is not None


# -- what blocks a release ---------------------------------------------------------------


def test_an_incomplete_set_exits_seven_and_names_the_missing_target_and_role(
    run: Any, inputs: Inputs, tmp_path: Path
) -> None:
    shutil.rmtree(inputs.dist / "paper-1.21.11")
    out = tmp_path / "assembled"

    code, payload, _ = assemble(run, inputs, out)

    assert code == 7
    assert payload["missing"] == [{"target": "paper-1.21.11", "role": "server-mod"}]
    assert not out.exists() or not any(out.iterdir())


def test_a_stale_fingerprint_exits_seven(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    def stale(document: dict[str, Any]) -> None:
        document["artifacts"][0]["fingerprint"] = "b" * 64

    inputs.rewrite_manifest("fabric-26.2", stale)
    meta = inputs.dist / "fabric-26.2" / f"{inputs.targets['fabric-26.2']['file']}.meta.json"
    meta.write_text(json.dumps({**json.loads(meta.read_text()), "fingerprint": "b" * 64}))

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 7
    assert "stale fingerprint" in payload["error"]


def test_bytes_that_disagree_with_the_manifest_exit_five(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    inputs.jar("fabric-26.2").write_bytes(b"not the jar that was built")

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 5
    assert payload["expected"] == inputs.targets["fabric-26.2"]["sha256"]


@pytest.mark.parametrize(
    ("field", "value"),
    [("connector", "rust"), ("version", "9.9.9"), ("sourceRevision", "c" * 40)],
)
def test_a_manifest_for_another_build_exits_seven(
    run: Any, inputs: Inputs, tmp_path: Path, field: str, value: str
) -> None:
    def mutate(document: dict[str, Any]) -> None:
        document[field] = value

    inputs.rewrite_manifest("fabric-26.2", mutate)

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 7
    assert value in payload["error"]


def test_a_wrong_artifact_file_name_exits_seven(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    directory = inputs.dist / "fabric-26.2"
    original = inputs.targets["fabric-26.2"]["file"]
    renamed = "takaro-minecraft-mod-fabric-26.2-9.9.9.jar"
    (directory / original).rename(directory / renamed)
    (directory / f"{original}.meta.json").rename(directory / f"{renamed}.meta.json")

    def mutate(document: dict[str, Any]) -> None:
        document["artifacts"][0]["file"] = renamed

    inputs.rewrite_manifest("fabric-26.2", mutate)
    meta = directory / f"{renamed}.meta.json"
    meta.write_text(json.dumps({**json.loads(meta.read_text()), "file": renamed}))

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 7
    assert original in payload["error"]


def test_an_unrecognised_dist_directory_exits_seven(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    (inputs.dist / "dist-minecraft-legacy").mkdir()

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 7
    assert payload["directory"] == "dist-minecraft-legacy"


# -- what the evidence has to say --------------------------------------------------------


def test_a_protocol_target_without_a_report_exits_eight(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    shutil.rmtree(inputs.reports / "neoforge-1.21.11")

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 8
    assert payload["target"] == "neoforge-1.21.11"
    assert "evidence missing" in payload["error"]


@pytest.mark.parametrize(("field", "value"), [("outcome", "fail"), ("level", "startup")])
def test_a_failed_or_underlevel_report_exits_eight(
    run: Any, inputs: Inputs, tmp_path: Path, field: str, value: str
) -> None:
    inputs.rewrite_report("paper-1.21.11", **{field: value})

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 8
    assert payload["target"] == "paper-1.21.11"


def test_a_report_for_other_bytes_exits_seven(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    inputs.rewrite_report(
        "fabric-26.2",
        artifacts=[{"role": "server-mod", "file": "x.jar", "sha256": "d" * 64, "connectorVersion": VERSION}],
    )

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 7
    assert "verified bytes are not the published bytes" in payload["error"]


def test_a_report_for_another_revision_exits_seven(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    inputs.rewrite_report("fabric-26.2", source={"repo": REPO, "revision": "e" * 40, "dirty": False})

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled")

    assert code == 7
    assert "e" * 40 in payload["error"]


def test_a_report_from_a_dirty_tree_is_refused_for_stable_and_recorded_with_allow_dirty(
    run: Any, inputs: Inputs, tmp_path: Path
) -> None:
    inputs.rewrite_report("paper-1.21.11", source={"repo": REPO, "revision": inputs.commit, "dirty": True})

    code, payload, _ = assemble(run, inputs, tmp_path / "stable", "--source-commit", inputs.commit)
    assert code == 7
    assert "paper-1.21.11" in payload["error"]
    assert "dirty" in payload["error"]

    out = tmp_path / "rolling"
    code, _, err = assemble(run, inputs, out, "--allow-dirty", "--source-commit", inputs.commit, channel="rolling")
    assert code == 0, err
    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["source"]["dirty"] is True


def test_a_gameplay_requirement_is_not_met_by_a_protocol_report(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    record_path = inputs.root / "catalog" / CONNECTOR / "targets" / "fabric-26.2.json"
    record = json.loads(record_path.read_text())
    record["verification"]["required"] = "gameplay"
    record_path.write_text(json.dumps(record, indent=2))
    # The fingerprint does not cover `verification`, so the built artifacts stay valid.

    code, payload, _ = assemble(
        run, inputs, tmp_path / "assembled", "--allow-dirty", "--source-commit", inputs.commit, channel="pr"
    )

    assert code == 8
    assert "gameplay" in payload["error"]


# -- the source the set is attributed to -------------------------------------------------


def test_a_dirty_tree_is_refused_for_stable_and_recorded_with_allow_dirty(
    run: Any, inputs: Inputs, tmp_path: Path
) -> None:
    (inputs.root / "catalog" / CONNECTOR / "stray.txt").write_text("uncommitted\n")

    refused, payload, _ = assemble(run, inputs, tmp_path / "stable")
    assert refused == 7
    assert "dirty" in payload["error"]

    rejected, usage, _ = assemble(run, inputs, tmp_path / "usage", "--allow-dirty")
    assert rejected == 2
    assert "--allow-dirty" in usage["error"]

    out = tmp_path / "rolling"
    code, _, err = assemble(run, inputs, out, "--allow-dirty", "--source-commit", inputs.commit, channel="rolling")
    assert code == 0, err
    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["source"]["dirty"] is True
    assert record["source"]["tag"] is None


def test_assemble_ignores_the_working_directory_and_ci_variables(inputs: Inputs, tmp_path: Path) -> None:
    out = tmp_path / "assembled"
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "takaro_maint",
            "--repo-root",
            str(inputs.root),
            "release",
            "assemble",
            "--connector",
            CONNECTOR,
            "--version",
            VERSION,
            "--channel",
            "stable",
            "--tag",
            f"{CONNECTOR}-v{VERSION}",
            "--dist",
            str(inputs.dist),
            "--reports",
            str(inputs.reports),
            "--out",
            str(out),
            "--repo",
            REPO,
        ],
        cwd="/",
        env={
            **os.environ,
            "PYTHONPATH": str(REPO_ROOT / "maintenance" / "src"),
            "GITHUB_WORKSPACE": "/nonexistent",
            "GITHUB_SHA": "deadbeef",
            "GITHUB_REPOSITORY": "someone/else",
        },
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["sourceCommit"] == inputs.commit
    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["source"]["repo"] == REPO


# -- legacy connectors -------------------------------------------------------------------

#: A connector the catalog has no record for. Legacy mode is about that absence, not about any
#: one connector, so this deliberately names no real game: every name that is one acquires a
#: catalog record eventually, and the test below would fail on the day it does.
LEGACY_CONNECTOR = "uncatalogued-connector"


def test_legacy_mode_takes_the_given_files_and_writes_a_null_catalog_record(
    run: Any, catalog_copy: Path, tmp_path: Path
) -> None:
    assert not (catalog_copy / "catalog" / LEGACY_CONNECTOR).exists(), (
        f"{LEGACY_CONNECTOR} has a catalog record, so this no longer exercises legacy mode"
    )
    dist = tmp_path / "dist"
    dist.mkdir()
    plugin = dist / f"takaro-{LEGACY_CONNECTOR}-plugin.zip"
    companion = dist / f"takaro-{LEGACY_CONNECTOR}-companion.zip"
    plugin.write_bytes(b"plugin")
    companion.write_bytes(b"companion")
    out = tmp_path / "assembled"

    code, payload, err = run(
        "release",
        "assemble",
        "--connector",
        LEGACY_CONNECTOR,
        "--version",
        "3.0.3",
        "--channel",
        "rolling",
        "--tag",
        f"{LEGACY_CONNECTOR}-dev",
        "--dist",
        str(dist),
        "--out",
        str(out),
        "--repo",
        REPO,
        "--source-commit",
        "f" * 40,
        "--allow-dirty",
        str(plugin),
        str(companion),
        repo=catalog_copy,
    )

    assert code == 0, err
    assert payload["mode"] == "legacy"
    record = json.loads((out / f"takaro-{LEGACY_CONNECTOR}-3.0.3.compat.json").read_text())
    assert record["catalog"] is None
    assert record["targets"] == {}
    assert record["aliases"] == {}
    assert {asset["kind"] for asset in record["assets"]} == {"artifact"}
    assert sorted(path.name for path in out.iterdir()) == [
        "SHA256SUMS",
        f"takaro-{LEGACY_CONNECTOR}-3.0.3.compat.json",
        f"takaro-{LEGACY_CONNECTOR}-companion.zip",
        f"takaro-{LEGACY_CONNECTOR}-plugin.zip",
    ]


def test_legacy_aliases_are_byte_identical_copies_of_their_target(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    game_file = inputs.root / "catalog" / CONNECTOR / "game.json"
    game = json.loads(game_file.read_text())
    game["legacyAssetAliases"] = {
        "takaro-fabric-{version}.jar": "fabric-26.2/server-mod",
        "takaro-gone-{version}.jar": "fabric-0.0.0/server-mod",
    }
    game_file.write_text(json.dumps(game, indent=2))
    out = tmp_path / "assembled"

    code, payload, err = assemble(run, inputs, out, "--source-commit", inputs.commit, "--allow-dirty", channel="pr")

    assert code == 0, err
    alias = out / f"takaro-fabric-{VERSION}.jar"
    assert alias.read_bytes() == (out / inputs.targets["fabric-26.2"]["file"]).read_bytes()
    assert payload["aliases"][alias.name]["of"] == inputs.targets["fabric-26.2"]["file"]
    assert payload["aliasesSkipped"] == ["takaro-gone-{version}.jar"]

    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["aliases"][alias.name]["target"] == "fabric-26.2"
    assert {a["name"] for a in record["assets"] if a["kind"] == "alias"} == {alias.name}


# -- the same inputs assemble to the same bytes ------------------------------------------


def test_two_assemblies_of_the_same_inputs_are_byte_identical(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    """Without this, a stable retry has nothing to recognise: every rerun would conflict."""
    first, second = tmp_path / "first", tmp_path / "second"

    assert assemble(run, inputs, first)[0] == 0
    assert assemble(run, inputs, second)[0] == 0

    left = {path.name: path.read_bytes() for path in first.iterdir()}
    right = {path.name: path.read_bytes() for path in second.iterdir()}
    assert left == right
    record = json.loads((first / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["generatedAt"].endswith("Z")


def test_source_date_epoch_pins_the_record_stamp(run: Any, inputs: Inputs, tmp_path: Path, monkeypatch: Any) -> None:
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "1700000000")
    out = tmp_path / "assembled"

    assert assemble(run, inputs, out)[0] == 0

    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["generatedAt"] == "2023-11-14T22:13:20Z"


# -- provenance the build recorded about itself ------------------------------------------


def test_a_build_made_from_a_dirty_tree_is_refused_and_recorded(run: Any, inputs: Inputs, tmp_path: Path) -> None:
    def dirty(document: dict[str, Any]) -> None:
        document["dirty"] = True

    inputs.rewrite_manifest("fabric-26.2", dirty)

    code, payload, _ = assemble(run, inputs, tmp_path / "stable")
    assert code == 7
    assert payload["directory"] == "fabric-26.2"
    assert "dirty tree" in payload["error"]

    out = tmp_path / "rolling"
    code, _, err = assemble(run, inputs, out, "--allow-dirty", channel="rolling")
    assert code == 0, err
    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    assert record["source"]["dirty"] is True


# -- one name, one file ------------------------------------------------------------------


@pytest.mark.parametrize("colliding", ["artifact", "SHA256SUMS"])
def test_an_alias_that_collides_with_another_asset_exits_seven(
    run: Any, inputs: Inputs, tmp_path: Path, colliding: str
) -> None:
    name = inputs.targets["fabric-26.2"]["file"] if colliding == "artifact" else "SHA256SUMS"
    _set_aliases(inputs, {name: "paper-1.21.11/server-mod"})

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled", "--allow-dirty", channel="pr")

    assert code == 7
    assert payload["asset"] == name
    assert "one file per name" in payload["error"]


@pytest.mark.parametrize("pattern", ["../escaped-{version}.jar", "/tmp/absolute-{version}.jar"])
def test_an_alias_naming_a_path_is_refused(run: Any, inputs: Inputs, tmp_path: Path, pattern: str) -> None:
    _set_aliases(inputs, {pattern: "fabric-26.2/server-mod"})

    code, payload, _ = assemble(run, inputs, tmp_path / "assembled", "--allow-dirty", channel="pr")

    assert code == 2
    assert payload["alias"] == pattern
    assert not (tmp_path / f"escaped-{VERSION}.jar").exists()


def _set_aliases(inputs: Inputs, aliases: dict[str, str]) -> None:
    game_file = inputs.root / "catalog" / CONNECTOR / "game.json"
    game = json.loads(game_file.read_text())
    game["legacyAssetAliases"] = aliases
    game_file.write_text(json.dumps(game, indent=2))


def test_each_platforms_default_target_ships_under_its_legacy_asset_name(
    assembled: tuple[Path, dict[str, Any], Inputs],
) -> None:
    """Existing download links name the platform, not the server build; they keep resolving."""
    out, payload, inputs = assembled
    record = json.loads((out / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())

    assert payload["aliasesSkipped"] == []
    assert set(payload["aliases"]) == set(LEGACY_ALIASES)
    for alias_name, target_id in LEGACY_ALIASES.items():
        source = inputs.targets[target_id]["file"]
        assert (out / alias_name).read_bytes() == (out / source).read_bytes()
        assert payload["aliases"][alias_name]["of"] == source
        assert record["aliases"][alias_name]["target"] == target_id
