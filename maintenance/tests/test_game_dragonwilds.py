"""The RuneScape: Dragonwilds target: catalog, resolution, artifact naming, deploy.

This is this connector's first exact catalog target (#300). Unlike Dune: Awakening, the
server is a plain Steam depot install -- one binary, its ``.sym`` file, a launcher script
-- so the adapter owns a real ``install`` through the shared Steam exact-install path, the
way Conan Exiles does. There is no runtime container for this game yet (the operator
supplies their own dedicated-server image), so the target claims ``contract`` and the
workflow passes ``runtime: false``, which is asserted here, because those two are the
whole honesty of this connector's release at this stage.
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
TARGET = "linux-25465077"
APP = 4019830
DEPOT = "3501791"
MANIFEST = "2601451637939157694"
VERSION = "0.1.0-dev.abc1234"
PLUGIN_ARTIFACT = f"takaro-dragonwilds-plugin-{TARGET}-{VERSION}.tar.gz"
SIDECAR_ARTIFACT = f"takaro-dragonwilds-sidecar-{TARGET}-{VERSION}.tar.gz"
PLUGIN_FOLDER = "TakaroDragonwilds"
SIDECAR_FOLDER = "TakaroDragonwildsSidecar"

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
GAME_PATH = REPO_ROOT / "catalog" / GAME / "game.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "dragonwilds.yml"


def record() -> dict[str, Any]:
    return json.loads(TARGET_PATH.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "DRAGONWILDS")
    assert code == 0, err
    return dict(payload)


# -- catalog -----------------------------------------------------------------------------


def test_catalog_validate_accepts_the_dragonwilds_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert rows, "the dragonwilds target produced no checks"
    assert {check["id"] for check in rows} >= {
        "input-kind-schema",
        "build-system-schema",
        "build-script-exists",
        "no-null-hash",
        "immutable-tag-and-digest",
    }
    assert all(check["status"] == "pass" for check in rows), rows


def test_the_steam_pin_is_exact_and_anonymous() -> None:
    server = record()["inputs"]["server"]

    assert (server["app"], server["branch"], server["buildid"]) == (APP, "public", 25465077)
    assert server["depots"][DEPOT]["manifest"] == MANIFEST
    assert server["credentials"] is None
    assert all(declared["sha256"] for declared in server["files"].values())
    assert "RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping" in server["files"]
    assert "RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping.sym" in server["files"]


def test_the_target_claims_contract_verification_and_a_steam_install() -> None:
    document = record()

    # No runtime image exists for this game yet, so no level above contract may be claimed
    # as required -- the same posture Dune: Awakening and Conan Exiles took at this stage.
    assert document["verification"]["required"] == "contract"
    assert "startup" in document["verification"]["separate"]
    assert document["support"]["status"] == "candidate"
    # Unlike Dune, the server is a plain depot: a rig ledger is meaningful here.
    assert document["devServers"] == {"gameId": "dragonwilds"}


def test_legacy_aliases_point_at_this_targets_roles() -> None:
    game = json.loads(GAME_PATH.read_text(encoding="utf-8"))

    assert game["legacyAssetAliases"] == {
        "takaro-dragonwilds-plugin.tar.gz": f"{TARGET}/plugin",
        "takaro-dragonwilds-sidecar.tar.gz": f"{TARGET}/sidecar",
    }
    assert game["devServers"] == {"composeFile": "dragonwilds.yml"}


def test_the_workflow_skips_the_runtime_leg() -> None:
    workflow = WORKFLOW.read_text(encoding="utf-8")

    assert "uses: ./.github/workflows/connector-release.yml" in workflow
    assert "runtime: false" in workflow
    # The contract claim has to be produced by something: the sidecar's own suite and the
    # plugin's unit tests, both run in the `test` job before `release` starts.
    assert "npm test" in workflow
    assert "./build.sh --native --tests" in workflow


# -- resolution --------------------------------------------------------------------------


def test_targets_resolve_env_for_dragonwilds(run: Any) -> None:
    resolved = resolve(run)
    env = resolved["env"]

    assert env["DRAGONWILDS_TARGET"] == TARGET
    assert env["DRAGONWILDS_REVISION"] == "25465077"
    assert env["DRAGONWILDS_STEAM_APP"] == str(APP)
    assert env["DRAGONWILDS_STEAM_DEPOTS"] == f"{DEPOT}:{MANIFEST}"
    assert env["DRAGONWILDS_ARTIFACT_PLUGIN"] == f"takaro-dragonwilds-plugin-{TARGET}-{{version}}.tar.gz"
    assert env["DRAGONWILDS_ARTIFACT_SIDECAR"] == f"takaro-dragonwilds-sidecar-{TARGET}-{{version}}.tar.gz"
    assert env["DRAGONWILDS_INSTALL_DIR_PLUGIN"] == PLUGIN_FOLDER
    assert env["DRAGONWILDS_INSTALL_DIR_SIDECAR"] == SIDECAR_FOLDER
    # One pinned image builds both halves, and it is pinned by digest.
    assert env["DRAGONWILDS_TOOLCHAIN"] == env["DRAGONWILDS_IMAGE"]
    assert "@sha256:" in env["DRAGONWILDS_TOOLCHAIN"]
    # Nothing here is a JVM.
    assert "DRAGONWILDS_JAVA" not in env
    assert env["DRAGONWILDS_DEP_WS_URL"].startswith("https://registry.npmjs.org/")
    assert len(env["DRAGONWILDS_DEP_WS_SHA256"]) == 64


def test_the_adapter_names_one_file_per_role_and_never_a_glob(run: Any) -> None:
    resolved = resolve(run)

    paths = adapter_for(GAME).artifact_paths(resolved, VERSION, REPO_ROOT)

    assert {role: path.name for role, path in paths.items()} == {
        "plugin": PLUGIN_ARTIFACT,
        "sidecar": SIDECAR_ARTIFACT,
    }
    assert all(path.parent == REPO_ROOT / "games/dragonwilds/_data/dist" / resolved["fp16"] for path in paths.values())


def test_the_adapter_defines_a_real_install_step() -> None:
    adapter = adapter_for(GAME)

    # Unlike Dune's image bundle, this depot is a plain install: the adapter overrides
    # `install` itself rather than falling back to BaseAdapter's "generic path" default.
    assert "install" in type(adapter).__dict__


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


def test_after_deploy_replaces_the_sidecar_folder(tmp_path: Path, run: Any) -> None:
    resolved = resolve(run)
    component = next(c for c in resolved["components"] if c["role"] == "sidecar")
    dest = tmp_path / "rig"
    live = dest / component["installDir"] / SIDECAR_FOLDER
    live.mkdir(parents=True)
    (live / "gone.js").write_text("// removed upstream\n", encoding="utf-8")
    archive = _archive(
        tmp_path,
        SIDECAR_ARTIFACT,
        SIDECAR_FOLDER,
        {"dist/index.js": "// new\n", "takaro-target.json": json.dumps({"connectorVersion": VERSION})},
    )

    adapter_for(GAME).after_deploy(dest, component, archive)

    assert (live / "dist" / "index.js").read_text(encoding="utf-8") == "// new\n"
    # A file removed upstream must not survive an upgrade.
    assert not (live / "gone.js").exists()


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

    adapter_for(GAME).after_deploy(dest, component, archive)

    live = dest / component["installDir"] / PLUGIN_FOLDER
    assert (live / "libtakaro-dragonwilds.so").read_text(encoding="utf-8") == "ELF\n"


def test_after_deploy_refuses_an_archive_that_reaches_outside_its_folder(tmp_path: Path, run: Any) -> None:
    resolved = resolve(run)
    component = next(c for c in resolved["components"] if c["role"] == "plugin")
    archive = _archive(tmp_path, PLUGIN_ARTIFACT, "..", {"libtakaro-dragonwilds.so": "ELF\n"})

    with pytest.raises(ConflictError):
        adapter_for(GAME).after_deploy(tmp_path / "rig", component, archive)
