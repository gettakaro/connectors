"""The SCUM target: catalog, resolution, artifact naming, deploy.

The server is a Windows-only Steam depot; the connector is the UE4SS mod TakaroSCUM, shipped as
one zip that mirrors SCUM/Binaries/Win64 (the pinned UE4SS loader plus the mod). No runtime image
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

GAME = "scum"
TARGET = "windows-25803875"
APP = 3792580
DEPOT = "3792581"
MANIFEST = "6749284477071250233"
VERSION = "0.1.0-dev.abc1234"
ARTIFACT = f"takaro-scum-{TARGET}-{VERSION}.zip"
MOD = "ue4ss/Mods/TakaroSCUM"

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "scum.yml"


def record() -> dict[str, Any]:
    return json.loads(TARGET_PATH.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "SCUM")
    assert code == 0, err
    return dict(payload)


def test_catalog_validate_accepts_the_scum_target(run: Any) -> None:
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

    assert (server["app"], server["branch"], server["buildid"], server["os"]) == (APP, "public", 25803875, "windows")
    assert server["depots"][DEPOT]["manifest"] == MANIFEST
    assert server["credentials"] is None
    assert server["files"]["SCUM/Binaries/Win64/SCUMServer.exe"]["sha256"]


def test_the_build_pins_compiler_inputs_and_ue4ss_by_hash() -> None:
    deps = record()["build"]["deps"]

    assert set(deps) == {"xwin", "msvc-sysroot", "ue4ss"}
    assert all(len(dep["sha256"]) == 64 for dep in deps.values())
    assert deps["ue4ss"]["coordinate"] == "UE4SS-RE:RE-UE4SS:v3.0.1-1181-g83330635"


def test_the_target_claims_contract_and_keeps_operator_state() -> None:
    document = record()

    assert document["verification"]["required"] == "contract"
    assert [component["role"] for component in document["components"]] == ["mod"]
    for state in ("TakaroConfig.txt", "TakaroIdentity.txt", "known-players.json", "takaro-bans.json"):
        assert f"SCUM/Binaries/Win64/{MOD}/{state}" in document["preserve"]
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "runtime: false" in text
    assert "scripts/release-params.sh scum" in text


def test_resolve_names_the_artifact_and_build_inputs(run: Any) -> None:
    payload = resolve(run)

    assert payload["artifactFileNames"]["mod"] == f"takaro-scum-{TARGET}-{{version}}.zip"
    env = adapter_for(GAME).env(payload, "SCUM")
    assert env["SCUM_STEAM_BUILDID"] == "25803875"
    assert env["SCUM_ARTIFACT_MOD"] == f"takaro-scum-{TARGET}-{{version}}.zip"
    assert env["SCUM_DEP_UE4SS_URL"].endswith("/UE4SS_v3.0.1-1181-g83330635.zip")
    assert env["SCUM_DEP_MSVC_SYSROOT_COORDINATE"] == "microsoft:msvc-crt-14.44.17.14+winsdk:10.0.26100"


def test_the_game_version_banner_is_the_runtime_identity() -> None:
    line = "[2026.10.13-21.07.24:499][  0]LogSCUM: Game version: 1.3.4.2.161892"
    parsed = adapter_for(GAME).parse_runtime_identity(line)

    assert parsed == {"gameVersion": "1.3.4.2.161892", "loader": "ue4ss", "loaderVersion": None}


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
        "dwmapi.dll": b"loader",
        "ue4ss/UE4SS.dll": b"ue4ss",
        "ue4ss/UE4SS-settings.ini": b"[Hooks]\nHookEngineTick = 1\n",
        "ue4ss/Mods/mods.txt": b"TakaroSCUM : 1\r\n",
        f"{MOD}/Scripts/main.lua": b"-- new",
        f"{MOD}/dlls/main.dll": b"dll",
        f"{MOD}/takaro-target.json": json.dumps({"connectorVersion": VERSION}).encode(),
        "THIRD-PARTY-NOTICES.txt": b"MIT",
    }


COMPONENT = {"role": "mod", "artifact": ARTIFACT, "installDir": "SCUM/Binaries/Win64"}


def test_deploy_unpacks_over_win64_and_keeps_the_operator_config(tmp_path: Path) -> None:
    dest = tmp_path / "server"
    mod = dest / "SCUM/Binaries/Win64" / MOD
    mod.mkdir(parents=True)
    (mod / "TakaroConfig.txt").write_text("REGISTRATION_TOKEN=x\n")
    (mod / "Scripts").mkdir()
    (mod / "Scripts/main.lua").write_text("-- old")

    adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, _release_entries()))

    assert (mod / "Scripts/main.lua").read_text() == "-- new"
    assert (mod / "dlls/main.dll").read_bytes() == b"dll"
    assert (dest / "SCUM/Binaries/Win64/dwmapi.dll").read_bytes() == b"loader"
    assert (mod / "TakaroConfig.txt").read_text() == "REGISTRATION_TOKEN=x\n"


@pytest.mark.parametrize(
    ("extra", "missing"),
    [
        ({"../evil.dll": b"x"}, None),
        ({"Engine/evil.dll": b"x"}, None),
        ({f"{MOD}/TakaroConfig.txt": b"REGISTRATION_TOKEN=leak"}, None),
        ({}, f"{MOD}/dlls/main.dll"),
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

    assert not (dest / "SCUM/Binaries/Win64/dwmapi.dll").exists(), "nothing may be written before the zip is checked"
