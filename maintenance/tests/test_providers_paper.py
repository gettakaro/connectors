"""The Paper target: its record, its acquisition path, and every way it must refuse."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from conftest import FIXTURES, Wired, make_jar, point_at, read_target, sha256, write_target
from test_cli_install import tree

TARGET = "paper-1.21.11"
JAR = FIXTURES / "upstream/paper/1.21.11/paper-1.21.11-132.jar"


@pytest.fixture
def wired_paper(catalog_copy: Path) -> Any:
    """Serve a tiny stand-in for the one Paper download and re-pin the catalog copy to it."""
    from fake_upstream import FakeUpstream

    with FakeUpstream() as upstream:
        record = read_target(catalog_copy, TARGET)
        payload = JAR.read_bytes()
        digest = sha256(payload)
        loader = record["inputs"]["loader"]
        # The URL is content-addressed, so re-pinning the hash means rewriting the path too.
        loader["path"] = f"/v1/objects/{digest}/paper-1.21.11-132.jar"
        loader["sha256"] = digest
        loader["size"] = len(payload)
        write_target(catalog_copy, record, TARGET)
        point_at(catalog_copy, upstream.base_url)
        upstream.add(loader["path"], payload)
        yield Wired(catalog_copy, upstream)


def solo(root: Path) -> None:
    """Drop every target but this one, so a whole-catalog check only needs this upstream."""
    for path in (root / "catalog/minecraft/targets").glob("*.json"):
        if path.stem != TARGET:
            path.unlink()


def install(run: Any, wired: Any, dest: Path, *extra: str) -> tuple[int, Any, str]:
    return run("install", "--game", "minecraft", "--target", TARGET, "--dest", str(dest), *extra, repo=wired.root)


def jar_path(wired: Any) -> str:
    return str(read_target(wired.root, TARGET)["inputs"]["loader"]["path"])


# --- the record itself ------------------------------------------------------------------


def test_the_real_record_is_internally_consistent(repo_root: Path) -> None:
    record = json.loads((repo_root / "catalog/minecraft/targets" / f"{TARGET}.json").read_text())
    loader = record["inputs"]["loader"]
    env = record["runtime"]["container"]["env"]

    assert record["default"] is True
    assert record["platform"] == "paper"
    assert record["revision"] == loader["gameVersion"] == "1.21.11"
    assert loader["loaderVersion"] == env["PAPER_BUILD"] == "132"
    assert loader["path"].split("/")[3] == loader["sha256"], "the URL must address the bytes it pins"
    assert env["PAPER_CUSTOM_JAR"] == "/data/" + loader["installPath"]
    assert record["runtime"]["java"] == record["build"]["javaRelease"] == 21
    assert record["build"]["javaRelease"] <= record["build"]["gradleJvm"]
    assert record["components"][0]["installDir"] == "plugins"
    assert record["devServers"]["gameId"] == "minecraft-paper"


def test_the_repository_catalog_validates_with_paper_in_it(run: Any, repo_root: Path) -> None:
    code, payload, _ = run("catalog", "validate", repo=repo_root)

    assert code == 0, payload
    files = {check["file"] for check in payload["checks"]}
    assert f"catalog/minecraft/targets/{TARGET}.json" in files
    assert [check for check in payload["checks"] if check["status"] != "pass"] == []


# --- resolution -------------------------------------------------------------------------


def test_resolve_prints_the_paper_env_keys(run: Any, repo_root: Path) -> None:
    code, text, _ = run(
        "targets",
        "resolve",
        "--game",
        "minecraft",
        "--target",
        TARGET,
        "--format",
        "env",
        "--prefix",
        "MC_PAPER",
        repo=repo_root,
    )

    assert code == 0, text
    env = dict(line.split("=", 1) for line in str(text).strip().splitlines())
    assert set(env) == {
        "MC_PAPER_TARGET",
        "MC_PAPER_FINGERPRINT",
        "MC_PAPER_FP16",
        "MC_PAPER_IMAGE",
        "MC_PAPER_JAVA",
        "MC_PAPER_VERSION",
        "MC_PAPER_BUILD",
        "MC_PAPER_CUSTOM_JAR",
    }
    assert env["MC_PAPER_TARGET"] == TARGET
    assert env["MC_PAPER_BUILD"] == "132"
    assert env["MC_PAPER_VERSION"] == "1.21.11"
    assert env["MC_PAPER_JAVA"] == "21"
    assert env["MC_PAPER_CUSTOM_JAR"] == "/data/paper-1.21.11-132.jar"
    assert env["MC_PAPER_IMAGE"].startswith("itzg/minecraft-server:2026.9.1-java21@sha256:")


def test_default_selection_by_platform(run: Any, repo_root: Path) -> None:
    code, payload, _ = run("targets", "resolve", "--game", "minecraft", "--platform", "paper", repo=repo_root)

    assert code == 0, payload
    assert payload["id"] == TARGET


def test_the_rig_game_selects_this_target(run: Any, repo_root: Path) -> None:
    code, payload, _ = run("targets", "list", "--game", "minecraft", "--rig-game", "minecraft-paper", repo=repo_root)

    assert code == 0, payload
    assert [row["id"] for row in payload["targets"]] == [TARGET]


# --- install ----------------------------------------------------------------------------


def test_a_fresh_install_places_the_paper_jar_and_writes_the_ledger(run: Any, wired_paper: Any, tmp_path: Path) -> None:
    dest = tmp_path / "server"

    code, payload, _ = install(run, wired_paper, dest)

    assert code == 0, payload
    assert payload["status"] == "installed"
    assert (dest / "paper-1.21.11-132.jar").is_file()
    ledger = json.loads((dest / ".takaro/installed-target.json").read_text())
    assert ledger["target"] == TARGET
    assert ledger["world"]["revision"] == "1.21.11"
    assert [entry["name"] for entry in ledger["inputs"]] == ["loader"]
    assert ledger["inputs"][0]["path"] == "paper-1.21.11-132.jar"
    assert ledger["inputs"][0]["sha256"] == sha256(JAR.read_bytes())
    assert wired_paper.upstream.requested.count(jar_path(wired_paper)) == 1


def test_a_404_exits_four_with_one_request_and_no_fallback(run: Any, wired_paper: Any, tmp_path: Path) -> None:
    path = jar_path(wired_paper)
    wired_paper.upstream.status_overrides[path] = 404

    code, payload, _ = install(run, wired_paper, tmp_path / "server")

    assert code == 4, payload
    assert wired_paper.upstream.requested.count(path) == 1
    assert [p for p in wired_paper.upstream.requested if "paper-" in p and p != path] == []


def test_altered_bytes_exit_five_and_leave_the_install_untouched(
    run: Any, wired_paper: Any, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    dest = tmp_path / "server"
    assert install(run, wired_paper, dest)[0] == 0

    # The local jar is damaged, so the ledger is stale and the next install really
    # re-downloads instead of taking the already-installed fast path. A fresh cache makes
    # the download real, and upstream now answers with different bytes at the same address.
    (dest / "paper-1.21.11-132.jar").write_bytes(b"a damaged local copy")
    before = tree(dest)
    monkeypatch.setenv("TAKARO_MAINT_CACHE", str(tmp_path / "cache2"))
    wired_paper.upstream.files[jar_path(wired_paper)] = b"not the jar upstream promised"

    code, payload, _ = install(run, wired_paper, dest)

    assert code == 5, payload
    assert tree(dest) == before
    assert not (dest / ".takaro/staging").exists()


def test_a_record_whose_url_contradicts_its_hash_is_refused_before_download(
    run: Any, wired_paper: Any, tmp_path: Path
) -> None:
    record = read_target(wired_paper.root, TARGET)
    record["inputs"]["loader"]["path"] = "/v1/objects/" + "a" * 64 + "/paper-1.21.11-132.jar"
    write_target(wired_paper.root, record, TARGET)

    code, payload, _ = install(run, wired_paper, tmp_path / "server")

    assert code == 2, payload
    assert wired_paper.upstream.requested == []
    assert "contradicts itself" in payload["error"]


def test_ledger_check_rehashes_the_paper_jar(run: Any, wired_paper: Any, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, wired_paper, dest)[0] == 0

    code, payload, _ = run(
        "ledger", "check", "--game", "minecraft", "--target", TARGET, "--dest", str(dest), repo=wired_paper.root
    )
    assert code == 0, payload

    (dest / "paper-1.21.11-132.jar").write_bytes(b"tampered")
    code, payload, _ = run(
        "ledger", "check", "--game", "minecraft", "--target", TARGET, "--dest", str(dest), repo=wired_paper.root
    )

    assert code == 7, payload
    assert any("paper-1.21.11-132.jar" in reason for reason in payload["reasons"])


# --- wrong artifact ---------------------------------------------------------------------


def test_artifact_validate_rejects_the_neoforge_jar_for_the_paper_target(
    run: Any, repo_root: Path, tmp_path: Path
) -> None:
    _, other, _ = run("targets", "resolve", "--game", "minecraft", "--target", "neoforge-1.21.11", repo=repo_root)
    jar = make_jar(
        tmp_path / "takaro-minecraft-mod-neoforge-1.21.11-0.1.1.jar",
        target="neoforge-1.21.11",
        fingerprint=other["fingerprint"],
        revision="1.21.11",
    )

    code, payload, _ = run("artifact", "validate", "--game", "minecraft", "--target", TARGET, str(jar), repo=repo_root)

    assert code == 7, payload
    problems = " ".join(payload["files"][0]["problems"])
    assert "Takaro-Target neoforge-1.21.11 != paper-1.21.11" in problems


def test_deploy_rejects_a_manifest_built_for_another_target(run: Any, wired_paper: Any, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, wired_paper, dest)[0] == 0

    out = tmp_path / "dist"
    out.mkdir()
    file_name = "takaro-minecraft-mod-neoforge-1.21.11-0.1.1.jar"
    jar = make_jar(out / file_name, target="neoforge-1.21.11", fingerprint="b" * 64, revision="1.21.11")
    (out / "build-manifest.json").write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "connector": "minecraft",
                "version": "0.1.1",
                "sourceRevision": "deadbeef",
                "dirty": False,
                "builtAt": "2026-09-17T00:00:00Z",
                "toolchain": {
                    "image": "eclipse-temurin",
                    "tag": "25-jdk",
                    "digest": "sha256:" + "0" * 64,
                    "mode": "container",
                },
                "artifacts": [
                    {
                        "role": "server-mod",
                        "target": "neoforge-1.21.11",
                        "fingerprint": "b" * 64,
                        "file": file_name,
                        "sha256": sha256(jar.read_bytes()),
                        "size": jar.stat().st_size,
                    }
                ],
            },
            indent=2,
        )
    )

    code, payload, _ = run(
        "deploy",
        "--game",
        "minecraft",
        "--target",
        TARGET,
        "--dest",
        str(dest),
        "--from",
        str(out / "build-manifest.json"),
        repo=wired_paper.root,
    )

    assert code == 7, payload
    assert not (dest / "plugins").is_dir() or list((dest / "plugins").glob("*")) == []


# --- log parsing ------------------------------------------------------------------------


def test_parse_runtime_identity_reads_the_paper_banner_and_the_target_check_line() -> None:
    from takaro_maint.games.minecraft import paper

    # Copied verbatim from a Paper 1.21.11 build 132 boot in the pinned java21 image.
    banner = (
        "[19:41:27 INFO]: This server is running Paper version 1.21.11-132-ver/1.21.11@c5eb079 "
        "(2026-05-11T11:43:09Z) (Implementing API version 1.21.11-R0.1-SNAPSHOT)"
    )
    assert paper.parse_runtime_identity(banner) == {
        "gameVersion": "1.21.11",
        "loader": "paper",
        "loaderVersion": "132",
    }

    check = '[21:04:20 INFO]: Takaro target-check: {"target":"paper-1.21.11","result":"ok"}'
    assert paper.parse_runtime_identity(check) == {"targetCheck": {"target": "paper-1.21.11", "result": "ok"}}

    assert paper.parse_runtime_identity('[21:04:11 INFO]: Preparing level "world"') is None


def test_the_paper_env_values_are_all_strings(run: Any, repo_root: Path) -> None:
    from takaro_maint.games.minecraft import paper

    code, resolved, _ = run("targets", "resolve", "--game", "minecraft", "--target", TARGET, repo=repo_root)

    assert code == 0, resolved
    assert all(isinstance(value, str) for value in paper.env(resolved, "MC_PAPER").values())
    assert paper.runtime_env(resolved)["TYPE"] == "PAPER"


# --- online validation ------------------------------------------------------------------


def test_online_validation_matches_the_fake_upstream(run: Any, wired_paper: Any) -> None:
    solo(wired_paper.root)

    code, payload, _ = run("catalog", "validate", "--online", repo=wired_paper.root)
    assert code == 0, payload
    assert any(check["id"] == "online-hash" and check["status"] == "pass" for check in payload["checks"])

    wired_paper.upstream.files[jar_path(wired_paper)] = b"different bytes at the same address"

    code, payload, _ = run("catalog", "validate", "--online", repo=wired_paper.root)
    assert code == 5, payload
    assert any(check["id"] == "online-hash" and check["status"] == "fail" for check in payload["checks"])
