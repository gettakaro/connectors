"""The Team Fortress 2 targets: catalog, resolution, artifact naming, deploy.

The server is a Steam depot install (app 232250, Linux or Windows); the connector is the SourceMod
plugin takaro_tf2.smx plus the ProjectSky websocket and json extensions, shipped as one zip per
platform that mirrors the server's tf/ folder. No runtime image runs this server, so the targets
claim ``contract`` and the workflow passes ``runtime: false``.
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

GAME = "tf2"
BUILD = 25995664
TARGETS = (f"linux-{BUILD}", f"windows-{BUILD}")
APP = 232250
VERSION = "0.1.0-dev.abc1234"
SM = "addons/sourcemod"

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "tf2.yml"


def record(target: str) -> dict[str, Any]:
    path = REPO_ROOT / "catalog" / GAME / "targets" / f"{target}.json"
    return json.loads(path.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def resolve(run: Any, target: str) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", target, "--prefix", "TF2")
    assert code == 0, err
    return dict(payload)


def test_catalog_validate_accepts_both_tf2_targets(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    for target in TARGETS:
        rows = [check for check in payload["checks"] if check["file"].endswith(f"{target}.json")]
        assert {check["id"] for check in rows} >= {
            "input-kind-schema",
            "build-system-schema",
            "build-script-exists",
            "no-null-hash",
            "immutable-tag-and-digest",
        }
        assert all(check["status"] == "pass" for check in rows), rows


@pytest.mark.parametrize(
    ("target", "platform", "binary"),
    [(TARGETS[0], "linux", "srcds_linux"), (TARGETS[1], "windows", "srcds.exe")],
)
def test_the_steam_pin_is_exact_and_anonymous(target: str, platform: str, binary: str) -> None:
    server = record(target)["inputs"]["server"]

    assert (server["app"], server["branch"], server["buildid"], server["os"]) == (APP, "public", BUILD, platform)
    assert "232250" in server["depots"]
    assert server["credentials"] is None
    assert len(server["files"][binary]["sha256"]) == 64
    assert record(target)["default"] is True


def test_the_build_pins_the_compiler_and_extensions_by_hash() -> None:
    linux = record(TARGETS[0])["build"]["deps"]
    windows = record(TARGETS[1])["build"]["deps"]

    shared = {"sourcemod", "websocket-license", "json-license", "websocket-linux", "json-linux"}
    assert set(linux) == shared
    # The Windows zip ships the Windows binaries; the plugin still compiles against the Linux includes.
    assert set(windows) == shared | {"websocket-windows", "json-windows"}
    for deps in (linux, windows):
        assert all(len(dep["sha256"]) == 64 for dep in deps.values())
        assert deps["sourcemod"]["coordinate"] == "alliedmodders:sourcemod-linux:1.13.0-git7477"


def test_the_targets_claim_contract_and_keep_operator_state() -> None:
    for target in TARGETS:
        document = record(target)
        assert document["verification"]["required"] == "contract"
        assert [component["role"] for component in document["components"]] == ["plugin"]
        assert document["components"][0]["installDir"] == "tf"
        assert "tf/cfg/sourcemod/takaro_tf2.cfg" in document["preserve"]
        assert "tf/addons/sourcemod/data/takaro_tf2_bans.txt" in document["preserve"]
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "runtime: false" in text
    assert "scripts/release-params.sh tf2" in text


@pytest.mark.parametrize("target", TARGETS)
def test_resolve_names_the_artifact_and_build_inputs(run: Any, target: str) -> None:
    payload = resolve(run, target)

    assert payload["artifactFileNames"]["plugin"] == f"takaro-tf2-{target}-{{version}}.zip"
    env = adapter_for(GAME).env(payload, "TF2")
    assert env["TF2_STEAM_BUILDID"] == str(BUILD)
    assert env["TF2_PLATFORM"] == target.split("-")[0]
    assert env["TF2_ARTIFACT_PLUGIN"] == f"takaro-tf2-{target}-{{version}}.zip"
    assert env["TF2_DEP_SOURCEMOD_URL"].endswith("/sourcemod-1.13.0-git7477-linux.tar.gz")
    assert env["TF2_DEP_WEBSOCKET_LINUX_URL"].endswith("/sm-ext-websocket-linux-sm1.13-06e2cd0.zip")


def test_the_version_command_output_is_the_runtime_identity() -> None:
    parsed = adapter_for(GAME).parse_runtime_identity("Exe version 11138802 (tf)")

    assert parsed == {"gameVersion": "11138802", "loader": "sourcemod", "loaderVersion": None}


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
        f"{SM}/plugins/takaro_tf2.smx": b"new",
        f"{SM}/extensions/websocket.ext.so": b"ws",
        f"{SM}/extensions/json.ext.so": b"json",
        f"{SM}/takaro-target.json": json.dumps({"connectorVersion": VERSION}).encode(),
        "cfg/sourcemod/takaro_tf2.cfg": b'takaro_tf2_registration_token ""\n',
    }


ARTIFACT = f"takaro-tf2-{TARGETS[0]}-{VERSION}.zip"
COMPONENT = {"role": "plugin", "artifact": ARTIFACT, "installDir": "tf"}


def test_deploy_unpacks_over_tf_and_keeps_the_operator_config(tmp_path: Path) -> None:
    dest = tmp_path / "server"
    tf = dest / "tf"
    (tf / "cfg/sourcemod").mkdir(parents=True)
    (tf / "cfg/sourcemod/takaro_tf2.cfg").write_text('takaro_tf2_registration_token "x"\n')
    (tf / f"{SM}/plugins").mkdir(parents=True)
    (tf / f"{SM}/plugins/takaro_tf2.smx").write_bytes(b"old")

    adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, _release_entries()))

    assert (tf / f"{SM}/plugins/takaro_tf2.smx").read_bytes() == b"new"
    assert (tf / f"{SM}/extensions/websocket.ext.so").read_bytes() == b"ws"
    assert (tf / "cfg/sourcemod/takaro_tf2.cfg").read_text() == 'takaro_tf2_registration_token "x"\n'


def test_a_first_deploy_writes_the_config_template(tmp_path: Path) -> None:
    dest = tmp_path / "server"

    adapter_for(GAME).after_deploy(dest, COMPONENT, _zip(tmp_path / ARTIFACT, _release_entries()))

    assert (dest / "tf/cfg/sourcemod/takaro_tf2.cfg").read_text() == 'takaro_tf2_registration_token ""\n'


@pytest.mark.parametrize(
    ("extra", "missing"),
    [
        ({"../evil.so": b"x"}, None),
        ({"maps/evil.bsp": b"x"}, None),
        ({"cfg/server.cfg": b"rcon_password x"}, None),
        ({"addons/sourcemod/configs/admins_simple.ini": b'"STEAM_0:1:1" "99:z"'}, None),
        ({f"{SM}/data/takaro_tf2_bans.txt": b"76561198000000000"}, None),
        ({}, f"{SM}/plugins/takaro_tf2.smx"),
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

    assert not (dest / "tf").exists(), "nothing may be written before the zip is checked"
