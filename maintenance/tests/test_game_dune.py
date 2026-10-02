"""The Dune: Awakening target: catalog, resolution, artifact naming, deploy.

Two things this game does not have, and the tests say so rather than pretending otherwise.
There is no install test, because the depot is a bundle of container images and the shared
Steam install path has no ``docker load`` seam yet; the adapter therefore defines no
``install`` and the record declares no ``devServers`` block, and both are asserted here so
a future seam cannot land silently. And there is no runtime verification, because a world
only becomes joinable with an operator's own Funcom token: the target claims ``contract``
and the workflow passes ``runtime: false``, which is also asserted, because those two are
the whole honesty of this connector's release.
"""

from __future__ import annotations

import fnmatch
import io
import json
import subprocess
import tarfile
from pathlib import Path
from typing import Any

import pytest

from takaro_maint.exit_codes import ConflictError
from takaro_maint.games import adapter_for

GAME = "dune"
TARGET = "linux-25635074"
APP = 4754530
DEPOT = "4754532"
MANIFEST = "5212661890512609510"
VERSION = "0.1.0-dev.abc1234"
SIDECAR_ARTIFACT = f"takaro-dune-sidecar-{TARGET}-{VERSION}.tar.gz"
PLUGIN_ARTIFACT = f"takaro-dune-plugin-{TARGET}-{VERSION}.tar.gz"
SIDECAR_FOLDER = "TakaroDuneSidecar"
PLUGIN_FOLDER = "TakaroDune"

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
GAME_PATH = REPO_ROOT / "catalog" / GAME / "game.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "dune.yml"


def record() -> dict[str, Any]:
    return json.loads(TARGET_PATH.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "DUNE")
    assert code == 0, err
    return dict(payload)


# -- catalog -----------------------------------------------------------------------------


def test_catalog_validate_accepts_the_dune_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert rows, "the dune target produced no checks"
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

    assert (server["app"], server["branch"], server["buildid"]) == (APP, "public", 25635074)
    assert server["depots"] == {DEPOT: {"manifest": MANIFEST, "size": 5209358233, "files": 75}}
    # Tool app 4754530 is free to download and steamcmd still refuses it; DepotDownloader
    # reads it with the anonymous login, which is what `credentials: null` claims.
    assert server["credentials"] is None
    # The image bundle is the identity of this build, so it may never lose its hash.
    assert server["files"]["images/battlegroup/server.tar"]["sha256"]
    assert all(declared["sha256"] for declared in server["files"].values())


def test_the_target_claims_contract_verification_and_no_rig_install() -> None:
    document = record()

    # A hosted runner can neither hold 15 GB of images nor obtain a Funcom token, so no
    # level above contract may be claimed as required.
    assert document["verification"]["required"] == "contract"
    assert "startup" in document["verification"]["separate"]
    # No ledger is written for this target yet, so nothing may tell the rig to check one.
    assert "devServers" not in document
    assert json.loads(GAME_PATH.read_text(encoding="utf-8"))["legacyAssetAliases"] == {}


def test_the_workflow_skips_the_runtime_leg() -> None:
    workflow = WORKFLOW.read_text(encoding="utf-8")

    assert "uses: ./.github/workflows/connector-release.yml" in workflow
    assert "runtime: false" in workflow
    # The contract claim has to be produced by something: the sidecar's own suite.
    assert "npm test" in workflow


# -- resolution --------------------------------------------------------------------------


def test_targets_resolve_env_for_dune(run: Any) -> None:
    resolved = resolve(run)
    env = resolved["env"]

    assert env["DUNE_TARGET"] == TARGET
    assert env["DUNE_REVISION"] == "25635074"
    assert env["DUNE_STEAM_APP"] == str(APP)
    assert env["DUNE_STEAM_DEPOTS"] == f"{DEPOT}:{MANIFEST}"
    assert env["DUNE_ARTIFACT_SIDECAR"] == f"takaro-dune-sidecar-{TARGET}-{{version}}.tar.gz"
    assert env["DUNE_ARTIFACT_PLUGIN"] == f"takaro-dune-plugin-{TARGET}-{{version}}.tar.gz"
    assert env["DUNE_INSTALL_DIR_SIDECAR"] == SIDECAR_FOLDER
    # One pinned image builds both halves, and it is pinned by digest.
    assert env["DUNE_TOOLCHAIN"] == env["DUNE_IMAGE"]
    assert "@sha256:" in env["DUNE_TOOLCHAIN"]
    # Nothing here is a JVM.
    assert "DUNE_JAVA" not in env
    # Every dependency the build checks the lockfile against.
    for name in ("AMQPLIB", "PG", "WS"):
        assert env[f"DUNE_DEP_{name}_URL"].startswith("https://registry.npmjs.org/")
        assert len(env[f"DUNE_DEP_{name}_SHA256"]) == 64


def test_the_adapter_names_one_file_per_role_and_never_a_glob(run: Any) -> None:
    resolved = resolve(run)

    paths = adapter_for(GAME).artifact_paths(resolved, VERSION, REPO_ROOT)

    assert {role: path.name for role, path in paths.items()} == {
        "sidecar": SIDECAR_ARTIFACT,
        "plugin": PLUGIN_ARTIFACT,
    }
    assert all(path.parent == REPO_ROOT / "games/dune/_data/dist" / resolved["fp16"] for path in paths.values())


def test_the_adapter_defines_no_install_step() -> None:
    adapter = adapter_for(GAME)

    # The depot is an image bundle. Until the shared Steam path can `docker load`, an
    # install hook here would claim a path that does not exist.
    assert not hasattr(adapter, "install")
    assert not hasattr(adapter, "container_command")


def test_the_map_server_banners_are_parsed() -> None:
    adapter = adapter_for(GAME)

    assert adapter.parse_runtime_identity("LogInit: Build: ++Dune+Release-2118731") == {
        "gameVersion": "Dune+Release-2118731",
        "loader": "unreal",
        "loaderVersion": None,
    }
    assert adapter.parse_runtime_identity("LogInit: Engine Version: 5.2.1-2118731+++Dune+Release") == {
        "gameVersion": None,
        "loader": "unreal",
        "loaderVersion": "5.2.1",
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


def test_after_deploy_replaces_the_folder_and_keeps_the_operators_env(tmp_path: Path, run: Any) -> None:
    resolved = resolve(run)
    component = next(c for c in resolved["components"] if c["role"] == "sidecar")
    dest = tmp_path / "rig"
    live = dest / component["installDir"] / SIDECAR_FOLDER
    live.mkdir(parents=True)
    (live / ".env").write_text("TAKARO_REGISTRATION_TOKEN=keep-me\n", encoding="utf-8")
    (live / "gone.js").write_text("// removed upstream\n", encoding="utf-8")
    archive = _archive(
        tmp_path,
        SIDECAR_ARTIFACT,
        SIDECAR_FOLDER,
        {"dist/index.js": "// new\n", "takaro-target.json": json.dumps({"connectorVersion": VERSION})},
    )

    adapter_for(GAME).after_deploy(dest, component, archive)

    assert (live / "dist" / "index.js").read_text(encoding="utf-8") == "// new\n"
    # A file removed upstream must not survive an upgrade...
    assert not (live / "gone.js").exists()
    # ...but the operator's own configuration must.
    assert (live / ".env").read_text(encoding="utf-8") == "TAKARO_REGISTRATION_TOKEN=keep-me\n"


def test_after_deploy_refuses_an_archive_that_reaches_outside_its_folder(tmp_path: Path, run: Any) -> None:
    resolved = resolve(run)
    component = next(c for c in resolved["components"] if c["role"] == "plugin")
    archive = _archive(tmp_path, PLUGIN_ARTIFACT, "..", {"libtakaro-dune.so": "ELF\n"})

    with pytest.raises(ConflictError):
        adapter_for(GAME).after_deploy(tmp_path / "rig", component, archive)


# -- the dev-servers split ---------------------------------------------------------------


def test_the_rig_reads_its_pins_from_the_catalog() -> None:
    """No second copy of the app, the depot or the manifest anywhere in the rig."""
    game_file = (REPO_ROOT / "dev-servers" / "lib" / "games" / "dune.sh").read_text(encoding="utf-8")

    assert "targets resolve --game dune" in game_file
    # Prose may name the app while explaining why steamcmd refuses it; the download may not.
    assert MANIFEST not in game_file
    download = game_file[game_file.index("DepotDownloader -app") :].split("\n")[0]
    assert "${appid}" in download and "${depotid}" in download and "${manifest}" in download


def test_the_registry_row_is_registered_from_the_game_file() -> None:
    listing = subprocess.run(
        ["bash", "-c", '. dev-servers/lib/common.sh; ds_registry | cut -d"|" -f1'],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    assert listing.returncode == 0, listing.stderr
    assert GAME in listing.stdout.split()


def test_the_sidecar_release_docker_build_can_see_its_prebuilt_dist() -> None:
    """The release Dockerfile COPYs dist/; the source tree's .dockerignore excludes it for Dockerfile.dev."""
    project = REPO_ROOT / "games" / "dune"
    dockerfile = (project / "sidecar" / "Dockerfile").read_text(encoding="utf-8")
    release_ignore = (project / "scripts" / "templates" / "sidecar.dockerignore.release").read_text(encoding="utf-8")
    build = (project / "scripts" / "build-release.sh").read_text(encoding="utf-8")

    ignored = [line.strip().rstrip("/") for line in release_ignore.splitlines() if line.strip() and not line.startswith("#")]
    copied = [
        source.rstrip("/")
        for line in dockerfile.splitlines()
        if line.startswith("COPY ") and "--from=" not in line
        for source in line.split()[1:-1]
    ]
    assert "dist" in copied
    # Every path the release Dockerfile copies must survive the packaged .dockerignore.
    assert [s for s in copied if any(fnmatch.fnmatch(s, pattern) for pattern in ignored)] == []
    assert 'templates/sidecar.dockerignore.release" "${SPKG}/.dockerignore"' in build
    assert '"${PROJECT_ROOT}/sidecar/.dockerignore"' not in build
