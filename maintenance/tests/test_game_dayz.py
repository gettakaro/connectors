"""The DayZ target: catalog, resolution, artifact naming, runtime identity and deploy.

The connector is two required parts: the @TakaroIntegration server mod (one PBO, loaded with
-serverMod=) and a Node.js sidecar on the same host that holds the Takaro WebSocket. The
server's depots are served only to a Steam account that owns DayZ, so the declared file hashes
are self-recorded, the install needs STEAM_USER, the target claims ``contract`` and the workflow
passes ``runtime: false``. Each of those is asserted here, because together they are the whole
honesty of this connector's release.
"""

from __future__ import annotations

import io
import json
import tarfile
import zipfile
from pathlib import Path
from typing import Any

import pytest

from takaro_maint.exit_codes import ConflictError
from takaro_maint.games import adapter_for

GAME = "dayz"
TARGET = "linux-26220720"
APP = 223350
DEPOTS = {
    "223351": "5401746265257134544",
    "223352": "9061428159600400099",
    "223354": "5867941930105127159",
}
VERSION = "0.1.0-dev.abc1234"
MOD_ARTIFACT = f"takaro-dayz-mod-{TARGET}-{VERSION}.zip"
SIDECAR_ARTIFACT = f"takaro-dayz-sidecar-{TARGET}-{VERSION}.tar.gz"
MOD_FOLDER = "@TakaroIntegration"
SIDECAR_FOLDER = "TakaroDayZSidecar"

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
GAME_PATH = REPO_ROOT / "catalog" / GAME / "game.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "dayz.yml"


def record() -> dict[str, Any]:
    return json.loads(TARGET_PATH.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "DAYZ")
    assert code == 0, err
    return dict(payload)


# -- catalog -----------------------------------------------------------------------------


def test_catalog_validate_accepts_the_dayz_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert rows, "the dayz target produced no checks"
    assert {check["id"] for check in rows} >= {
        "input-kind-schema",
        "build-system-schema",
        "build-script-exists",
        "no-null-hash",
        "immutable-tag-and-digest",
    }
    assert all(check["status"] == "pass" for check in rows), rows


def test_the_steam_pin_is_exact_and_needs_an_owning_account() -> None:
    server = record()["inputs"]["server"]

    assert (server["app"], server["branch"], server["buildid"]) == (APP, "public", 26220720)
    assert {depot: entry["manifest"] for depot, entry in server["depots"].items()} == DEPOTS
    # The Linux binaries depot: its three files add up to exactly the size Steam publishes.
    binaries = ("DayZServer", "libsteam_api.so", "steamclient.so")
    assert sum(server["files"][name]["size"] for name in binaries) == server["depots"]["223352"]["size"]
    # Steam serves these depots only to an account that owns DayZ, so the hashes cannot come
    # from an anonymous download and the record must not claim the anonymous login works.
    assert server["hashOrigin"] == "self-recorded"
    assert server["credentials"] == {"accountEnv": "STEAM_USER"}
    assert all(declared["sha256"] for declared in server["files"].values())


def test_the_target_claims_contract_verification() -> None:
    document = record()

    assert document["verification"]["required"] == "contract"
    assert "startup" in document["verification"]["separate"]
    game = json.loads(GAME_PATH.read_text(encoding="utf-8"))
    assert game["legacyAssetAliases"] == {}
    assert set(game["componentRoles"]) == {"mod", "sidecar"}
    assert game["devServers"] == {"composeFile": "dayz.yml"}


def test_the_workflow_skips_the_runtime_leg() -> None:
    workflow = WORKFLOW.read_text(encoding="utf-8")

    assert "uses: ./.github/workflows/connector-release.yml" in workflow
    assert "connector: dayz" in workflow
    assert "runtime: false" in workflow
    # The contract claim has to be produced by something: the sidecar's suite and the packer's.
    assert "games/dayz/scripts/test.sh" in workflow


# -- resolution --------------------------------------------------------------------------


def test_targets_resolve_env_for_dayz(run: Any) -> None:
    env = resolve(run)["env"]

    assert env["DAYZ_TARGET"] == TARGET
    assert env["DAYZ_REVISION"] == "26220720"
    assert env["DAYZ_PLATFORM"] == "linux"
    assert env["DAYZ_STEAM_APP"] == str(APP)
    assert env["DAYZ_STEAM_DEPOTS"] == ";".join(f"{depot}:{manifest}" for depot, manifest in sorted(DEPOTS.items()))
    assert env["DAYZ_ARTIFACT_MOD"] == f"takaro-dayz-mod-{TARGET}-{{version}}.zip"
    assert env["DAYZ_ARTIFACT_SIDECAR"] == f"takaro-dayz-sidecar-{TARGET}-{{version}}.tar.gz"
    assert env["DAYZ_INSTALL_DIR_SIDECAR"] == "TakaroDayZSidecar"
    assert "@sha256:" in env["DAYZ_TOOLCHAIN"]
    assert "DAYZ_JAVA" not in env
    for name in ("WINSTON", "WS"):
        assert env[f"DAYZ_DEP_{name}_URL"].startswith("https://registry.npmjs.org/")
        assert len(env[f"DAYZ_DEP_{name}_SHA256"]) == 64


def test_the_pinned_deps_are_what_the_lockfile_resolves() -> None:
    lock = json.loads((REPO_ROOT / "games/dayz/sidecar/package-lock.json").read_text(encoding="utf-8"))
    for name, dep in record()["build"]["deps"].items():
        entry = lock["packages"][f"node_modules/{name}"]
        assert dep["coordinate"] == f"npm:{name}:{entry['version']}"
        assert dep["resolvedCoordinate"] == entry["resolved"]


def test_the_adapter_names_one_file_per_role_and_never_a_glob(run: Any) -> None:
    resolved = resolve(run)

    paths = adapter_for(GAME).artifact_paths(resolved, VERSION, REPO_ROOT)

    assert {role: path.name for role, path in paths.items()} == {"mod": MOD_ARTIFACT, "sidecar": SIDECAR_ARTIFACT}
    assert all(path.parent == REPO_ROOT / "games/dayz/_data/dist" / resolved["fp16"] for path in paths.values())


def test_the_rpt_version_line_is_the_runtime_identity() -> None:
    adapter = adapter_for(GAME)

    assert adapter.parse_runtime_identity("Version 1.30.164289.70") == {
        "gameVersion": "1.30.164289.70",
        "loader": "enfusion",
        "loaderVersion": None,
    }
    assert adapter.parse_runtime_identity("[Takaro] Bridge v0.2.0 starting") is None


# -- deploy ------------------------------------------------------------------------------


def _tar(tmp_path: Path, name: str, entries: dict[str, str]) -> Path:
    archive = tmp_path / name
    with tarfile.open(archive, "w:gz") as handle:
        for relative, body in entries.items():
            payload = body.encode()
            info = tarfile.TarInfo(relative)
            info.size = len(payload)
            handle.addfile(info, io.BytesIO(payload))
    return archive


def _zip(tmp_path: Path, name: str, entries: dict[str, bytes]) -> Path:
    archive = tmp_path / name
    with zipfile.ZipFile(archive, "w") as handle:
        for relative, body in entries.items():
            handle.writestr(relative, body)
    return archive


def test_after_deploy_replaces_the_sidecar_and_keeps_the_operators_config(tmp_path: Path, run: Any) -> None:
    component = next(c for c in resolve(run)["components"] if c["role"] == "sidecar")
    dest = tmp_path / "server"
    live = dest / component["installDir"] / SIDECAR_FOLDER
    live.mkdir(parents=True)
    (live / "TakaroConfig.txt").write_text("registrationToken=keep-me\n", encoding="utf-8")
    (live / "gone.js").write_text("// removed upstream\n", encoding="utf-8")
    archive = _tar(
        tmp_path,
        SIDECAR_ARTIFACT,
        {
            f"{SIDECAR_FOLDER}/dist/index.js": "// new\n",
            f"{SIDECAR_FOLDER}/takaro-target.json": json.dumps({"connectorVersion": VERSION}),
        },
    )

    adapter_for(GAME).after_deploy(dest, component, archive)

    assert (live / "dist" / "index.js").read_text(encoding="utf-8") == "// new\n"
    assert not (live / "gone.js").exists()
    assert (live / "TakaroConfig.txt").read_text(encoding="utf-8") == "registrationToken=keep-me\n"


def test_after_deploy_unpacks_the_mod_folder(tmp_path: Path, run: Any) -> None:
    component = next(c for c in resolve(run)["components"] if c["role"] == "mod")
    dest = tmp_path / "server"
    stale = dest / component["installDir"] / MOD_FOLDER / "Addons" / "Old.pbo"
    stale.parent.mkdir(parents=True)
    stale.write_bytes(b"old")
    archive = _zip(
        tmp_path,
        MOD_ARTIFACT,
        {f"{MOD_FOLDER}/Addons/TakaroIntegration.pbo": b"\x00PBO", f"{MOD_FOLDER}/mod.cpp": b"name = x;\n"},
    )

    adapter_for(GAME).after_deploy(dest, component, archive)

    folder = dest / component["installDir"] / MOD_FOLDER
    assert (folder / "Addons" / "TakaroIntegration.pbo").read_bytes() == b"\x00PBO"
    assert not stale.exists()


@pytest.mark.parametrize("role", ["mod", "sidecar"])
def test_after_deploy_refuses_an_archive_that_reaches_outside_its_folder(tmp_path: Path, run: Any, role: str) -> None:
    component = next(c for c in resolve(run)["components"] if c["role"] == role)
    if role == "mod":
        archive = _zip(tmp_path, MOD_ARTIFACT, {"../escape.pbo": b"x"})
    else:
        archive = _tar(tmp_path, SIDECAR_ARTIFACT, {"other/dist/index.js": "x"})

    with pytest.raises(ConflictError):
        adapter_for(GAME).after_deploy(tmp_path / "server", component, archive)


# -- the build and the rig ---------------------------------------------------------------


def test_the_sidecar_release_docker_build_can_see_its_prebuilt_dist() -> None:
    project = REPO_ROOT / "games" / "dayz"
    dockerfile = (project / "sidecar" / "Dockerfile").read_text(encoding="utf-8")
    release_ignore = (project / "scripts" / "templates" / "sidecar.dockerignore.release").read_text(encoding="utf-8")
    build = (project / "scripts" / "build-release.sh").read_text(encoding="utf-8")

    assert "COPY dist ./dist" in dockerfile
    # The sidecar reads its game icon from assets/ beside dist/.
    assert "COPY assets ./assets" in dockerfile
    assert (project / "assets" / "icon-256.png").is_file()
    assert 'assets/icon-256.png" "${SPKG}/assets/' in build
    assert "dist" not in [line.strip().rstrip("/") for line in release_ignore.splitlines()]
    assert "sidecar.dockerignore.release" in build


def test_the_rig_builds_the_sidecar_from_this_repository() -> None:
    compose = (REPO_ROOT / "dev-servers" / "compose" / "dayz.yml").read_text(encoding="utf-8")

    assert "${DAYZ_BRIDGE_SRC:-../../games/dayz/sidecar}" in compose
    assert (REPO_ROOT / "games" / "dayz" / "sidecar" / "package.json").is_file()
