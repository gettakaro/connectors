"""The RuneScape: Dragonwilds targets: catalog, resolution, artifact naming, deploy.

Unlike Dune: Awakening, the server is a plain Steam depot install -- one binary, its
``.sym`` file, a launcher script -- so the adapter owns a real ``install`` through the
shared Steam exact-install path, the way Conan Exiles does. One artifact ships: the native
plugin holds the Takaro connection itself, so there is no sidecar role anywhere. There is no
runtime container for this game yet (the operator supplies their own dedicated-server
image), so every target claims ``contract`` and the workflow passes ``runtime: false``,
which is asserted here, because those two are the whole honesty of this connector's
release at this stage.
"""

from __future__ import annotations

import io
import json
import tarfile
from pathlib import Path
from typing import Any

import pytest

from takaro_maint.exit_codes import ConflictError
from takaro_maint.games import adapter_for

GAME = "dragonwilds"
TARGET = "linux-25630937"
PREVIOUS = "linux-25501739"
OLDEST = "linux-25465077"
APP = 4019830
DEPOT = "3501791"
MANIFEST = "5180331908424149228"
PREVIOUS_MANIFEST = "6714393990492196440"
OLDEST_MANIFEST = "2601451637939157694"
VERSION = "0.3.0-dev.abc1234"
PLUGIN_ARTIFACT = f"takaro-dragonwilds-plugin-{TARGET}-{VERSION}.tar.gz"
PLUGIN_FOLDER = "TakaroDragonwilds"
NATIVE_DEPS = {"openssl", "libwebsockets", "pcre2", "nlohmann-json"}

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
PREVIOUS_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{PREVIOUS}.json"
OLDEST_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{OLDEST}.json"
GAME_PATH = REPO_ROOT / "catalog" / GAME / "game.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "dragonwilds.yml"


def record(path: Path = TARGET_PATH) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any, target: str = TARGET) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", target, "--prefix", "DRAGONWILDS")
    assert code == 0, err
    return dict(payload)


# -- catalog -----------------------------------------------------------------------------


def test_catalog_validate_accepts_the_dragonwilds_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    for target in (TARGET, PREVIOUS, OLDEST):
        rows = [check for check in payload["checks"] if check["file"].endswith(f"{target}.json")]
        assert rows, f"the dragonwilds target {target} produced no checks"
        assert {check["id"] for check in rows} >= {
            "input-kind-schema",
            "build-system-schema",
            "build-script-exists",
            "no-null-hash",
            "immutable-tag-and-digest",
        }
        assert all(check["status"] == "pass" for check in rows), rows


@pytest.mark.parametrize(
    ("path", "buildid", "manifest"),
    [
        (TARGET_PATH, 25630937, MANIFEST),
        (PREVIOUS_PATH, 25501739, PREVIOUS_MANIFEST),
        (OLDEST_PATH, 25465077, OLDEST_MANIFEST),
    ],
)
def test_the_steam_pin_is_exact_and_anonymous(path: Path, buildid: int, manifest: str) -> None:
    server = record(path)["inputs"]["server"]

    assert (server["app"], server["branch"], server["buildid"]) == (APP, "public", buildid)
    assert server["depots"][DEPOT]["manifest"] == manifest
    assert server["credentials"] is None
    assert all(declared["sha256"] for declared in server["files"].values())
    assert "RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping" in server["files"]
    assert "RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping.sym" in server["files"]


def test_the_newest_build_is_the_one_default() -> None:
    assert record()["default"] is True
    assert record(PREVIOUS_PATH)["default"] is False
    assert record(OLDEST_PATH)["default"] is False


@pytest.mark.parametrize("path", [TARGET_PATH, PREVIOUS_PATH, OLDEST_PATH])
def test_every_target_ships_the_plugin_alone(path: Path) -> None:
    document = record(path)

    # The plugin holds the Takaro connection: no sidecar component, folder or npm pin is left.
    assert [component["role"] for component in document["components"]] == ["plugin"]
    assert set(document["build"]["deps"]) == NATIVE_DEPS
    assert all(len(dep["sha256"]) == 64 for dep in document["build"]["deps"].values())
    assert not [entry for entry in document["preserve"] if "Sidecar" in entry]


@pytest.mark.parametrize("path", [TARGET_PATH, PREVIOUS_PATH, OLDEST_PATH])
def test_the_target_claims_contract_verification_and_a_steam_install(path: Path) -> None:
    document = record(path)

    # No runtime image exists for this game yet, so no level above contract may be claimed
    # as required -- the same posture Dune: Awakening and Conan Exiles took at this stage.
    assert document["verification"]["required"] == "contract"
    assert "startup" in document["verification"]["separate"]
    # The current target is promoted on its live proof; the older pins stay candidates.
    expected_status = "maintained" if path == TARGET_PATH else "candidate"
    assert document["support"]["status"] == expected_status
    # Unlike Dune, the server is a plain depot: a rig ledger is meaningful here.
    assert document["devServers"] == {"gameId": "dragonwilds"}


def test_legacy_aliases_point_at_this_targets_roles() -> None:
    game = json.loads(GAME_PATH.read_text(encoding="utf-8"))

    assert game["legacyAssetAliases"] == {"takaro-dragonwilds-plugin.tar.gz": f"{TARGET}/plugin"}
    assert list(game["componentRoles"]) == ["plugin"]
    assert game["devServers"] == {"composeFile": "dragonwilds.yml"}


def test_the_workflow_skips_the_runtime_leg() -> None:
    workflow = WORKFLOW.read_text(encoding="utf-8")

    assert "uses: ./.github/workflows/connector-release.yml" in workflow
    assert "runtime: false" in workflow
    # The contract claim has to be produced by something: the plugin's unit and native
    # connector tests and the upgrade tool's tests, run in the `test` job before `release`.
    assert "mod/build.sh --tests" in workflow
    assert "test_drain_legacy.py" in workflow or "test_*.py" in workflow
    assert "sidecar" not in workflow.lower()


# -- resolution --------------------------------------------------------------------------


def test_targets_resolve_env_for_dragonwilds(run: Any) -> None:
    resolved = resolve(run)
    env = resolved["env"]

    assert env["DRAGONWILDS_TARGET"] == TARGET
    assert env["DRAGONWILDS_REVISION"] == "25630937"
    assert env["DRAGONWILDS_STEAM_APP"] == str(APP)
    assert env["DRAGONWILDS_STEAM_DEPOTS"] == f"{DEPOT}:{MANIFEST}"
    assert env["DRAGONWILDS_ARTIFACT_PLUGIN"] == f"takaro-dragonwilds-plugin-{TARGET}-{{version}}.tar.gz"
    assert env["DRAGONWILDS_INSTALL_DIR_PLUGIN"] == PLUGIN_FOLDER
    # One component: the plugin holds the Takaro connection, so nothing else is built or pinned.
    assert not [key for key in env if "SIDECAR" in key]
    # The builder base is pinned by digest.
    assert env["DRAGONWILDS_TOOLCHAIN"] == env["DRAGONWILDS_IMAGE"]
    assert "@sha256:" in env["DRAGONWILDS_TOOLCHAIN"]
    # Nothing here is a JVM.
    assert "DRAGONWILDS_JAVA" not in env
    # The four static libraries, by exact URL and hash, for Dockerfile.builder.
    for key in ("OPENSSL", "LIBWEBSOCKETS", "PCRE2", "NLOHMANN_JSON"):
        assert env[f"DRAGONWILDS_DEP_{key}_URL"].startswith("https://github.com/")
        assert len(env[f"DRAGONWILDS_DEP_{key}_SHA256"]) == 64
    assert not [key for key in env if key.startswith("DRAGONWILDS_DEP_WS_")]


def test_the_default_target_resolves_to_the_newest_build(run: Any) -> None:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--prefix", "DRAGONWILDS")

    assert code == 0, err
    assert payload["env"]["DRAGONWILDS_TARGET"] == TARGET
    assert resolve(run, PREVIOUS)["env"]["DRAGONWILDS_REVISION"] == "25501739"
    assert resolve(run, OLDEST)["env"]["DRAGONWILDS_REVISION"] == "25465077"


def test_the_adapter_names_one_file_per_role_and_never_a_glob(run: Any) -> None:
    resolved = resolve(run)

    paths = adapter_for(GAME).artifact_paths(resolved, VERSION, REPO_ROOT)

    assert {role: path.name for role, path in paths.items()} == {"plugin": PLUGIN_ARTIFACT}
    assert all(path.parent == REPO_ROOT / "games/dragonwilds/_data/dist" / resolved["fp16"] for path in paths.values())


def test_the_adapter_defines_a_real_install_step() -> None:
    adapter = adapter_for(GAME)

    # Unlike Dune's image bundle, this depot is a plain install: the adapter overrides
    # `install` itself rather than falling back to BaseAdapter's "generic path" default.
    assert "install" in type(adapter).__dict__


def test_post_install_gives_every_program_its_executable_bit_back(tmp_path: Path) -> None:
    # DepotDownloader writes every file 0644; the image entrypoint runs the binary directly and the
    # binary spawns Sentry's crash handler, which aborts the server when it cannot be executed.
    launcher = tmp_path / "RSDragonwildsServer.sh"
    binary = tmp_path / "RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping"
    crashpad = tmp_path / "RSDragonwilds/Plugins/Developer/Sentry/Binaries/Linux/crashpad_handler"
    data = tmp_path / "RSDragonwilds/Content/Paks/RSDragonwilds-LinuxServer.pak"
    for path, body in ((launcher, b"#!/bin/sh\n"), (binary, b"\x7fELF..."), (crashpad, b"\x7fELF..."), (data, b"PAK")):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
        path.chmod(0o644)

    adapter_for(GAME)._post_install(tmp_path)  # type: ignore[attr-defined]

    for program in (launcher, binary, crashpad):
        assert program.stat().st_mode & 0o111 == 0o111, program
    assert data.stat().st_mode & 0o111 == 0
    assert (tmp_path / "RSDragonwilds" / "Saved").is_dir()


def test_an_already_installed_tree_gets_its_executable_bits_back(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    binary = tmp_path / "RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"\x7fELF...")
    binary.chmod(0o644)
    monkeypatch.setattr("takaro_maint.steam.install.install_exact", lambda *a, **k: {"status": "already-installed"})
    args = type("Args", (), {"dest": str(tmp_path), "rollback": False, "dry_run": False})()

    adapter_for(GAME).install(None, type("T", (), {"game": GAME, "id": TARGET})(), {"preserve": []}, args)

    assert binary.stat().st_mode & 0o111 == 0o111
    assert json.loads(capsys.readouterr().out)["executablesRestored"] == 1


def test_the_server_banners_are_parsed() -> None:
    adapter = adapter_for(GAME)

    assert adapter.parse_runtime_identity("LogInit: Build: ++dominion+staging-CL-240163") == {
        "gameVersion": "dominion+staging-CL-240163",
        "loader": "unreal",
        "loaderVersion": None,
    }
    assert adapter.parse_runtime_identity("LogInit: Engine Version: 5.6.1-240163+++dominion+staging") == {
        "gameVersion": None,
        "loader": "unreal",
        "loaderVersion": "5.6.1",
    }
    assert adapter.parse_runtime_identity("LogTakaro: nothing to see here") is None


# -- deploy ------------------------------------------------------------------------------


def _archive(tmp_path: Path, name: str, folder: str, entries: dict[str, str]) -> Path:
    archive = tmp_path / name
    with tarfile.open(archive, "w:gz") as handle:
        for relative, body in entries.items():
            payload = body.encode()
            info = tarfile.TarInfo(f"{folder}/{relative}")
            info.size = len(payload)
            handle.addfile(info, io.BytesIO(payload))
    return archive


def test_after_deploy_replaces_the_plugin_folder(tmp_path: Path, run: Any) -> None:
    resolved = resolve(run)
    component = next(c for c in resolved["components"] if c["role"] == "plugin")
    dest = tmp_path / "rig"
    archive = _archive(
        tmp_path,
        PLUGIN_ARTIFACT,
        PLUGIN_FOLDER,
        {"libtakaro-dragonwilds.so": "ELF\n", "takaro-target.json": json.dumps({"connectorVersion": VERSION})},
    )
    live = dest / component["installDir"] / PLUGIN_FOLDER
    live.mkdir(parents=True)
    (live / "gone.txt").write_text("removed upstream\n", encoding="utf-8")

    adapter_for(GAME).after_deploy(dest, component, archive)

    assert (live / "libtakaro-dragonwilds.so").read_text(encoding="utf-8") == "ELF\n"
    # A file removed upstream must not survive an upgrade.
    assert not (live / "gone.txt").exists()


def test_after_deploy_refuses_a_role_without_a_folder(tmp_path: Path, run: Any) -> None:
    archive = _archive(tmp_path, PLUGIN_ARTIFACT, PLUGIN_FOLDER, {"libtakaro-dragonwilds.so": "ELF\n"})

    with pytest.raises(ConflictError):
        adapter_for(GAME).after_deploy(tmp_path / "rig", {"role": "sidecar", "installDir": "x"}, archive)


def test_after_deploy_refuses_an_archive_that_reaches_outside_its_folder(tmp_path: Path, run: Any) -> None:
    resolved = resolve(run)
    component = next(c for c in resolved["components"] if c["role"] == "plugin")
    archive = _archive(tmp_path, PLUGIN_ARTIFACT, "..", {"libtakaro-dragonwilds.so": "ELF\n"})

    with pytest.raises(ConflictError):
        adapter_for(GAME).after_deploy(tmp_path / "rig", component, archive)
