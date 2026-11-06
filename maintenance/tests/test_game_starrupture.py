"""The StarRupture target: catalog, resolution, artifact naming, deploy.

The server is a Windows-only Steam depot; the connector is the ModLoader plugin TakaroStarRupture,
shipped as one zip that mirrors StarRupture/Binaries/Win64 (the pinned ModLoader plus the plugin).
No runtime image runs this server, so the target claims ``contract`` and the workflow passes
``runtime: false``.
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

GAME = "starrupture"
TARGET = "windows-25642172"
VERSION = "0.1.0-dev.abc1234"
ARTIFACT = f"takaro-starrupture-{TARGET}-{VERSION}.zip"
WIN64 = "StarRupture/Binaries/Win64"
PLUGINS = "ModLoader/Plugins"

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_PATH = REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "starrupture.yml"


def record() -> dict[str, Any]:
    return json.loads(TARGET_PATH.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def test_catalog_validate_accepts_the_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert rows
    assert all(check["status"] == "pass" for check in rows), rows


def test_the_steam_pin_is_exact_and_anonymous() -> None:
    server = record()["inputs"]["server"]

    assert (server["app"], server["branch"], server["buildid"], server["os"]) == (3809400, "public", 25642172, "windows")
    assert server["depots"]["3809401"]["manifest"] == "3659990775896995763"
    assert server["credentials"] is None


def test_the_build_pins_every_input_by_hash() -> None:
    deps = record()["build"]["deps"]

    assert set(deps) == {"xwin", "msvc-sysroot", "modloader", "plugin-sdk", "game-sdk"}
    assert all(len(dep["sha256"]) == 64 for dep in deps.values())


def test_the_target_claims_contract_and_keeps_operator_state() -> None:
    document = record()

    assert document["verification"]["required"] == "contract"
    assert [component["role"] for component in document["components"]] == ["mod"]
    for state in ("TakaroStarRupture.ini", "TakaroStarRupture.bans"):
        assert f"{WIN64}/{PLUGINS}/config/{state}" in document["preserve"]
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "runtime: false" in text
    assert "scripts/release-params.sh starrupture" in text


def test_resolve_names_the_artifact_and_build_inputs(run: Any) -> None:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "STARRUPTURE")
    assert code == 0, err

    env = adapter_for(GAME).env(dict(payload), "STARRUPTURE")
    assert env["STARRUPTURE_STEAM_BUILDID"] == "25642172"
    assert env["STARRUPTURE_ARTIFACT_MOD"] == f"takaro-starrupture-{TARGET}-{{version}}.zip"
    assert env["STARRUPTURE_DEP_MODLOADER_URL"].endswith("/StarRupture-ModLoader-Server-v1.22.1.zip")


def test_the_loader_version_line_is_the_runtime_identity() -> None:
    line = "[19:14:46.769] [INFO ] [T:16304] [ModLoader] Game version OK: ++Earth20+Neon-CL-127004"

    assert adapter_for(GAME).parse_runtime_identity(line) == {
        "gameVersion": "CL-127004",
        "loader": "modloader",
        "loaderVersion": None,
    }


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
        "ModLoader/StarRupture-ModLoader-Core.dll": b"core",
        "ModLoader/StarRupture-ModLoader-Core.pdb": b"pdb",
        f"{PLUGINS}/TakaroStarRupture.dll": b"new",
        f"{PLUGINS}/config/TakaroStarRupture.example.ini": b"[Takaro]\n",
        "THIRD-PARTY-NOTICES.txt": b"MIT",
    }


COMPONENT = {"role": "mod", "artifact": ARTIFACT, "installDir": WIN64}


def test_deploy_unpacks_over_win64_and_keeps_the_operator_config(tmp_path: Path) -> None:
    dest = tmp_path / "server"
    config = dest / WIN64 / PLUGINS / "config"
    config.mkdir(parents=True)
    (config / "TakaroStarRupture.ini").write_text("RegistrationToken=x\n")

    adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, _release_entries()))

    assert (dest / WIN64 / PLUGINS / "TakaroStarRupture.dll").read_bytes() == b"new"
    assert (dest / WIN64 / "dwmapi.dll").read_bytes() == b"loader"
    assert (config / "TakaroStarRupture.ini").read_text() == "RegistrationToken=x\n"


@pytest.mark.parametrize(
    ("extra", "missing"),
    [
        ({"../evil.dll": b"x"}, None),
        ({"Engine/evil.dll": b"x"}, None),
        ({f"{PLUGINS}/config/TakaroStarRupture.ini": b"RegistrationToken=leak"}, None),
        ({}, f"{PLUGINS}/TakaroStarRupture.dll"),
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

    assert not (dest / WIN64 / "dwmapi.dll").exists(), "nothing may be written before the zip is checked"
