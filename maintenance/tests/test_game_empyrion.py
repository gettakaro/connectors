"""The Empyrion target: catalog, resolution, artifact naming, deploy.

The server is a Windows-only Steam depot; the connector is the C# mod TakaroEmpyrion, shipped as
one zip that mirrors Content/Mods (a single TakaroEmpyrion/ folder). No runtime image runs this
server, so the target claims ``contract`` and the workflow passes ``runtime: false``.
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

GAME = "empyrion"
TARGET = "windows-25490402"
APP = 530870
DEPOT = "530871"
MANIFEST = "8329430202076268665"
VERSION = "0.1.0-dev.abc1234"
ARTIFACT = f"takaro-empyrion-{TARGET}-{VERSION}.zip"
MOD = "TakaroEmpyrion"
MANAGED = "DedicatedServer/EmpyrionDedicated_Data/Managed"

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "empyrion.yml"


def record() -> dict[str, Any]:
    return json.loads(TARGET_PATH.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "EMPYRION")
    assert code == 0, err
    return dict(payload)


def test_catalog_validate_accepts_the_empyrion_target(run: Any) -> None:
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

    assert (server["app"], server["branch"], server["buildid"], server["os"]) == (APP, "public", 25490402, "windows")
    assert server["depots"][DEPOT]["manifest"] == MANIFEST
    assert server["credentials"] is None
    assert server["files"]["DedicatedServer/EmpyrionDedicated.exe"]["sha256"]


def test_the_build_compiles_against_exactly_the_three_pinned_server_assemblies() -> None:
    document = record()
    files = document["inputs"]["server"]["files"]
    references = document["build"]["references"]

    assert references == [f"{MANAGED}/{name}" for name in ("Mif.dll", "ModApi.dll", "Newtonsoft.Json.dll")]
    assert all(len(files[selector]["sha256"]) == 64 for selector in references)
    assert document["default"] is True
    assert document["build"]["toolchain"]["image"] == "mono"
    assert document["build"]["deps"]["netstandard.dll"]["coordinate"].startswith("image-file:")


def test_the_target_claims_contract_and_keeps_operator_state() -> None:
    document = record()

    assert document["verification"]["required"] == "contract"
    assert [component["role"] for component in document["components"]] == ["mod"]
    assert document["components"][0]["installDir"] == "Content/Mods"
    assert "Saves/" in document["preserve"]
    for state in ("TakaroConfig.txt", "players.json", "bans.json", "Logs/"):
        assert f"Content/Mods/{MOD}/{state}" in document["preserve"]
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "runtime: false" in text
    assert "scripts/release-params.sh empyrion" in text


def test_resolve_names_the_artifact_and_build_inputs(run: Any) -> None:
    payload = resolve(run)

    assert payload["artifactFileNames"]["mod"] == f"takaro-empyrion-{TARGET}-{{version}}.zip"
    env = adapter_for(GAME).env(payload, "EMPYRION")
    assert env["EMPYRION_STEAM_BUILDID"] == "25490402"
    assert env["EMPYRION_ARTIFACT_MOD"] == f"takaro-empyrion-{TARGET}-{{version}}.zip"
    assert env["EMPYRION_TOOLCHAIN"].startswith("mono:6.12.0.182@sha256:")
    assert [entry.split(":", 1)[0] for entry in env["EMPYRION_REFERENCES"].split(";")] == [
        "Mif.dll",
        "ModApi.dll",
        "Newtonsoft.Json.dll",
    ]
    assert env["EMPYRION_DEP_NETSTANDARD_DLL_COORDINATE"].endswith(":/usr/lib/mono/4.5/Facades/netstandard.dll")


@pytest.mark.parametrize(
    ("line", "parsed"),
    [
        (
            "03-13:49:46.770 13_49 -LOG- Version: v1.19.6",
            {"gameVersion": "1.19.6", "loader": "unity", "loaderVersion": None},
        ),
        (
            "Initialize engine version: 2022.3.62f2 (7670c08855a9)",
            {"gameVersion": None, "loader": "unity", "loaderVersion": "2022.3.62f2"},
        ),
        ("03-13:49:46.771 13_49 -LOG- Build: 5188", None),
    ],
)
def test_the_dedicated_server_banner_is_the_runtime_identity(line: str, parsed: dict[str, Any] | None) -> None:
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
        f"{MOD}/TakaroEmpyrion.dll": b"dll",
        f"{MOD}/TakaroEmpyrion_Info.yaml": b"Name: Takaro\nModTargets: Dedi, PfServer\n",
        f"{MOD}/TakaroConfig.txt.example": b"REGISTRATION_TOKEN=\n",
        f"{MOD}/takaro-target.json": json.dumps({"connectorVersion": VERSION}).encode(),
        f"{MOD}/PROVENANCE.txt": b"pinned",
    }


COMPONENT = {"role": "mod", "artifact": ARTIFACT, "installDir": "Content/Mods"}


def test_deploy_unpacks_into_content_mods_and_keeps_the_operator_state(tmp_path: Path) -> None:
    dest = tmp_path / "server"
    mod = dest / "Content/Mods" / MOD
    (mod / "Logs").mkdir(parents=True)
    (mod / "TakaroConfig.txt").write_text("REGISTRATION_TOKEN=x\n")
    (mod / "players.json").write_text("{}")
    (mod / "Logs/takaro.log").write_text("old log")
    (mod / "TakaroEmpyrion.dll").write_bytes(b"old")

    adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, _release_entries()))

    assert (mod / "TakaroEmpyrion.dll").read_bytes() == b"dll"
    assert (mod / "TakaroConfig.txt").read_text() == "REGISTRATION_TOKEN=x\n"
    assert (mod / "players.json").read_text() == "{}"
    assert (mod / "Logs/takaro.log").read_text() == "old log"


@pytest.mark.parametrize(
    ("extra", "missing"),
    [
        ({"../evil.dll": b"x"}, None),
        ({"OtherMod/evil.dll": b"x"}, None),
        ({f"{MOD}/TakaroConfig.txt": b"REGISTRATION_TOKEN=leak"}, None),
        ({f"{MOD}/bans.json": b"{}"}, None),
        ({f"{MOD}/Logs/takaro.log": b"log"}, None),
        ({}, f"{MOD}/TakaroEmpyrion.dll"),
        ({}, f"{MOD}/TakaroEmpyrion_Info.yaml"),
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

    assert not (dest / "Content/Mods" / MOD).exists(), "nothing may be written before the zip is checked"


def test_deploy_refuses_another_role(tmp_path: Path) -> None:
    with pytest.raises(ConflictError):
        adapter_for(GAME).after_deploy(
            tmp_path, {**COMPONENT, "role": "plugin"}, _zip(tmp_path / ARTIFACT, _release_entries())
        )
