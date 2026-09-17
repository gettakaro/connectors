"""The NeoForge target: its record, its sidecar-first acquisition, and every way it refuses."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from conftest import FIXTURES, Wired, make_jar, point_at, read_target, sha256, write_target
from test_cli_install import tree

TARGET = "neoforge-1.21.11"
INSTALLER = FIXTURES / "upstream/neoforge/21.11.45/neoforge-21.11.45-installer.jar"
UNIVERSAL = FIXTURES / "upstream/neoforge/21.11.45/neoforge-21.11.45-universal.jar"


@pytest.fixture
def wired_neoforge(catalog_copy: Path) -> Any:
    """Serve tiny stand-ins plus their .sha256 sidecars, and re-pin the catalog copy to them."""
    from fake_upstream import FakeUpstream

    with FakeUpstream() as upstream:
        record = read_target(catalog_copy, TARGET)
        for name, fixture in (("loader", INSTALLER), ("universal", UNIVERSAL)):
            payload = fixture.read_bytes()
            digest = sha256(payload)
            spec = record["inputs"][name]
            spec["sha256"] = digest
            spec["size"] = len(payload)
            upstream.add(spec["path"], payload)
            upstream.add(spec["path"] + ".sha256", (digest + "\n").encode("ascii"))
        write_target(catalog_copy, record, TARGET)
        point_at(catalog_copy, upstream.base_url)
        yield Wired(catalog_copy, upstream)


def solo(root: Path) -> None:
    """Drop every target but this one, so a whole-catalog check only needs this upstream."""
    for path in (root / "catalog/minecraft/targets").glob("*.json"):
        if path.stem != TARGET:
            path.unlink()


def install(run: Any, wired: Any, dest: Path, *extra: str) -> tuple[int, Any, str]:
    return run("install", "--game", "minecraft", "--target", TARGET, "--dest", str(dest), *extra, repo=wired.root)


def paths_of(wired: Any) -> dict[str, str]:
    inputs = read_target(wired.root, TARGET)["inputs"]
    return {name: str(inputs[name]["path"]) for name in ("loader", "universal")}


# --- the record itself ------------------------------------------------------------------


def test_the_real_record_is_internally_consistent(repo_root: Path) -> None:
    text = (repo_root / "catalog/minecraft/targets" / f"{TARGET}.json").read_text()
    record = json.loads(text)
    loader = record["inputs"]["loader"]
    env = record["runtime"]["container"]["env"]

    assert record["default"] is True
    assert record["platform"] == "neoforge"
    assert record["revision"] == loader["gameVersion"] == "1.21.11"
    assert loader["loaderVersion"] == env["NEOFORGE_VERSION"] == "21.11.45"
    assert env["NEOFORGE_INSTALLER"] == "/data/" + loader["installPath"]
    assert record["inputs"]["universal"]["installPath"].startswith("libraries/net/neoforged/neoforge/21.11.45/")
    assert record["build"]["plugins"]["neoforge-moddev"] == "2.0.141"
    assert record["runtime"]["java"] == record["build"]["javaRelease"] == 21
    assert record["components"][0]["installDir"] == "mods"
    assert record["devServers"]["gameId"] == "minecraft-neoforge"
    assert "beta" not in text, "the rig's floating NeoForge alias must not survive in the record"


def test_the_repository_catalog_declares_one_default_per_platform(run: Any, repo_root: Path) -> None:
    code, payload, _ = run("targets", "list", "--game", "minecraft", repo=repo_root)

    assert code == 0, payload
    defaults: dict[str, list[str]] = {}
    for row in payload["targets"]:
        _, resolved, _ = run("targets", "resolve", "--game", "minecraft", "--platform", row["platform"], repo=repo_root)
        defaults.setdefault(row["platform"], []).append(resolved["id"])
    assert set(defaults) == {"fabric", "paper", "neoforge"}
    assert defaults["neoforge"] == [TARGET] * len(defaults["neoforge"])
    assert defaults["paper"] == ["paper-1.21.11"] * len(defaults["paper"])


# --- resolution -------------------------------------------------------------------------


def test_resolve_prints_the_neoforge_env_keys(run: Any, repo_root: Path) -> None:
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
        "MC_NEOFORGE",
        repo=repo_root,
    )

    assert code == 0, text
    env = dict(line.split("=", 1) for line in str(text).strip().splitlines())
    assert set(env) == {
        "MC_NEOFORGE_TARGET",
        "MC_NEOFORGE_FINGERPRINT",
        "MC_NEOFORGE_FP16",
        "MC_NEOFORGE_IMAGE",
        "MC_NEOFORGE_JAVA",
        "MC_NEOFORGE_VERSION",
        "MC_NEOFORGE_LOADER_VERSION",
        "MC_NEOFORGE_INSTALLER",
    }
    assert env["MC_NEOFORGE_TARGET"] == TARGET
    assert env["MC_NEOFORGE_LOADER_VERSION"] == "21.11.45"
    assert env["MC_NEOFORGE_VERSION"] == "1.21.11"
    assert env["MC_NEOFORGE_JAVA"] == "21"
    assert env["MC_NEOFORGE_INSTALLER"] == "/data/neoforge-21.11.45-installer.jar"
    assert env["MC_NEOFORGE_IMAGE"].startswith("itzg/minecraft-server:2026.9.1-java21@sha256:")


def test_the_rig_game_selects_this_target(run: Any, repo_root: Path) -> None:
    code, payload, _ = run("targets", "list", "--game", "minecraft", "--rig-game", "minecraft-neoforge", repo=repo_root)

    assert code == 0, payload
    assert [row["id"] for row in payload["targets"]] == [TARGET]


# --- install ----------------------------------------------------------------------------


def test_a_fresh_install_places_installer_and_universal_jar(run: Any, wired_neoforge: Any, tmp_path: Path) -> None:
    dest = tmp_path / "server"

    code, payload, _ = install(run, wired_neoforge, dest)

    assert code == 0, payload
    assert (dest / "neoforge-21.11.45-installer.jar").is_file()
    assert (dest / "libraries/net/neoforged/neoforge/21.11.45/neoforge-21.11.45-universal.jar").is_file()
    ledger = json.loads((dest / ".takaro/installed-target.json").read_text())
    assert [entry["name"] for entry in ledger["inputs"]] == ["loader", "universal"]

    # Each jar's sidecar is the gate, so it is always asked for before the jar itself.
    requested = wired_neoforge.upstream.requested
    for path in paths_of(wired_neoforge).values():
        assert requested.index(path + ".sha256") < requested.index(path)


def test_a_sidecar_that_disagrees_with_the_catalog_exits_five_before_the_jar_is_fetched(
    run: Any, wired_neoforge: Any, tmp_path: Path
) -> None:
    installer = paths_of(wired_neoforge)["loader"]
    wired_neoforge.upstream.files[installer + ".sha256"] = (("c" * 64) + "\n").encode("ascii")

    code, payload, _ = install(run, wired_neoforge, tmp_path / "server")

    assert code == 5, payload
    assert installer not in wired_neoforge.upstream.requested
    assert installer + ".sha256" in payload["error"]
    assert payload["sidecar"] == "c" * 64


def test_a_missing_sidecar_exits_four_without_fetching_the_jar(run: Any, wired_neoforge: Any, tmp_path: Path) -> None:
    installer = paths_of(wired_neoforge)["loader"]
    wired_neoforge.upstream.status_overrides[installer + ".sha256"] = 404

    code, payload, _ = install(run, wired_neoforge, tmp_path / "server")

    assert code == 4, payload
    assert wired_neoforge.upstream.requested.count(installer + ".sha256") == 1
    assert installer not in wired_neoforge.upstream.requested


def test_a_missing_jar_exits_four_with_no_fallback(run: Any, wired_neoforge: Any, tmp_path: Path) -> None:
    installer = paths_of(wired_neoforge)["loader"]
    wired_neoforge.upstream.status_overrides[installer] = 503

    code, payload, _ = install(run, wired_neoforge, tmp_path / "server")

    assert code == 4, payload
    assert wired_neoforge.upstream.requested.count(installer) == 1


def test_a_malformed_sidecar_exits_four(run: Any, wired_neoforge: Any, tmp_path: Path) -> None:
    installer = paths_of(wired_neoforge)["loader"]
    wired_neoforge.upstream.files[installer + ".sha256"] = b"<html>404 not found</html>\n"

    code, payload, _ = install(run, wired_neoforge, tmp_path / "server")

    assert code == 4, payload
    assert "is not a sha256 sidecar" in payload["error"]
    assert installer not in wired_neoforge.upstream.requested


def test_altered_jar_bytes_with_a_matching_sidecar_exit_five(
    run: Any, wired_neoforge: Any, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    dest = tmp_path / "server"
    assert install(run, wired_neoforge, dest)[0] == 0
    universal = "libraries/net/neoforged/neoforge/21.11.45/neoforge-21.11.45-universal.jar"

    # The local installer is damaged, so the ledger is stale and the next install really
    # re-downloads instead of taking the already-installed fast path. A fresh cache makes
    # the download real, and the sidecar still says what the catalog says.
    (dest / "neoforge-21.11.45-installer.jar").write_bytes(b"a damaged local copy")
    before = tree(dest)
    monkeypatch.setenv("TAKARO_MAINT_CACHE", str(tmp_path / "cache2"))
    wired_neoforge.upstream.files[paths_of(wired_neoforge)["loader"]] = b"not the installer upstream promised"

    code, payload, _ = install(run, wired_neoforge, dest)

    assert code == 5, payload
    assert tree(dest) == before, "a failed install must not touch the universal jar or the ledger"
    assert (dest / universal).is_file()
    assert not (dest / ".takaro/staging").exists()


def test_a_failed_universal_download_leaves_an_existing_install_byte_identical(
    run: Any, wired_neoforge: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "server"
    assert install(run, wired_neoforge, dest)[0] == 0
    before = tree(dest)

    universal = paths_of(wired_neoforge)["universal"]
    record = read_target(wired_neoforge.root, TARGET)
    record["inputs"]["universal"]["sha256"] = "d" * 64
    write_target(wired_neoforge.root, record, TARGET)
    wired_neoforge.upstream.status_overrides[universal] = 500

    code, payload, _ = install(run, wired_neoforge, dest)

    assert code != 0, payload
    assert tree(dest) == before
    assert not (dest / ".takaro/staging").exists()


def test_ledger_check_rehashes_the_universal_jar(run: Any, wired_neoforge: Any, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, wired_neoforge, dest)[0] == 0
    relative = "libraries/net/neoforged/neoforge/21.11.45/neoforge-21.11.45-universal.jar"

    code, payload, _ = run(
        "ledger", "check", "--game", "minecraft", "--target", TARGET, "--dest", str(dest), repo=wired_neoforge.root
    )
    assert code == 0, payload

    (dest / relative).write_bytes(b"tampered")
    code, payload, _ = run(
        "ledger", "check", "--game", "minecraft", "--target", TARGET, "--dest", str(dest), repo=wired_neoforge.root
    )

    assert code == 7, payload
    assert any(relative in reason for reason in payload["reasons"])


# --- wrong artifact ---------------------------------------------------------------------


def test_artifact_validate_rejects_the_paper_jar_for_the_neoforge_target(
    run: Any, repo_root: Path, tmp_path: Path
) -> None:
    _, other, _ = run("targets", "resolve", "--game", "minecraft", "--target", "paper-1.21.11", repo=repo_root)
    jar = make_jar(
        tmp_path / "takaro-minecraft-mod-paper-1.21.11-0.1.1.jar",
        target="paper-1.21.11",
        fingerprint=other["fingerprint"],
        revision="1.21.11",
    )

    code, payload, _ = run("artifact", "validate", "--game", "minecraft", "--target", TARGET, str(jar), repo=repo_root)

    assert code == 7, payload
    problems = " ".join(payload["files"][0]["problems"])
    assert "Takaro-Target paper-1.21.11 != neoforge-1.21.11" in problems


# --- log parsing ------------------------------------------------------------------------


def test_parse_runtime_identity_reads_the_neoforge_banner_and_the_target_check_line() -> None:
    from takaro_maint.games.minecraft import neoforge

    # Copied verbatim from a NeoForge 21.11.45 boot in the pinned java21 image.
    banner = (
        "[19:43:06] [modloading-worker-0/INFO] [ne.ne.ne.co.NeoForgeMod/NEOFORGE-MOD]: "
        "NeoForge mod loading, version 21.11.45, for MC 1.21.11"
    )
    assert neoforge.parse_runtime_identity(banner) == {
        "gameVersion": "1.21.11",
        "loader": "neoforge",
        "loaderVersion": "21.11.45",
    }

    check = '[21:04:20] [Server thread/INFO]: Takaro target-check: {"target":"neoforge-1.21.11","result":"ok"}'
    assert neoforge.parse_runtime_identity(check) == {"targetCheck": {"target": "neoforge-1.21.11", "result": "ok"}}

    assert neoforge.parse_runtime_identity("[19:42:52] [main/INFO]: Starting FancyModLoader version 10.0.36") is None
    # The image helper's installer line carries both versions but describes the install, not
    # the running server, so it must not be read as a runtime identity.
    assert (
        neoforge.parse_runtime_identity(
            "[mc-image-helper] INFO : Running NeoForge 21.11.45 installer for Minecraft 1.21.11."
        )
        is None
    )


def test_the_neoforge_env_values_are_all_strings(run: Any, repo_root: Path) -> None:
    from takaro_maint.games.minecraft import neoforge

    code, resolved, _ = run("targets", "resolve", "--game", "minecraft", "--target", TARGET, repo=repo_root)

    assert code == 0, resolved
    assert all(isinstance(value, str) for value in neoforge.env(resolved, "MC_NEOFORGE").values())
    assert neoforge.runtime_env(resolved)["TYPE"] == "NEOFORGE"


# --- online validation ------------------------------------------------------------------


def test_online_validation_matches_the_fake_upstream(run: Any, wired_neoforge: Any) -> None:
    solo(wired_neoforge.root)

    code, payload, _ = run("catalog", "validate", "--online", repo=wired_neoforge.root)
    assert code == 0, payload
    online = [check for check in payload["checks"] if check["id"] == "online-hash"]
    assert len(online) == 2
    assert all(check["status"] == "pass" for check in online)

    wired_neoforge.upstream.files[paths_of(wired_neoforge)["universal"]] = b"different bytes"

    code, payload, _ = run("catalog", "validate", "--online", repo=wired_neoforge.root)
    assert code == 5, payload
    assert any(check["id"] == "online-hash" and check["status"] == "fail" for check in payload["checks"])
