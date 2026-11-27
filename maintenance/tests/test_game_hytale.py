"""The Hytale target: catalog, resolution, artifact naming, runtime identity, install and deploy.

The connector is one Java plugin jar in the server's mods/ folder. It compiles against
com.hypixel.hytale:Server from Hypixel's Maven repository and never ships it. A Hytale server
needs Assets.zip and an account login, so no runtime image runs it: the target claims
``contract`` and the workflow passes ``runtime: false``.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from takaro_maint.exit_codes import ConflictError, UsageError
from takaro_maint.games import adapter_for

GAME = "hytale"
TARGET = "server-0.7.7"
REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "hytale.yml"


def record() -> dict[str, Any]:
    path = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
    return json.loads(path.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def test_catalog_validate_accepts_the_hytale_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert {check["id"] for check in rows} >= {"input-kind-schema", "build-script-exists", "no-null-hash"}
    assert all(check["status"] == "pass" for check in rows), rows


def test_the_server_jar_is_pinned_once_and_consistently() -> None:
    document = record()
    server = document["inputs"]["server"]
    dep = document["build"]["deps"]["hytale-server"]

    assert document["revision"] == "0.7.7"
    assert dep["coordinate"] == "com.hypixel.hytale:Server:0.7.7"
    assert server["sha256"] == dep["sha256"]
    assert dep["resolvedCoordinate"] == "https://maven.hytale.com" + server["path"]
    assert document["runtime"]["java"] == 25
    assert document["verification"]["required"] == "contract"


def test_the_pom_compiles_against_the_pinned_server_version() -> None:
    pom = (REPO_ROOT / "games/hytale/mod/pom.xml").read_text(encoding="utf-8")

    assert "<hytale.server.version>0.7.7</hytale.server.version>" in pom
    assert "<scope>provided</scope>" in pom, "the Hytale server must never be shaded into the plugin"


def test_resolve_names_the_artifact_and_build_inputs(run: Any) -> None:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "HYTALE")
    assert code == 0, err
    env = adapter_for(GAME).env(payload, "HYTALE")

    assert env["HYTALE_ARTIFACT_PLUGIN"] == "takaro-hytale-server-0.7.7-{version}.jar"
    assert env["HYTALE_DEP_HYTALE_SERVER_COORDINATE"] == "com.hypixel.hytale:Server:0.7.7"
    assert env["HYTALE_REVISION"] == "0.7.7"
    assert env["HYTALE_TOOLCHAIN"].startswith("maven:3.10.0-eclipse-temurin-25@sha256:")


def test_the_boot_line_is_the_runtime_identity() -> None:
    line = "[HytaleServer] Booting up HytaleServer - Version: 0.7.7, Revision: 12fc465604eecca4"

    assert adapter_for(GAME).parse_runtime_identity(line) == {
        "gameVersion": "0.7.7",
        "loader": "hytale",
        "loaderVersion": None,
    }
    assert adapter_for(GAME).parse_runtime_identity("[World|default] Loading world") is None


def test_install_is_refused_with_a_reason() -> None:
    with pytest.raises(UsageError, match="Hytale account"):
        adapter_for(GAME).install(None, None, {}, None)


def test_deploy_accepts_only_the_plugin_jar(tmp_path: Path) -> None:
    adapter = adapter_for(GAME)
    jar = tmp_path / "takaro-hytale-server-0.7.7-0.1.0.jar"
    jar.write_bytes(b"PK")

    adapter.after_deploy(tmp_path, {"role": "plugin", "installDir": "mods"}, jar)
    with pytest.raises(ConflictError):
        adapter.after_deploy(tmp_path, {"role": "sidecar", "installDir": "mods"}, jar)
    with pytest.raises(ConflictError):
        adapter.after_deploy(tmp_path, {"role": "plugin", "installDir": "mods"}, tmp_path / "x.zip")


def test_the_workflow_releases_at_contract_level() -> None:
    text = WORKFLOW.read_text(encoding="utf-8")

    assert "uses: ./.github/workflows/connector-release.yml" in text
    assert "connector: hytale" in text
    assert "runtime: false" in text
    assert "games/hytale/scripts/test.sh" in text
