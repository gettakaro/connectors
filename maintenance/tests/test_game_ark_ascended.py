"""The ARK: Survival Ascended target: catalog, resolution, artifact naming, deploy.

The server is a Windows-only Steam depot; the connector is the AsaApi plugin TakaroAsa, shipped
with the pinned AsaApi release as one zip that mirrors ShooterGame/Binaries/Win64. No runtime image
runs this server, so the target claims ``contract`` and the workflow passes ``runtime: false``.
"""

from __future__ import annotations

import io
import json
import zipfile
from pathlib import Path
from typing import Any

import pytest

from takaro_maint.exit_codes import ConflictError, UsageError
from takaro_maint.games import adapter_for

GAME = "ark-ascended"
TARGET = "windows-26334129"
APP = 2430930
DEPOT = "2430931"
MANIFEST = "6604340029022292600"
VERSION = "0.1.0-dev.abc1234"
ARTIFACT = f"takaro-ark-ascended-{TARGET}-{VERSION}.zip"
PLUGIN = "ArkApi/Plugins/TakaroAsa"
WIN64 = "ShooterGame/Binaries/Win64"

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ark-ascended.yml"


def record() -> dict[str, Any]:
    return json.loads(TARGET_PATH.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "ARK_ASCENDED")
    assert code == 0, err
    return dict(payload)


def test_catalog_validate_accepts_the_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
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

    assert (server["app"], server["branch"], server["buildid"], server["os"]) == (APP, "public", 26334129, "windows")
    assert server["depots"][DEPOT]["manifest"] == MANIFEST
    assert server["credentials"] is None
    assert len(server["files"][f"{WIN64}/ArkAscendedServer.exe"]["sha256"]) == 64


def test_the_asaapi_headers_and_binaries_are_pinned_separately() -> None:
    deps = record()["build"]["deps"]

    assert deps["asaapi"]["coordinate"] == "ArkServerApi:AsaApi:2.03"
    assert deps["asaapi"]["resolvedCoordinate"].endswith("/2.03/AsaApi_2.03.zip")
    assert deps["asaapi-source"]["coordinate"].rsplit(":", 1)[1] in deps["asaapi-source"]["resolvedCoordinate"]
    assert len(deps["asaapi-source"]["coordinate"].rsplit(":", 1)[1]) == 40
    assert all(len(dep["sha256"]) == 64 for dep in deps.values())


def test_the_target_claims_contract_and_keeps_operator_state() -> None:
    document = record()

    assert document["default"] is True
    assert document["verification"]["required"] == "contract"
    assert [component["role"] for component in document["components"]] == ["mod"]
    assert document["components"][0]["installDir"] == WIN64
    assert "ShooterGame/Saved/" in document["preserve"]
    for state in ("TakaroConfig.txt", "takaro.log", "state/"):
        assert f"{WIN64}/{PLUGIN}/{state}" in document["preserve"]
    assert f"{WIN64}/config.json" in document["preserve"]
    assert f"{WIN64}/ArkApi/Cache/" in document["preserve"]


def test_the_workflow_compiles_on_windows_and_releases_through_the_shared_path() -> None:
    text = WORKFLOW.read_text(encoding="utf-8")

    assert "runs-on: windows-2022" in text
    assert "prebuilt_artifact: ark-ascended-native" in text
    assert "name: ark-ascended-native-${{ matrix.target.id }}" in text
    assert "runtime: false" in text
    assert "scripts/release-params.sh ark-ascended" in text


def test_resolve_names_the_artifact_and_build_inputs(run: Any) -> None:
    payload = resolve(run)

    assert payload["artifactFileNames"]["mod"] == f"takaro-ark-ascended-{TARGET}-{{version}}.zip"
    env = adapter_for(GAME).env(payload, "ARK_ASCENDED")
    assert env["ARK_ASCENDED_STEAM_BUILDID"] == "26334129"
    assert env["ARK_ASCENDED_ARTIFACT_MOD"] == f"takaro-ark-ascended-{TARGET}-{{version}}.zip"
    assert env["ARK_ASCENDED_TOOLCHAIN"].startswith("debian:13.7@sha256:")
    assert env["ARK_ASCENDED_DEP_ASAAPI_URL"].endswith("AsaApi_2.03.zip")
    assert "tar.gz" in env["ARK_ASCENDED_DEP_ASAAPI_SOURCE_URL"]


@pytest.mark.parametrize(
    ("line", "parsed"),
    [
        (
            "[2026.11.05-22.57.24:668][  0]ARK Version: 96.9",
            {"gameVersion": "96.9", "loader": "asaapi", "loaderVersion": None},
        ),
        ("[2026.11.05-22.57.24:668][  0]LogNet: Listening", None),
    ],
)
def test_the_server_log_banner_is_the_runtime_identity(line: str, parsed: dict[str, Any] | None) -> None:
    assert adapter_for(GAME).parse_runtime_identity(line) == parsed


# -- deploy ---------------------------------------------------------------------------------


def _zip(path: Path, entries: dict[str, bytes]) -> Path:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    path.write_bytes(buffer.getvalue())
    return path


def _release_entries() -> dict[str, bytes]:
    return {
        "AsaApiLoader.exe": b"loader",
        "config.json": b"{}",
        "ArkApi/AsaApi.dll": b"api",
        "ArkApi/LICENSE-AsaApi.txt": b"MIT",
        f"{PLUGIN}/TakaroAsa.dll": b"dll",
        f"{PLUGIN}/PluginInfo.json": b"{}",
        f"{PLUGIN}/TakaroConfig.txt.example": b"TAKARO_REGISTRATION_TOKEN=\n",
        f"{PLUGIN}/takaro-target.json": json.dumps({"connectorVersion": VERSION}).encode(),
        "PROVENANCE.txt": b"pinned",
    }


COMPONENT = {"role": "mod", "artifact": ARTIFACT, "installDir": WIN64}


def test_deploy_unpacks_into_win64_and_keeps_the_operator_state(tmp_path: Path) -> None:
    dest = tmp_path / "server"
    win64 = dest / WIN64
    plugin = win64 / PLUGIN
    (plugin / "state").mkdir(parents=True)
    (plugin / "TakaroConfig.txt").write_text("TAKARO_REGISTRATION_TOKEN=x\n")
    (plugin / "state/identity-token").write_text("keep")
    (plugin / "takaro.log").write_text("old log")
    (plugin / "TakaroAsa.dll").write_bytes(b"old")
    (win64 / "config.json").write_text('{"mine": true}')

    adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, _release_entries()))

    assert (plugin / "TakaroAsa.dll").read_bytes() == b"dll"
    assert (win64 / "AsaApiLoader.exe").read_bytes() == b"loader"
    assert (plugin / "TakaroConfig.txt").read_text() == "TAKARO_REGISTRATION_TOKEN=x\n"
    assert (plugin / "state/identity-token").read_text() == "keep"
    assert (plugin / "takaro.log").read_text() == "old log"
    assert (win64 / "config.json").read_text() == '{"mine": true}'


def test_deploy_writes_the_default_asaapi_config_on_a_fresh_install(tmp_path: Path) -> None:
    dest = tmp_path / "server"

    adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, _release_entries()))

    assert (dest / WIN64 / "config.json").read_bytes() == b"{}"


@pytest.mark.parametrize(
    ("extra", "missing"),
    [
        ({"../evil.dll": b"x"}, None),
        ({"Other/evil.dll": b"x"}, None),
        ({f"{PLUGIN}/TakaroConfig.txt": b"TAKARO_REGISTRATION_TOKEN=leak"}, None),
        ({f"{PLUGIN}/takaro.log": b"log"}, None),
        ({f"{PLUGIN}/state/bans.json": b"{}"}, None),
        ({}, "AsaApiLoader.exe"),
        ({}, "ArkApi/AsaApi.dll"),
        ({}, f"{PLUGIN}/TakaroAsa.dll"),
    ],
)
def test_deploy_refuses_a_zip_that_escapes_carries_state_or_is_incomplete(
    tmp_path: Path, extra: dict[str, bytes], missing: str | None
) -> None:
    entries = {**_release_entries(), **extra}
    if missing:
        del entries[missing]
    dest = tmp_path / "server"

    with pytest.raises((ConflictError, UsageError)):
        adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, entries))

    assert not (dest / WIN64).exists(), "nothing may be written before the zip is checked"


def test_deploy_refuses_another_role(tmp_path: Path) -> None:
    with pytest.raises(ConflictError):
        adapter_for(GAME).after_deploy(
            tmp_path, {**COMPONENT, "role": "plugin"}, _zip(tmp_path / ARTIFACT, _release_entries())
        )
