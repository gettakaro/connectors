"""The Terraria target end to end: catalog, resolution, install, build, deploy, verify.

Terraria is the first game here whose server is a container image and whose connector is
two artifacts — a TShock plugin and a Node bridge. So the assertions are about what a
maintainer, the rig, the harness and CI observe: exit codes, the JSON on stdout, the files
on disk and the argv the runner hands docker. Every command is the real one.

The registry half runs the real provider against an in-process stand-in serving the raw
manifest bytes recorded from ghcr.io, because the digest a manifest hashes to is the whole
point and a re-serialised fixture would not have the same one.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import re
import subprocess
import urllib.error
import zipfile
from dataclasses import dataclass, field
from io import BytesIO
from pathlib import Path
from typing import Any

import pytest

from conftest import REPO_ROOT
from fake_github import FakeGitHub
from fake_upstream import FakeUpstream
from fake_verify import CannedSocket
from fake_verify import FakeRun as VerifyRun
from takaro_maint import net, readiness
from takaro_maint.games import adapter_for
from takaro_maint.games.terraria import verify as hooks
from takaro_maint.providers import provider_for
from takaro_maint.publish.manifest import artifact_row, write_manifest, write_meta

GAME = "terraria"
TARGET = "tshock-v6.2.1"
VERSION = "0.2.2-dev.abc1234"
PLUGIN_ZIP = f"takaro-terraria-plugin-{TARGET}-{VERSION}.zip"
BRIDGE_ZIP = f"takaro-terraria-bridge-{TARGET}-{VERSION}.zip"
BUILD_SCRIPT = "games/terraria/scripts/build-release.sh"

IMAGE_REF = "ghcr.io/pryaxis/tshock:6.2.1@sha256:70e59a8e6b4c79b5fad469d320962c1fc98625ed3a955510a2f2641dbff2f7e7"
TSHOCKAPI_SHA = "d58260463d29f61ecc4590bff129a5ca1761b33e0ce6ede55c431aaa1b663705"
ASSET_URL = (
    "https://github.com/Pryaxis/TShock/releases/download/v6.2.1/TShock-6.2.1-for-Terraria-1.4.5.8-linux-x64-Release.zip"
)

FIXTURES = Path(__file__).parent / "fixtures"
OCI = FIXTURES / "providers" / "oci_registry"
TERRARIA_FIXTURES = FIXTURES / "games" / "terraria"

REPOSITORY = "pryaxis/tshock"
TAGS_PATH = f"/v2/{REPOSITORY}/tags/list?n=1000"

#: The digests the recorded manifest bodies hash to. Asserted, not assumed: a fixture that
#: was reformatted would silently stop being the document ghcr.io served.
INDEX_DIGESTS = {
    "6.1.0": "911459f0ce02014a64c197647a16e9ee57e4d16695de8cfda1f1b552af56ab43",
    "6.0.0": "5131efd03a96fc048500a75afcc32248d204242156ef50bd6ad8e4750e28a45a",
    "stable": "b40db2c722aabcf4a3a757f761f9b85f0d7208da824f77d03e4ae183871a35fc",
}
PLATFORM_DIGEST = "sha256:29f877e073490b0f12977fa09bab8b910708a6e9e2dd38eabe76ec6d577d2e4d"
CONFIG_DIGEST = "sha256:0a40c4aa2c47ae237c311a89ab9cc8ce23ccea0ce165fcd8f4d69fa5d24ac448"


def test_every_terraria_verification_body_has_pass_and_failure_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    items = [{"code": str(index), "name": f"Item {index}"} for index in range(hooks.CATALOGUE_MINIMUM)]
    items[9] = {"code": "9", "name": "Wood"}
    run = VerifyRun(tmp_path)
    run.resolved = {
        "containerRef": "image@sha256:" + "a" * 64,
        "build": {
            "references": ["/tshock/TShockAPI.dll"],
            "deps": {"TShockAPI.dll": {"sha256": "b" * 64}},
        },
    }
    fake = CannedSocket(
        {
            "listItems": items,
            "listEntities": [],
            "sendMessage": {"success": True},
            "testReachability": {"connectable": True},
        },
        identify_count=1,
    )
    monkeypatch.setattr(hooks.checks, "wait_for_line", lambda *args, **kwargs: (1, "matched"))
    monkeypatch.setattr(hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=1))
    monkeypatch.setattr(hooks.checks_lifecycle, "wait_for_count", lambda *args, **kwargs: 2)
    monkeypatch.setattr(hooks, "_hash_in_container", lambda *args: "b" * 64)
    monkeypatch.setattr(
        hooks.subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(args[0], 0, "", ""),
    )

    assert asyncio.run(hooks._check_handshake(run, fake, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_items(run, fake)).status == "pass"
    assert asyncio.run(hooks._check_entities(run, fake)).status == "pass"
    assert asyncio.run(hooks._check_action(run, fake, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_references(run)).status == "pass"
    assert asyncio.run(hooks._check_reconnect(run, fake, lambda: True)).status == "pass"
    asyncio.run(hooks.after_protocol(run, fake, lambda: True))
    asyncio.run(hooks.after_shutdown(run, fake, run.ws_url, []))

    failed_run = VerifyRun(tmp_path / "failed", wanted=set())
    failed_run.resolved = run.resolved
    failed = CannedSocket(
        {
            "listItems": RuntimeError("no items"),
            "listEntities": ["unexpected"],
            "sendMessage": {"success": False},
            "testReachability": None,
        },
        reconnects=False,
    )
    monkeypatch.setattr(hooks.checks, "wait_for_line", lambda *args, **kwargs: None)
    monkeypatch.setattr(
        hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=None)
    )
    monkeypatch.setattr(hooks.checks_lifecycle, "wait_for_count", lambda *args, **kwargs: 0)
    monkeypatch.setattr(hooks, "_hash_in_container", lambda *args: None)
    monkeypatch.setattr(
        hooks.subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(args[0], 1, "", "cannot chown"),
    )

    assert asyncio.run(hooks._check_handshake(failed_run, failed, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_items(failed_run, failed)).status == "fail"
    assert asyncio.run(hooks._check_entities(failed_run, failed)).status == "fail"
    assert asyncio.run(hooks._check_action(failed_run, failed, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_references(failed_run)).status == "fail"
    assert asyncio.run(hooks._check_reconnect(failed_run, failed, lambda: False)).status == "fail"
    asyncio.run(hooks.after_protocol(failed_run, failed, lambda: False))
    asyncio.run(hooks.after_shutdown(failed_run, failed, failed_run.ws_url, []))
    assert {check for check, _ in failed_run.skips} == set(hooks.CHECK_IDS)


# -- the repository copy ------------------------------------------------------------------


def target_path(root: Path) -> Path:
    return root / "catalog" / GAME / "targets" / f"{TARGET}.json"


def read_target(root: Path) -> dict[str, Any]:
    return json.loads(target_path(root).read_text(encoding="utf-8"))


def write_target(root: Path, record: dict[str, Any]) -> None:
    target_path(root).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def read_game(root: Path) -> dict[str, Any]:
    return json.loads((root / "catalog" / GAME / "game.json").read_text(encoding="utf-8"))


def write_game(root: Path, record: dict[str, Any]) -> None:
    (root / "catalog" / GAME / "game.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    """A repository copy holding the real catalog, the tool lock and a stub build script."""
    root = tmp_path / "repo"
    (root / "catalog").mkdir(parents=True)
    for game_dir in sorted((REPO_ROOT / "catalog").iterdir()):
        if game_dir.is_dir():
            (root / "catalog" / game_dir.name).mkdir(exist_ok=True)
            for item in game_dir.rglob("*"):
                if item.is_file():
                    destination = root / "catalog" / game_dir.name / item.relative_to(game_dir)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(item.read_bytes())
    (root / "maintenance").mkdir(parents=True, exist_ok=True)
    (root / "maintenance" / "tools.lock.json").write_bytes((REPO_ROOT / "maintenance" / "tools.lock.json").read_bytes())
    script = root / BUILD_SCRIPT
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)
    return root


@dataclass
class Pinned:
    """A repository copy whose Terraria download points at an in-process upstream."""

    root: Path
    upstream: FakeUpstream
    asset_path: str
    payload: bytes


@pytest.fixture
def pinned(repo: Path) -> Any:
    """Re-pin ``inputs.server`` at tiny bytes served locally, so a real install can run."""
    with FakeUpstream() as upstream:
        payload = b"a stand-in for the 34 MB TShock release zip\n"
        record = read_target(repo)
        asset = record["inputs"]["server"]
        asset_path = f"/{asset['repo']}/releases/download/{asset['tag']}/{asset['asset']}"
        asset["sha256"] = hashlib.sha256(payload).hexdigest()
        asset["size"] = len(payload)
        write_target(repo, record)
        game = read_game(repo)
        game["sources"]["tshock-download"]["baseUrl"] = upstream.base_url
        write_game(repo, game)
        upstream.add(asset_path, payload)
        yield Pinned(root=repo, upstream=upstream, asset_path=asset_path, payload=payload)


def resolve(run: Any, repo: Path) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, repo=repo)
    assert code == 0, err
    return dict(payload)


# -- catalog ------------------------------------------------------------------------------


def test_catalog_validate_accepts_the_terraria_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert {check["id"] for check in rows} >= {
        "input-kind-schema",
        "build-system-schema",
        "build-script-exists",
        "immutable-tag-and-digest",
        "no-null-hash",
        "no-floating-words",
    }
    assert all(check["status"] == "pass" for check in rows), rows


def test_a_stable_image_tag_is_refused(run: Any, repo: Path) -> None:
    """A floating tag such as `stable` is never a pin: validation has to refuse it."""
    record = read_target(repo)
    record["runtime"]["container"]["tag"] = "stable"
    write_target(repo, record)

    code, payload, _ = run("catalog", "validate", repo=repo)

    assert code == 2, payload
    failed = {row["id"] for row in payload["failures"] if row["file"].endswith(f"{TARGET}.json")}
    assert {"immutable-tag-and-digest", "no-floating-words"} <= failed


def test_targets_resolve_env_for_terraria(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "TERRARIA")

    assert code == 0, payload
    env = payload["env"]
    assert env["TERRARIA_IMAGE"] == IMAGE_REF
    assert env["TERRARIA_TSHOCK_TAG"] == "6.2.1"
    assert env["TERRARIA_BRIDGE_IMAGE"].startswith("docker.io/library/node:22.23.2-bookworm-slim@sha256:")
    assert env["TERRARIA_PLUGIN_ARTIFACT"] == f"takaro-terraria-plugin-{TARGET}-{{version}}.zip"
    assert env["TERRARIA_BRIDGE_ARTIFACT"] == f"takaro-terraria-bridge-{TARGET}-{{version}}.zip"
    assert env["TERRARIA_DEP_TSHOCKAPI_DLL_SHA256"] == TSHOCKAPI_SHA
    assert env["TERRARIA_REFERENCES"] == (
        "/server/ServerPlugins/TShockAPI.dll;/server/bin/OTAPI.dll;/server/bin/TerrariaServer.dll"
    )
    assert env["TERRARIA_REFERENCES_DIR"].endswith(payload["fp16"])
    # This server runs on .NET; nothing here may hand a script a Java runtime.
    assert not any(key.endswith("_JAVA") for key in env)
    assert payload["resolvedUrls"]["server"] == ASSET_URL


# -- install ------------------------------------------------------------------------------


def tree_hash(root: Path) -> str:
    from takaro_maint.commands.install import tree_hash as _tree_hash

    return str(_tree_hash(root))


def test_install_places_the_pinned_distribution_and_refuses_altered_or_missing_bytes(
    run: Any, pinned: Any, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    dest = tmp_path / "terraria"

    code, payload, err = run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=pinned.root)

    assert code == 0, f"{err}\n{payload}"
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    assert ledger["inputs"][0]["path"] == "inputs/TShock-6.2.1-for-Terraria-1.4.5.8-linux-x64-Release.zip"
    assert ledger["inputs"][0]["sha256"] == hashlib.sha256(pinned.payload).hexdigest()
    assert (dest / ledger["inputs"][0]["path"]).read_bytes() == pinned.payload

    code, payload, _ = run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=pinned.root)
    assert code == 0
    assert payload["status"] == "already-installed"
    assert run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=pinned.root)[0] == 0

    # Upstream serving other bytes under the pinned name: refused, and nothing on disk moves.
    # The download cache is content-addressed, so it is emptied first — otherwise the run
    # would be served the good bytes it already holds and prove nothing.
    before = tree_hash(dest)
    monkeypatch.setenv("TAKARO_MAINT_CACHE", str(tmp_path / "cold-cache"))
    pinned.upstream.add(pinned.asset_path, b"upstream replaced the asset in place\n")
    other = tmp_path / "second"
    code, payload, _ = run("install", "--game", GAME, "--target", TARGET, "--dest", str(other), repo=pinned.root)
    assert code == 5, payload
    assert tree_hash(dest) == before

    # And an asset that is simply gone is an upstream failure, never a fallback.
    pinned.upstream.status_overrides[pinned.asset_path] = 404
    code, payload, _ = run(
        "install", "--game", GAME, "--target", TARGET, "--dest", str(tmp_path / "third"), repo=pinned.root
    )
    assert code == 4, payload


# -- build --------------------------------------------------------------------------------


def write_build_stub(
    repo: Path,
    fingerprint: str,
    *,
    plugin: str | None = PLUGIN_ZIP,
    bridge: str | None = BRIDGE_ZIP,
) -> None:
    """A stand-in for the real release script: the same contract, no .NET and no Node."""
    names = json.dumps({"plugin": plugin, "bridge": bridge})
    script = repo / BUILD_SCRIPT
    script.write_text(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        'version="$1"; out="$2"\n'
        'mkdir -p "$out"\n'
        f"NAMES='{names}' FINGERPRINT='{fingerprint}' python3 - \"$version\" \"$out\" <<'PY'\n"
        "import json, os, sys, zipfile\n"
        "from pathlib import Path\n"
        "version, out = sys.argv[1], Path(sys.argv[2])\n"
        "names = json.loads(os.environ['NAMES'])\n"
        "bodies = {\n"
        "    'plugin': ('TakaroTerrariaEvents', {'TakaroTerrariaEvents.dll': 'assembly',\n"
        "                                        'README.txt': 'install me'}),\n"
        "    'bridge': ('TakaroTerrariaBridge', {'dist/index.js': 'bridge',\n"
        "                                        'package.json': '{}'}),\n"
        "}\n"
        "for role, name in names.items():\n"
        "    if not name:\n"
        "        continue\n"
        "    name = name.replace('{version}', version)\n"
        "    folder, files = bodies[role]\n"
        "    with zipfile.ZipFile(out / name, 'w') as archive:\n"
        "        for path, body in files.items():\n"
        "            archive.writestr(f'{folder}/{path}', body)\n"
        "    (out / (name + '.meta.json')).write_text(json.dumps({\n"
        "        'target': %r, 'fingerprint': os.environ['FINGERPRINT'], 'connectorVersion': version,\n"
        "        'game': 'terraria', 'platform': 'tshock', 'revision': 'v6.2.1', 'role': role}))\n"
        "PY\n" % TARGET,
        encoding="utf-8",
    )
    script.chmod(0o755)


def test_build_selects_both_artifacts_and_their_meta(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(repo, resolved["fingerprint"])
    out = tmp_path / "dist"

    code, payload, err = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(out), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert [row["role"] for row in payload["artifacts"]] == ["bridge", "plugin"]
    for name in (PLUGIN_ZIP, BRIDGE_ZIP):
        assert (out / name).is_file()
        assert (out / f"{name}.meta.json").is_file()
    manifest = json.loads((out / "build-manifest.json").read_text())
    assert {row["fingerprint"] for row in manifest["artifacts"]} == {resolved["fingerprint"]}

    # One role missing is not "a partial release": it is a refusal. The build script writes
    # into the target's own dist directory, so the first build's output is cleared first —
    # otherwise the missing role would be served by bytes from the run before.
    import shutil as _shutil

    _shutil.rmtree(repo / "games" / "terraria" / "_data" / "dist", ignore_errors=True)
    write_build_stub(repo, resolved["fingerprint"], bridge=None)
    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "half"), repo=repo
    )
    assert code == 7, payload
    assert "did not produce" in payload["error"]


def test_a_build_that_writes_the_legacy_names_is_refused(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(
        repo,
        resolved["fingerprint"],
        plugin="takaro-terraria-plugin.zip",
        bridge="takaro-terraria-bridge.zip",
    )

    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "dist"), repo=repo
    )

    assert code == 7, payload
    assert "did not produce" in payload["error"]


# -- deploy -------------------------------------------------------------------------------


def make_zip(path: Path, folder: str, files: dict[str, str], *, escape: bool = False) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for name, body in files.items():
            archive.writestr(f"{folder}/{name}", body)
        if escape:
            archive.writestr("../escaped.txt", "nope")
    return path


def plugin_zip(directory: Path, **kwargs: Any) -> Path:
    return make_zip(
        directory / PLUGIN_ZIP,
        "TakaroTerrariaEvents",
        {"TakaroTerrariaEvents.dll": "assembly", "README.txt": "install me"},
        **kwargs,
    )


def bridge_zip(directory: Path, **kwargs: Any) -> Path:
    return make_zip(
        directory / BRIDGE_ZIP,
        "TakaroTerrariaBridge",
        {"dist/index.js": "bridge", "package.json": "{}"},
        **kwargs,
    )


def manifest_for(run: Any, repo: Path, directory: Path, *, target: str = TARGET) -> Path:
    resolved = resolve(run, repo)
    fingerprint = resolved["fingerprint"]
    rows = [
        artifact_row("plugin", target, fingerprint, directory / PLUGIN_ZIP),
        artifact_row("bridge", target, fingerprint, directory / BRIDGE_ZIP),
    ]
    for row in rows:
        write_meta(directory, row, connector=GAME, version=VERSION, revision="deadbeef")
    return write_manifest(
        directory,
        connector=GAME,
        version=VERSION,
        revision="deadbeef",
        dirty=False,
        toolchain=resolved["build"]["toolchain"],
        mode="container",
        artifacts=rows,
    )


def deploy(run: Any, repo: Path, dest: Path, manifest: Path) -> tuple[int, Any, str]:
    return run("deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo)


def installed(run: Any, pinned: Any, dest: Path) -> None:
    code, payload, err = run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=pinned.root)
    assert code == 0, f"{err}\n{payload}"


def test_deploy_unpacks_the_plugin_dll_and_the_bridge_folder_and_keeps_the_operator_config(
    run: Any, pinned: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    # What an operator already has: their own bridge configuration and an older plugin zip.
    (dest / "bridge").mkdir(parents=True, exist_ok=True)
    config = dest / "bridge" / "TakaroConfig.txt"
    config.write_text("registrationToken=the-operator-put-this-here\n")
    config_before = config.read_bytes()
    (dest / "plugins").mkdir(parents=True, exist_ok=True)
    stale = dest / "plugins" / f"takaro-terraria-plugin-{TARGET}-0.2.0.zip"
    stale.write_bytes(b"an older deploy")

    directory = tmp_path / "dist"
    plugin_zip(directory)
    bridge_zip(directory)
    manifest = manifest_for(run, pinned.root, directory)

    code, payload, err = deploy(run, pinned.root, dest, manifest)

    assert code == 0, f"{err}\n{payload}"
    assert (dest / "plugins" / "TakaroTerrariaEvents.dll").read_text() == "assembly"
    assert not stale.exists()
    assert (dest / "bridge" / "TakaroTerrariaBridge" / "dist" / "index.js").read_text() == "bridge"
    assert config.read_bytes() == config_before
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    # Both roles are attested, each with the path it actually landed at.
    by_role = {row["role"]: row["path"] for row in ledger["artifacts"]}
    assert by_role["plugin"].endswith(PLUGIN_ZIP)
    assert by_role["bridge"].endswith(BRIDGE_ZIP)


def test_ledger_check_guards_every_deployed_role(run: Any, pinned: Any, tmp_path: Path) -> None:
    """Both roles, not just the last one deployed, are what `ledger check` answers for.

    A two-role game deploys twice, so a ledger that remembered only the final role left
    the first artifact unattested: tampering with it, or deleting it, read as a clean
    install.
    """
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    plugin_zip(directory)
    bridge_zip(directory)
    assert deploy(run, pinned.root, dest, manifest_for(run, pinned.root, directory))[0] == 0

    def check() -> tuple[int, Any]:
        code, payload, _ = run(
            "ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=pinned.root
        )
        return code, payload

    assert check()[0] == 0

    ledger_file = dest / ".takaro" / "installed-target.json"
    clean = ledger_file.read_bytes()
    rows = {row["role"]: dest / row["path"] for row in json.loads(clean)["artifacts"]}
    assert set(rows) == {"plugin", "bridge"}

    for role, path in rows.items():
        kept = path.read_bytes()
        path.write_bytes(kept + b"tampered")
        code, payload = check()
        assert code == 7, (role, payload)
        assert role in json.dumps(payload), (role, payload)
        path.unlink()
        code, payload = check()
        assert code == 7, (role, payload)
        assert role in json.dumps(payload) and "missing artifact" in json.dumps(payload), (role, payload)
        path.write_bytes(kept)
        assert check()[0] == 0, role


def test_a_legacy_single_artifact_ledger_still_checks_and_is_upgraded(run: Any, pinned: Any, tmp_path: Path) -> None:
    """Ledgers already on rigs carry one `artifact` object; they keep working."""
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    plugin_zip(directory)
    bridge_zip(directory)
    assert deploy(run, pinned.root, dest, manifest_for(run, pinned.root, directory))[0] == 0

    ledger_file = dest / ".takaro" / "installed-target.json"
    data = json.loads(ledger_file.read_text(encoding="utf-8"))
    legacy = [row for row in data.pop("artifacts") if row["role"] == "bridge"][0]
    data["artifact"] = legacy
    ledger_file.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    code, payload, _ = run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=pinned.root)
    assert code == 0, payload

    assert deploy(run, pinned.root, dest, manifest_for(run, pinned.root, directory))[0] == 0
    upgraded = json.loads(ledger_file.read_text(encoding="utf-8"))
    assert "artifact" not in upgraded
    assert sorted(row["role"] for row in upgraded["artifacts"]) == ["bridge", "plugin"]


@pytest.mark.parametrize("role", ["plugin", "bridge"])
def test_a_zip_that_escapes_its_folder_is_refused(run: Any, pinned: Any, tmp_path: Path, role: str) -> None:
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    plugin_zip(directory, escape=role == "plugin")
    bridge_zip(directory, escape=role == "bridge")
    manifest = manifest_for(run, pinned.root, directory)

    code, payload, _ = deploy(run, pinned.root, dest, manifest)

    assert code == 7, payload
    assert not (dest / "bridge" / "TakaroTerrariaBridge").exists()
    assert not (tmp_path / "escaped.txt").exists()
    assert not (dest / "escaped.txt").exists()


def test_deploy_refuses_a_build_that_is_not_this_targets(run: Any, pinned: Any, tmp_path: Path) -> None:
    """Two ways a deploy can be for the wrong bytes, and both are refusals."""
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    plugin_zip(directory)
    bridge_zip(directory)

    # A manifest whose rows belong to another target: nothing is unpacked.
    foreign = manifest_for(run, pinned.root, directory, target="tshock-v9.9.9")
    code, payload, _ = deploy(run, pinned.root, dest, foreign)
    assert code == 7, payload
    assert "tshock-v9.9.9" in payload["error"] or "no 'plugin' artifact" in payload["error"]
    assert not (dest / "plugins" / "TakaroTerrariaEvents.dll").exists()

    # And a directory that holds a different pin of this target: the ledger says so. The
    # image digest is part of the fingerprint, so re-pinning it is a different target's
    # bytes even though the id has not changed.
    record = read_target(pinned.root)
    record["runtime"]["container"]["digest"] = "sha256:" + "a" * 64
    write_target(pinned.root, record)
    manifest = manifest_for(run, pinned.root, directory)
    code, payload, _ = deploy(run, pinned.root, dest, manifest)
    assert code == 7, payload
    assert "the build is for" in payload["error"]
    assert not (dest / "plugins" / "TakaroTerrariaEvents.dll").exists()


# -- verification hooks --------------------------------------------------------------------


class FakeOptions:
    def __init__(self, run_id: str = "tm148-163") -> None:
        self.run_id = run_id
        self.labels = ["tm.issue=163"]
        self.only: list[str] | None = None


class FakeRun:
    """Only what the hooks touch, so a hook that reaches for more fails loudly."""

    def __init__(self, tmp_path: Path, resolved: dict[str, Any]) -> None:
        self.data_dir = tmp_path / "data"
        self.out = tmp_path / "out"
        self.out.mkdir(parents=True, exist_ok=True)
        self.docker_log = self.out / "docker.log"
        self.options = FakeOptions()
        self.resolved = resolved
        self.registration_token = "a-throwaway-registration-token"
        self.containers: list[Any] = []
        self.extra_logs: list[Path] = []
        self.rest_token = ""


TAKARO_ENV = {
    "TAKARO_WS_URL": "ws://host.docker.internal:34567/",
    "TAKARO_IDENTITY_TOKEN": "takaro-verify-tm148-163",
    "TAKARO_REGISTRATION_TOKEN": "a-throwaway-registration-token",
}


def test_verify_hooks_render_both_configs_and_know_the_terraria_lines(run: Any, repo: Path, tmp_path: Path) -> None:
    from takaro_maint.verify.runner import check_ids

    resolved = resolve(run, repo)
    fake_run = FakeRun(tmp_path, resolved)
    adapter = adapter_for(GAME)

    hooks.before_boot(fake_run, TAKARO_ENV)

    tshock = json.loads((fake_run.data_dir / "tshock" / "config.json").read_text())
    assert oct((fake_run.data_dir / "tshock" / "config.json").stat().st_mode)[-3:] == "600"
    assert tshock["Settings"]["RestApiEnabled"] is True
    assert tshock["Settings"]["RestApiPort"] == hooks.REST_PORT
    tokens = tshock["Settings"]["ApplicationRestTokens"]
    assert list(tokens) == [fake_run.rest_token]
    assert tokens[fake_run.rest_token]["UserGroupName"] == "superadmin"

    bridge_config = fake_run.data_dir / "bridge" / "TakaroConfig.txt"
    assert oct(bridge_config.stat().st_mode)[-3:] == "600"
    body = bridge_config.read_text()
    assert "takaroWsUrl=ws://host.docker.internal:34567/" in body
    assert "identityToken=takaro-verify-tm148-163" in body
    assert f"tshockBaseUrl=http://127.0.0.1:{hooks.REST_PORT}" in body
    assert "logFiles=/tshock/logs" in body
    assert "enableShutdown=true" in body

    # The lines the harness waits for, against lines of the shape the server writes.
    assert hooks.READY_LINE.search("Server started")
    assert hooks.LOADED_LINE.search("Takaro Terraria Events plugin loaded (0.2.2-dev.abc1234)")
    assert hooks.IDENTIFIED_LINE.search("Identified successfully with Takaro (gameServerId=gs_terraria)")
    assert hooks.TERRARIA_BANNER.search("Terraria Server v1.4.5.6")
    assert hooks.TSHOCK_BANNER.search("TShock 6.1.0.0 (Mintaka) now running.")

    # The container the runner will start: the world is on the command line, because TShock
    # reads no environment variable for it and stops on its world-selection menu without one.
    command = adapter.container_command(resolved, fake_run.data_dir)
    assert command[:2] == ["-world", "/worlds/takaro-verify.wld"]
    assert command[command.index("-autocreate") + 1] == "1"
    # And nothing asks for a non-root run: TShock writes inside /server, which is neither
    # writable nor a volume, so a --user run dies before it reads its configuration.
    assert adapter.container_options(resolved, fake_run.data_dir) == []

    mounts = adapter.container_mounts(resolved, fake_run.data_dir)
    assert [mount.split(":")[-1] for mount in mounts] == ["/tshock", "/worlds", "/plugins"]
    for relative in ("tshock/logs", "worlds", "plugins", "bridge"):
        assert (fake_run.data_dir / relative).is_dir()

    assert set(hooks.CHECK_IDS) <= set(check_ids(GAME))
    # `protocol` is never claimed for this connector: the lines those checks look for are
    # written by the bridge, in its own log, not by the server.
    assert "identify" not in hooks.CHECK_IDS


def test_after_boot_attaches_the_bridge_to_the_server_network_namespace(
    run: Any, repo: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """`--network container:<server>` is the whole reason the hook exists."""
    recorder = tmp_path / "docker-argv.txt"
    stub = tmp_path / "docker-stub.sh"
    stub.write_text(
        f'#!/usr/bin/env bash\nprintf "%s\\n" "$*" >> {recorder}\nif [ "$1" = "logs" ]; then sleep 0.1; fi\nexit 0\n',
        encoding="utf-8",
    )
    stub.chmod(0o755)
    monkeypatch.setenv("TAKARO_MAINT_DOCKER", str(stub))

    resolved = resolve(run, repo)
    fake_run = FakeRun(tmp_path, resolved)
    hooks.before_boot(fake_run, TAKARO_ENV)

    class Server:
        name = "takaro-verify-tm148-163"

    hooks.after_boot(fake_run, Server(), TAKARO_ENV)

    assert len(fake_run.containers) == 1
    bridge = fake_run.containers[0]
    assert bridge.name == "takaro-verify-tm148-163-bridge"
    assert fake_run.out / "bridge.log" in fake_run.extra_logs
    argv = bridge.argv
    assert argv[argv.index("--network") + 1] == "container:takaro-verify-tm148-163"
    assert argv[-2:] == ["node", "dist/index.js"]
    assert resolved["build"]["deps"]["bridge-runtime"]["resolvedCoordinate"] in argv
    assert "tm.issue=163" in argv
    assert any(part.startswith("tm.run=") for part in argv)
    # The config is mounted outside the read-only /bridge mount: docker cannot create a
    # mountpoint inside one, so a config under /bridge never gets as far as node.
    assert "BRIDGE_CONFIG=/config/TakaroConfig.txt" in argv
    assert f"{fake_run.data_dir / 'bridge' / 'TakaroConfig.txt'}:/config/TakaroConfig.txt:ro" in argv

    # The command line carries two secrets, and the kept log carries neither.
    recorded = recorder.read_text()
    assert "run" in recorded
    docker_log = fake_run.docker_log.read_text()
    assert fake_run.rest_token not in docker_log
    assert fake_run.registration_token not in docker_log


def test_the_runtime_identity_comes_from_the_server_banners() -> None:
    adapter = adapter_for(GAME)

    assert adapter.parse_runtime_identity("Terraria Server v1.4.5.6") == {
        "gameVersion": "1.4.5.6",
        "loader": "tshock",
        "loaderVersion": None,
    }
    assert adapter.parse_runtime_identity("TShock 6.1.0.0 (Mintaka) now running.") == {
        "gameVersion": None,
        "loader": "tshock",
        "loaderVersion": "6.1.0.0",
    }
    assert adapter.parse_runtime_identity("Type 'help' for a list of commands") is None


# -- the release record ---------------------------------------------------------------------


def git(repo: Path, *args: str) -> str:
    env = {
        **os.environ,
        "GIT_AUTHOR_NAME": "t",
        "GIT_AUTHOR_EMAIL": "t@example.invalid",
        "GIT_COMMITTER_NAME": "t",
        "GIT_COMMITTER_EMAIL": "t@example.invalid",
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_CONFIG_SYSTEM": "/dev/null",
    }
    return subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True, check=True, env=env).stdout.strip()


def test_compat_record_carries_both_roles_and_the_legacy_aliases(run: Any, repo: Path, tmp_path: Path) -> None:
    git(repo, "init", "-q", "-b", "main")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "fixture")
    commit = git(repo, "rev-parse", "HEAD")
    resolved = resolve(run, repo)
    directory = tmp_path / "dist" / TARGET
    plugin_zip(directory)
    bridge_zip(directory)
    rows = [
        artifact_row("plugin", TARGET, resolved["fingerprint"], directory / PLUGIN_ZIP),
        artifact_row("bridge", TARGET, resolved["fingerprint"], directory / BRIDGE_ZIP),
    ]
    for row in rows:
        write_meta(directory, row, connector=GAME, version=VERSION, revision=commit)
    write_manifest(
        directory,
        connector=GAME,
        version=VERSION,
        revision=commit,
        dirty=False,
        toolchain=resolved["build"]["toolchain"],
        mode="container",
        artifacts=rows,
    )
    out = tmp_path / "assembled"

    code, payload, err = run(
        "release",
        "assemble",
        "--connector",
        GAME,
        "--version",
        VERSION,
        "--channel",
        "pr",
        "--tag",
        f"pr-1-{GAME}",
        "--dist",
        str(tmp_path / "dist"),
        "--out",
        str(out),
        "--repo",
        "o/r",
        repo=repo,
    )

    assert code == 0, f"{err}\n{payload}"
    record = json.loads((out / f"takaro-{GAME}-{VERSION}.compat.json").read_text())
    entry = record["targets"][TARGET]
    assert {row["role"] for row in entry["artifacts"]} == {"plugin", "bridge"}
    assert entry["fingerprint"] == resolved["fingerprint"]
    assert entry["verification"]["required"] == "contract"
    assert entry["inputs"]["server"]["url"] == ASSET_URL
    assert (out / "SHA256SUMS").is_file()
    assert (out / "takaro-terraria-plugin.zip").read_bytes() == (out / PLUGIN_ZIP).read_bytes()
    assert (out / "takaro-terraria-bridge.zip").read_bytes() == (out / BRIDGE_ZIP).read_bytes()


# -- the oci-registry provider ---------------------------------------------------------------


def index_bytes(tag: str) -> bytes:
    return (OCI / "manifests" / f"{tag}.index.json").read_bytes()


def serve_registry(upstream: FakeUpstream, *, tags: list[str] | None = None) -> None:
    """The three endpoints the provider reads, with the manifests as RAW recorded bytes."""
    listing = json.loads((OCI / "tags-list.json").read_text())
    if tags is not None:
        listing["tags"] = tags
    upstream.add(TAGS_PATH, json.dumps(listing).encode("utf-8"))
    for tag in INDEX_DIGESTS:
        upstream.add(f"/v2/{REPOSITORY}/manifests/{tag}", index_bytes(tag))
    upstream.add(
        f"/v2/{REPOSITORY}/manifests/{PLATFORM_DIGEST}", (OCI / "manifests" / "29f877e0.manifest.json").read_bytes()
    )
    upstream.add(f"/v2/{REPOSITORY}/blobs/{CONFIG_DIGEST}", (OCI / "blobs" / "0a40c4aa.config.json").read_bytes())


def oci_source(base_url: str, **watch: Any) -> dict[str, Any]:
    block: dict[str, Any] = {
        "kind": "framework",
        "component": "tshock",
        "repository": REPOSITORY,
        "platform": {"os": "linux", "architecture": "amd64"},
        "window": 2,
        "channels": {
            "release": {"branch": "release", "tag": r"regex:^[0-9]+\.[0-9]+\.[0-9]+$", "gameRevision": "v{tag}"}
        },
    }
    block.update(watch)
    return {"id": "tshock-image", "provider": "oci-registry", "baseUrl": base_url, "watch": block}


def test_oci_registry_observes_immutable_tags_by_digest_and_never_floating_ones() -> None:
    """The fixture bodies must hash to the digests ghcr.io served, or nothing below means anything."""
    for tag, digest in INDEX_DIGESTS.items():
        assert hashlib.sha256(index_bytes(tag)).hexdigest() == digest, tag

    readiness.reset_registry()
    with FakeUpstream() as upstream:
        serve_registry(upstream)
        result = provider_for("oci-registry").observe(oci_source(upstream.base_url))

    assert result.status == "ok"
    assert result.history == "heads-only"
    assert [observation.rev for observation in result.observations] == ["6.1.0.911459f0", "6.0.0.5131efd0"]
    assert result.heads == {"release": "6.1.0.911459f0"}
    newest = result.observations[0]
    assert newest.facts["digest"] == f"sha256:{INDEX_DIGESTS['6.1.0']}"
    assert newest.facts["artifact"]["sha256"] == INDEX_DIGESTS["6.1.0"]
    assert newest.facts["artifact"]["name"].endswith("/pryaxis/tshock:6.1.0")
    assert newest.facts["platformDigest"] == PLATFORM_DIGEST
    assert newest.facts["gameVersion"] == "v6.1.0"
    assert "mutable" in newest.facts["observationLimit"]

    # `stable`, `latest`, `6` and `6.1` are tags too. None of them was even fetched.
    asked = set(upstream.requested)
    for floating in ("stable", "latest", "6", "6.1"):
        assert f"/v2/{REPOSITORY}/manifests/{floating}" not in asked
    assert readiness.registry().watch_for("tshock") is not None


class Response(BytesIO):
    def __init__(self, body: bytes, headers: dict[str, str] | None = None) -> None:
        super().__init__(body)
        self.headers = headers or {}


@dataclass
class ScriptedTransport:
    """A transport that answers from a script, so a header or a 401 can be posed exactly."""

    answers: dict[str, Any]
    seen: list[tuple[str, dict[str, str]]] = field(default_factory=list)

    def open(self, url: str, headers: dict[str, str]) -> Any:
        self.seen.append((url, dict(headers)))
        for prefix, answer in self.answers.items():
            if prefix in url:
                if isinstance(answer, Exception):
                    raise answer
                return answer() if callable(answer) else answer
        raise urllib.error.HTTPError(url, 404, "not found", None, None)  # type: ignore[arg-type]

    def open_no_redirect(self, url: str, headers: dict[str, str], addresses: tuple[net.PinnedAddress, ...]) -> Any:
        assert addresses
        return self.open(url, headers)


def public_realm_dns(monkeypatch: pytest.MonkeyPatch) -> None:
    """Resolve the documentation-only auth host to a public address without consulting DNS."""
    monkeypatch.setattr(
        "takaro_maint.providers.oci_registry.socket.getaddrinfo",
        lambda host, port, **kwargs: [(2, 1, 6, "", ("93.184.216.34", port))],
    )


def test_oci_registry_never_follows_pagination_to_another_origin() -> None:
    from takaro_maint.exit_codes import MaintError

    listing = json.dumps({"tags": ["6.1.0"]}).encode("utf-8")
    transport = ScriptedTransport(
        {
            "/tags/list": lambda: Response(
                listing,
                {"Link": '<https://elsewhere.example/v2/pryaxis/tshock/tags/list?last=6.1.0>; rel="next"'},
            )
        }
    )
    net.set_transport(transport)
    try:
        with pytest.raises(MaintError) as caught:
            provider_for("oci-registry").observe(oci_source("https://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    assert caught.value.code == 4
    assert "next link leaves https://registry.invalid" in str(caught.value)
    assert "elsewhere.example" in str(caught.value)
    assert not [url for url, _ in transport.seen if "elsewhere.example" in url]


def test_oci_registry_fails_the_source_when_the_digest_header_disagrees() -> None:
    from takaro_maint.exit_codes import MaintError

    listing = json.dumps({"tags": ["6.1.0"]}).encode("utf-8")
    transport = ScriptedTransport(
        {
            "/tags/list": lambda: Response(listing),
            "/manifests/": lambda: Response(index_bytes("6.1.0"), {"Docker-Content-Digest": "sha256:" + "0" * 64}),
        }
    )
    net.set_transport(transport)
    try:
        with pytest.raises(MaintError) as caught:
            provider_for("oci-registry").observe(oci_source("http://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    assert caught.value.code == 4
    assert "digest header disagrees" in str(caught.value)


def test_oci_registry_refuses_an_index_without_the_watched_platform() -> None:
    from takaro_maint.exit_codes import MaintError

    listing = json.dumps({"tags": ["6.1.0"]}).encode("utf-8")
    transport = ScriptedTransport(
        {"/tags/list": lambda: Response(listing), "/manifests/": lambda: Response(index_bytes("6.1.0"))}
    )
    net.set_transport(transport)
    try:
        with pytest.raises(MaintError) as caught:
            provider_for("oci-registry").observe(
                oci_source("http://registry.invalid", platform={"os": "plan9", "architecture": "sparc"})
            )
    finally:
        net.set_transport(net.UrllibTransport())

    assert caught.value.code == 4
    assert "plan9/sparc" in str(caught.value)
    assert "linux/amd64" in str(caught.value)


@pytest.mark.parametrize(
    "realm",
    [
        "file:///etc/passwd",
        "http://169.254.169.254/latest",
        "https://user@auth.invalid/token",
        "https://169.254.169.254/latest",
        "https://localhost/token",
        "https://2130706433/token",
        "https://0x7f000001/token",
        "https://017700000001/token",
        "https://127.1/token",
    ],
)
def test_a_hostile_bearer_realm_is_never_followed(realm: str) -> None:
    """The realm comes from whoever answered the request, so it is attacker-controlled."""
    from takaro_maint.exit_codes import MaintError

    def tags() -> Any:
        raise urllib.error.HTTPError(
            "https://registry.invalid/v2/pryaxis/tshock/tags/list",
            401,
            "unauthorized",
            {"WWW-Authenticate": f'Bearer realm="{realm}",service="registry.invalid"'},  # type: ignore[arg-type]
            None,
        )

    transport = ScriptedTransport({"/tags/list": tags})
    net.set_transport(transport)
    readiness.reset_registry()
    try:
        with pytest.raises(MaintError) as caught:
            provider_for("oci-registry").observe(oci_source("https://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    assert caught.value.code == 4
    assert "is not an https URL to a named host" in str(caught.value)
    assert not [url for url, _ in transport.seen if url.startswith(realm.split("?")[0])]


def test_a_realm_name_resolving_to_a_private_address_is_never_followed(monkeypatch: pytest.MonkeyPatch) -> None:
    from takaro_maint.exit_codes import MaintError

    def tags() -> Any:
        raise urllib.error.HTTPError(
            "https://registry.invalid/v2/pryaxis/tshock/tags/list",
            401,
            "unauthorized",
            {"WWW-Authenticate": 'Bearer realm="https://auth.invalid/token"'},  # type: ignore[arg-type]
            None,
        )

    monkeypatch.setattr(
        "takaro_maint.providers.oci_registry.socket.getaddrinfo",
        lambda host, port, **kwargs: [(2, 1, 6, "", ("10.0.0.7", port))],
    )
    transport = ScriptedTransport({"/tags/list": tags})
    net.set_transport(transport)
    try:
        with pytest.raises(MaintError) as caught:
            provider_for("oci-registry").observe(oci_source("https://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    assert "does not resolve only to public addresses" in str(caught.value)
    assert not [url for url, _ in transport.seen if "auth.invalid" in url]


def test_a_validated_realm_address_is_reused_without_a_second_dns_lookup(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    listing = json.dumps({"tags": ["6.1.0"]}).encode("utf-8")
    challenged = {"done": False}
    resolutions = 0

    def resolve(host: str, port: int, **kwargs: Any) -> list[tuple[Any, ...]]:
        nonlocal resolutions
        resolutions += 1
        address = "93.184.216.34" if resolutions == 1 else "127.0.0.1"
        return [(2, 1, 6, "", (address, port))]

    def tags() -> Any:
        if not challenged["done"]:
            challenged["done"] = True
            raise urllib.error.HTTPError(
                "https://registry.invalid/v2/pryaxis/tshock/tags/list",
                401,
                "unauthorized",
                {"WWW-Authenticate": 'Bearer realm="https://auth.invalid/token"'},  # type: ignore[arg-type]
                None,
            )
        return Response(listing)

    monkeypatch.setattr("takaro_maint.providers.oci_registry.socket.getaddrinfo", resolve)
    transport = ScriptedTransport(
        {
            "auth.invalid/token": lambda: Response(json.dumps({"token": "t"}).encode()),
            "/tags/list": tags,
            "/manifests/": lambda: Response(index_bytes("6.1.0")),
        }
    )
    net.set_transport(transport)
    readiness.reset_registry()
    try:
        provider_for("oci-registry").observe(oci_source("https://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    assert resolutions == 1
    realm_url, _headers = next(row for row in transport.seen if "auth.invalid" in row[0])
    assert realm_url.startswith("https://auth.invalid/token")


def test_a_hostile_challenge_parameter_cannot_rewrite_the_realm_query(monkeypatch: pytest.MonkeyPatch) -> None:
    """The challenge's own values are encoded into the query string."""
    public_realm_dns(monkeypatch)
    listing = json.dumps({"tags": ["6.1.0"]}).encode("utf-8")
    challenged = {"done": False}

    def tags() -> Any:
        if not challenged["done"]:
            challenged["done"] = True
            raise urllib.error.HTTPError(
                "https://registry.invalid/v2/pryaxis/tshock/tags/list",
                401,
                "unauthorized",
                {  # type: ignore[arg-type]
                    "WWW-Authenticate": 'Bearer realm="https://auth.invalid/token",'
                    'service="a&b=c d",scope="repository:pryaxis/tshock:pull"'
                },
                None,
            )
        return Response(listing)

    transport = ScriptedTransport(
        {
            "auth.invalid/token": lambda: Response(json.dumps({"token": "t"}).encode("utf-8")),
            "/tags/list": tags,
            "/manifests/": lambda: Response(index_bytes("6.1.0")),
        }
    )
    net.set_transport(transport)
    readiness.reset_registry()
    try:
        provider_for("oci-registry").observe(oci_source("https://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    realm_call = next(url for url, _ in transport.seen if "auth.invalid" in url)
    assert "service=a%26b%3Dc+d" in realm_call
    # The scope's own colons and slashes stay readable, which is the form a registry wants.
    assert "scope=repository:pryaxis/tshock:pull" in realm_call


def test_a_prerelease_is_never_the_channel_head() -> None:
    """A prerelease is never the channel head, whatever the tag order."""
    readiness.reset_registry()
    with FakeUpstream() as upstream:
        serve_registry(upstream, tags=["6.1.0-pre3", "6.1.0"])
        # The prerelease's body only has to be a real index; which one decides its digest.
        upstream.add(f"/v2/{REPOSITORY}/manifests/6.1.0-pre3", index_bytes("6.0.0"))
        channels = {
            "release": {
                "branch": "release",
                "tag": r"regex:^[0-9]+\.[0-9]+\.[0-9]+(-pre[0-9]+)?$",
                "gameRevision": "v{tag}",
            }
        }
        result = provider_for("oci-registry").observe(oci_source(upstream.base_url, channels=channels))

    assert result.status == "ok"
    assert [observation.facts["tag"] for observation in result.observations] == ["6.1.0", "6.1.0-pre3"]
    assert result.heads == {"release": result.observations[0].rev}


def test_oci_registry_answers_a_bearer_challenge_and_never_prints_the_token(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    public_realm_dns(monkeypatch)
    secret = "secret-xyz"
    listing = json.dumps({"tags": ["6.1.0"]}).encode("utf-8")
    challenged = {"done": False}

    def tags() -> Any:
        if not challenged["done"]:
            challenged["done"] = True
            raise urllib.error.HTTPError(
                "http://registry.invalid/v2/pryaxis/tshock/tags/list",
                401,
                "unauthorized",
                {  # type: ignore[arg-type]
                    "WWW-Authenticate": 'Bearer realm="https://auth.invalid/token",'
                    'service="registry.invalid",scope="repository:pryaxis/tshock:pull"'
                },
                None,
            )
        return Response(listing)

    transport = ScriptedTransport(
        {
            "auth.invalid/token": lambda: Response(json.dumps({"token": secret}).encode("utf-8")),
            "/tags/list": tags,
            "/manifests/": lambda: Response(index_bytes("6.1.0")),
        }
    )
    net.set_transport(transport)
    readiness.reset_registry()
    try:
        result = provider_for("oci-registry").observe(oci_source("http://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    assert [observation.rev for observation in result.observations] == ["6.1.0.911459f0"]
    # The realm was asked with the challenge's own parameters, and every later request carried the token.
    realm_calls = [url for url, _ in transport.seen if "auth.invalid" in url]
    assert realm_calls and "scope=repository%3A" not in realm_calls[0]
    assert "scope=repository:pryaxis/tshock:pull" in realm_calls[0]
    later = [headers for url, headers in transport.seen if url.startswith("http://registry.invalid")][1:]
    assert later and all(headers.get("Authorization") == f"Bearer {secret}" for headers in later)

    captured = capsys.readouterr()
    assert secret not in captured.out
    assert secret not in captured.err
    assert secret not in json.dumps([observation.facts for observation in result.observations])


def test_a_bearer_realm_redirect_is_refused(monkeypatch: pytest.MonkeyPatch) -> None:
    from takaro_maint.exit_codes import MaintError

    public_realm_dns(monkeypatch)

    def tags() -> Any:
        raise urllib.error.HTTPError(
            "https://registry.invalid/v2/pryaxis/tshock/tags/list",
            401,
            "unauthorized",
            {"WWW-Authenticate": 'Bearer realm="https://auth.invalid/token"'},  # type: ignore[arg-type]
            None,
        )

    class RedirectingTransport(ScriptedTransport):
        def open_no_redirect(self, url: str, headers: dict[str, str], addresses: tuple[net.PinnedAddress, ...]) -> Any:
            assert addresses
            self.seen.append((url, dict(headers)))
            raise urllib.error.HTTPError(
                url,
                302,
                "found",
                {"Location": "https://127.0.0.1/token"},  # type: ignore[arg-type]
                None,
            )

    transport = RedirectingTransport({"/tags/list": tags})
    net.set_transport(transport)
    try:
        with pytest.raises(MaintError) as caught:
            provider_for("oci-registry").observe(oci_source("https://registry.invalid"))
    finally:
        net.set_transport(net.UrllibTransport())

    assert "redirects are refused" in str(caught.value)
    assert not [url for url, _ in transport.seen if "127.0.0.1" in url]


def test_oci_registry_enrich_reads_labels_from_the_platform_config() -> None:
    readiness.reset_registry()
    provider = provider_for("oci-registry")
    with FakeUpstream() as upstream:
        serve_registry(upstream)
        source = oci_source(upstream.base_url)
        observation = provider.observe(source).observations[0]
        enriched = provider.enrich(observation, source)

    assert enriched.facts["configDigest"] == CONFIG_DIGEST
    labels = enriched.facts["labels"]
    assert labels["org.opencontainers.image.version"] == "6.1.0"
    assert enriched.facts["revision"] == "1afaeb514343ca547abceeb357654603d1e2a456"
    assert enriched.facts["releaseTime"] == "2026-03-11T18:48:45.415Z"


def test_oci_registry_refuses_bytes_that_do_not_hash_to_the_digest_they_were_fetched_by() -> None:
    """Fetching by digest is the pin; a body that is not those bytes is not the image."""
    from takaro_maint.exit_codes import MaintError

    readiness.reset_registry()
    provider = provider_for("oci-registry")

    # A platform manifest swapped for another real manifest: well-formed, wrong bytes.
    with FakeUpstream() as upstream:
        serve_registry(upstream)
        upstream.add(f"/v2/{REPOSITORY}/manifests/{PLATFORM_DIGEST}", index_bytes("6.0.0"))
        source = oci_source(upstream.base_url)
        observation = provider.observe(source).observations[0]
        with pytest.raises(MaintError) as manifest_error:
            provider.enrich(observation, source)
    assert manifest_error.value.code == 4
    assert "does not hash to the digest it was fetched by" in str(manifest_error.value)

    # The config blob the manifest names, replaced with someone else's labels.
    readiness.reset_registry()
    with FakeUpstream() as upstream:
        serve_registry(upstream)
        upstream.add(f"/v2/{REPOSITORY}/blobs/{CONFIG_DIGEST}", b'{"config": {"Labels": {"a": "b"}}}')
        source = oci_source(upstream.base_url)
        observation = provider.observe(source).observations[0]
        with pytest.raises(MaintError) as blob_error:
            provider.enrich(observation, source)
    assert blob_error.value.code == 4
    assert "the config blob does not hash to the digest the manifest names" in str(blob_error.value)


def test_no_provider_names_a_game() -> None:
    """The provider is generic: a second game pointing at a registry writes no Python."""
    for module in sorted(Path(REPO_ROOT / "maintenance/src/takaro_maint/providers").glob("*.py")):
        body = module.read_text(encoding="utf-8").lower()
        assert "tshock" not in body, module.name
        assert "terraria" not in body, module.name


# -- the recorded GitHub listing ---------------------------------------------------------


def test_the_game_watch_observes_tshock_releases_and_not_prereleases() -> None:
    releases = json.loads((TERRARIA_FIXTURES / "tshock-releases.json").read_text())
    with FakeUpstream() as upstream:
        upstream.add("/repos/Pryaxis/TShock/releases?per_page=10", json.dumps(releases).encode("utf-8"))
        game = json.loads((REPO_ROOT / "catalog" / GAME / "game.json").read_text())
        source = {**game["sources"]["tshock-releases"], "id": "tshock-releases", "baseUrl": upstream.base_url}
        result = provider_for("github-release").observe(source)

    revs = [observation.rev for observation in result.observations]
    assert revs[0] == "v6.2.1"
    assert all(not rev.endswith(("-pre1", "-pre2", "-pre3")) for rev in revs), revs
    newest = result.observations[0]
    assert newest.kind == "game"
    assert newest.component == "terraria"
    # The Terraria version this TShock is for lives in the release name and the asset name.
    assert "1.4.5.8" in str(newest.facts.get("releaseName") or newest.facts.get("name") or "")


# -- the dev-servers rig -------------------------------------------------------------------

DS_ROOT = REPO_ROOT / "dev-servers"


def bash(script: str) -> str:
    completed = subprocess.run(["bash", "-c", script], cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


def test_dev_servers_terraria_rig_parses_and_resolves() -> None:
    completed = subprocess.run(
        ["bash", "-n", str(DS_ROOT / "lib" / "games" / "terraria.sh")], capture_output=True, text=True, check=False
    )
    assert completed.returncode == 0, completed.stderr

    assert bash(". dev-servers/lib/common.sh; ds_target_prefix terraria").strip() == "TERRARIA"
    assert bash(". dev-servers/lib/common.sh; ds_target_dest terraria").strip().endswith("/terraria")
    assert bash(". dev-servers/lib/common.sh; ds_success_pattern_terraria").strip() == "Identified successfully"
    registry = bash(". dev-servers/lib/common.sh; ds_registry")
    assert "terraria|terraria.yml|-|terraria terraria-bridge|1|1|plugin|" in registry
    assert bash(". dev-servers/lib/common.sh; ds_services terraria").split() == ["terraria", "terraria-bridge"]
    assert bash(". dev-servers/lib/common.sh; ds_startable_services terraria").split() == [
        "terraria",
        "terraria-bridge",
    ]


def test_the_terraria_compose_file_runs_the_bridge_in_the_server_namespace() -> None:
    if subprocess.run(["docker", "version"], capture_output=True, check=False).returncode != 0:
        pytest.skip("no docker on this host")
    environment = dict(os.environ)
    body = (DS_ROOT / "compose" / "terraria.yml").read_text()
    # The same substitution the compose lint makes: every reference .env.example does not
    # declare becomes a placeholder, default or no default.
    referenced = set(re.findall(r"\$\{([A-Za-z_][A-Za-z0-9_]*)", body))
    declared = (DS_ROOT / ".env.example").read_text()
    for name in sorted(referenced):
        if not re.search(rf"^{name}=.", declared, re.MULTILINE):
            environment[name] = "placeholder"
    environment["TERRARIA_IMAGE"] = IMAGE_REF
    environment["TERRARIA_BRIDGE_IMAGE"] = "docker.io/library/node:22.23.2-bookworm-slim"

    completed = subprocess.run(
        ["docker", "compose", "--env-file", ".env.example", "-f", "compose/terraria.yml", "config", "--format", "json"],
        cwd=DS_ROOT,
        capture_output=True,
        text=True,
        check=False,
        env=environment,
    )
    assert completed.returncode == 0, completed.stderr
    services = json.loads(completed.stdout)["services"]
    assert set(services) == {"terraria", "terraria-bridge"}
    assert services["terraria"]["image"] == IMAGE_REF
    assert services["terraria-bridge"]["network_mode"] == "service:terraria"
    assert services["terraria-bridge"]["command"] == ["node", "dist/index.js"]
    # The bridge's config is bound outside its read-only /bridge mount: docker cannot
    # create a mountpoint inside a read-only mount, so a config under /bridge never boots.
    assert services["terraria-bridge"]["environment"]["BRIDGE_CONFIG"] == "/config/TakaroConfig.txt"
    targets = {volume["target"] for volume in services["terraria-bridge"]["volumes"]}
    assert "/config/TakaroConfig.txt" in targets
    # Literal: a placeholder substituted into a port spec is not a port, and the lint
    # substitutes one for every variable .env.example does not declare.
    published = {(port["published"], port["target"]) for port in services["terraria"]["ports"]}
    assert published == {("7777", 7777), ("7878", 7878)}


# -- the build plumbing and CI ---------------------------------------------------------------


def test_builder_dockerfile_uses_the_catalog_toolchain(run: Any) -> None:
    resolved = resolve(run, REPO_ROOT)
    first_from = next(
        line.split(None, 1)[1].strip()
        for line in (REPO_ROOT / "games" / "terraria" / "Dockerfile.builder").read_text().splitlines()
        if line.startswith("FROM ")
    )

    assert first_from == resolved["toolchainRef"]


def test_the_workflow_delegates_to_connector_release_without_runtime_ci() -> None:
    body = (REPO_ROOT / ".github" / "workflows" / "terraria.yml").read_text()

    assert "uses: ./.github/workflows/connector-release.yml" in body
    assert "connector: terraria" in body
    assert "runtime: false" in body
    # The workflow packages nothing itself: it sets up no .NET and names no assets.
    assert "setup-dotnet" not in body
    assert "--mode legacy" not in body
    assert "catalog/terraria/**" in body


# -- the two watches, joined -----------------------------------------------------------------


def prune_other_watches(root: Path, keep: str = GAME) -> None:
    """A scan walks the whole catalog; this rig only answers for one game's sources."""
    for game_file in sorted((root / "catalog").glob("*/game.json")):
        if game_file.parent.name == keep:
            continue
        record = json.loads(game_file.read_text(encoding="utf-8"))
        for source in (record.get("sources") or {}).values():
            source.pop("watch", None)
        game_file.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def point_terraria_at(root: Path, base_url: str) -> None:
    record = read_game(root)
    for source in record["sources"].values():
        source["baseUrl"] = base_url
    write_game(root, record)


def next_release(releases: list[dict[str, Any]], tag: str, terraria: str) -> list[dict[str, Any]]:
    """One more published TShock release, shaped exactly like the recorded ones."""
    version = tag.lstrip("v")
    asset = f"TShock-{version}-for-Terraria-{terraria}-linux-x64-Release.zip"
    entry = {
        "tag_name": tag,
        "name": f"TShock {version.rpartition('.')[0]} for Terraria {terraria}",
        "prerelease": False,
        "draft": False,
        "published_at": "2026-06-02T10:00:00Z",
        "assets": [
            {
                "name": asset,
                "size": 34002037,
                "digest": "sha256:" + hashlib.sha256(asset.encode()).hexdigest(),
                "updated_at": "2026-06-02T09:58:00Z",
                "browser_download_url": f"https://github.com/Pryaxis/TShock/releases/download/{tag}/{asset}",
            }
        ],
    }
    return [entry, *releases]


def support_issues(tracker: Any) -> list[dict[str, Any]]:
    from takaro_maint.tracker import identity

    return [
        issue
        for issue in tracker.issues
        if (identity.parse_marker(str(issue.get("body") or "")) or {}).get("kind") == "support"
    ]


def test_terraria_readiness_joins_the_image_to_the_release_issue(
    run: Any, repo: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The point of two providers: one issue, whose readiness row is the image's own digest.

    A TShock release and the image built from it are published by different systems at
    different times, so the scan has to say both "there is a release nothing pins" and
    "the image that release needs already exists", in one issue a maintainer can act on.
    """
    releases = json.loads((TERRARIA_FIXTURES / "tshock-releases.json").read_text())
    tags = ["6.1.0", "6.1", "6", "6.0.0", "6.0", "stable", "latest"]
    readiness.reset_registry()
    with FakeUpstream() as upstream, FakeGitHub() as tracker:
        releases_path = "/repos/Pryaxis/TShock/releases?per_page=10"
        upstream.add(releases_path, json.dumps(releases).encode("utf-8"))
        # The floating tags are in the listing on purpose: "never observed" is asserted
        # against a real temptation, not against their absence.
        serve_registry(upstream, tags=tags)
        prune_other_watches(repo)
        point_terraria_at(repo, upstream.base_url)
        monkeypatch.setenv("GH_TOKEN", "token-for-tests")

        def scan(*flags: str) -> tuple[int, Any, str]:
            # A CLI run is one process, so a real scan starts with an empty registry.
            readiness.reset_registry()
            return run(
                "scan", "--game", GAME, "--publish", "--repo", "o/r", "--api-url", tracker.api_url, *flags, repo=repo
            )

        code, payload, err = scan("--bootstrap")
        assert code == 0, f"{err}\n{payload}"
        assert support_issues(tracker) == [], [issue["title"] for issue in support_issues(tracker)]

        # Upstream publishes TShock 6.2, and ghcr already carries the image built from it.
        upstream.add(releases_path, json.dumps(next_release(releases, "v6.2.0", "1.4.5.7")).encode("utf-8"))
        serve_registry(upstream, tags=["6.2.0", *tags])
        upstream.add(f"/v2/{REPOSITORY}/manifests/6.2.0", index_bytes("6.1.0"))

        code, payload, err = scan()
        assert code == 0, f"{err}\n{payload}"
        filed = support_issues(tracker)

    assert len(filed) == 1, [issue["title"] for issue in filed]
    issue = filed[0]
    assert issue["title"] == "Terraria (TShock) v6.2.0: new stable release needs a target"
    body = str(issue["body"])
    rows = readiness.parse_rows(body) or {}
    assert "tshock" in rows, body
    row = rows["tshock"]
    assert row.is_ready, row
    assert row.rev == "6.2.0.911459f0"
    assert "pryaxis/tshock:6.2.0" in body
    assert "ready-for-agent" in body


def test_setup_environment_refuses_a_reference_cache_that_is_not_the_catalogs(
    run: Any, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Both refusals happen before docker is reached, so they are checkable anywhere.

    A cache for another target is a 7 and a cache whose bytes are not the ones the catalog
    pins is a 5 — never a silent re-extraction, which would repair whatever changed those
    bytes without ever saying so and leave the next build looking clean.
    """
    resolved = resolve(run, REPO_ROOT)
    refs = REPO_ROOT / "games" / "terraria" / "_data" / "refs" / str(resolved["fp16"])
    if refs.exists():
        pytest.skip("this checkout already holds an extracted reference cache")

    script = REPO_ROOT / "games" / "terraria" / "scripts" / "setup-environment.sh"
    marker = refs / ".takaro" / "references.json"
    marker.parent.mkdir(parents=True)
    try:
        for name in ("TShockAPI.dll", "OTAPI.dll", "TerrariaServer.dll"):
            (refs / name).write_bytes(b"not the assembly the catalog pins")

        marker.write_text(json.dumps({"fingerprint": "9" * 64, "image": "x", "files": []}))
        stale = subprocess.run([str(script)], capture_output=True, text=True, check=False, cwd=REPO_ROOT)
        assert stale.returncode == 7, stale.stderr
        assert "stale reference cache" in stale.stderr

        marker.write_text(json.dumps({"fingerprint": resolved["fingerprint"], "image": "x", "files": []}))
        altered = subprocess.run([str(script)], capture_output=True, text=True, check=False, cwd=REPO_ROOT)
        assert altered.returncode == 5, altered.stderr
        assert "--force" in altered.stderr
    finally:
        import shutil as _shutil

        _shutil.rmtree(refs, ignore_errors=True)


def test_after_shutdown_hands_the_data_directory_back_to_the_caller(
    run: Any, repo: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The server runs as root, so something has to give its files back, and say so."""
    import asyncio

    recorder = tmp_path / "docker-argv.txt"
    stub = tmp_path / "docker-stub.sh"
    stub.write_text(f'#!/usr/bin/env bash\nprintf "%s\\n" "$*" >> {recorder}\nexit 0\n', encoding="utf-8")
    stub.chmod(0o755)
    monkeypatch.setenv("TAKARO_MAINT_DOCKER", str(stub))

    resolved = resolve(run, repo)
    fake_run = FakeRun(tmp_path, resolved)

    asyncio.run(hooks.after_shutdown(fake_run, None, "ws://unused/", None))

    argv = recorder.read_text().split()
    # chown has to replace the image's entrypoint, which is the server itself.
    assert argv[argv.index("--entrypoint") + 1] == "chown"
    assert f"{fake_run.data_dir}:/target" in argv
    assert f"{os.getuid()}:{os.getgid()}" in argv
    assert argv[-1] == "/target"


def test_npms_own_dot_metadata_never_reaches_a_deployable_bridge_archive(run: Any, pinned: Any, tmp_path: Path) -> None:
    """``npm ci`` writes node_modules/.package-lock.json, and a deploy refuses that name.

    The bridge archive ships its production dependency, so the release script has to prune
    npm's bookkeeping or every deploy of a real build stops at the path rule.
    """
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    plugin_zip(directory)
    make_zip(
        directory / BRIDGE_ZIP,
        "TakaroTerrariaBridge",
        {"dist/index.js": "bridge", "package.json": "{}", "node_modules/.package-lock.json": "{}"},
    )
    manifest = manifest_for(run, pinned.root, directory)

    code, payload, _ = deploy(run, pinned.root, dest, manifest)

    assert code == 2, payload
    assert ".package-lock.json" in payload["error"]
    assert not (dest / "bridge" / "TakaroTerrariaBridge").exists()

    release = (REPO_ROOT / "games" / "terraria" / "scripts" / "build-release.sh").read_text()
    assert "find \"${STAGE}/TakaroTerrariaBridge/node_modules\" -name '.*' -prune -exec rm -rf {} +" in release


def test_a_corrupt_bridge_archive_leaves_the_working_bridge_in_place(run: Any, pinned: Any, tmp_path: Path) -> None:
    """A half-extracted replacement is worse than no replacement: the old one stays."""
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    plugin_zip(directory)
    bridge_zip(directory)
    deploy(run, pinned.root, dest, manifest_for(run, pinned.root, directory))
    live = dest / "bridge" / "TakaroTerrariaBridge" / "dist" / "index.js"
    assert live.read_text() == "bridge"

    # The same archive with one member's stored bytes flipped: the name check passes, the
    # CRC does not, and extraction fails partway through.
    broken = tmp_path / "broken"
    make_zip(
        broken / BRIDGE_ZIP,
        "TakaroTerrariaBridge",
        {"dist/index.js": "replacement", "package.json": "{}", "node_modules/ws/index.js": "ws"},
    )
    raw = bytearray((broken / BRIDGE_ZIP).read_bytes())
    marker = raw.index(b"replacement")
    raw[marker] = raw[marker] ^ 0xFF
    (broken / BRIDGE_ZIP).write_bytes(bytes(raw))
    plugin_zip(broken)

    code, payload, stderr = deploy(run, pinned.root, dest, manifest_for(run, pinned.root, broken))

    # The archive is refused where it breaks -- on its own bytes, not on its file name.
    assert code != 0, payload
    assert "CRC" in stderr, stderr
    assert live.read_text() == "bridge"
    assert not (dest / "bridge" / ".TakaroTerrariaBridge.incoming").exists()


def test_a_default_run_drops_the_checks_this_connector_cannot_answer(run: Any, repo: Path, tmp_path: Path) -> None:
    """The obvious command must not be four guaranteed failures.

    The narrowing itself lives in the runner and is tested for every game in
    `test_verify_selection.py`; what Terraria owes is the declaration behind it.
    """
    from takaro_maint.verify.runner import check_ids, game_hooks

    declared = game_hooks("terraria")
    selected = set(check_ids("terraria")) - set(declared.unsupported_checks)

    assert declared.unsupported_checks == hooks.UNSUPPORTED_CHECKS
    # Each dropped check names the Terraria check that stands in for it, and the report
    # carries that map under `handshake` -- the runner's own skip reason cannot say it.
    assert set(hooks.UNSUPPORTED_CHECKS) == {"connector-load", "identify", "catalog-items", "catalog-entities"}
    assert all(reason for reason in hooks.UNSUPPORTED_CHECKS.values())
    # Everything Terraria does answer is still in, including the base lifecycle.
    assert {"build", "startup", "heartbeat", "players", "console", "shutdown"} <= selected
    assert set(hooks.CHECK_IDS) <= selected


@pytest.mark.parametrize(
    ("role", "contents"),
    [
        ("plugin", {"README.txt": "nothing here"}),
        ("bridge", {"README.md": "nothing here"}),
    ],
)
def test_an_archive_without_what_the_server_loads_is_refused(
    run: Any, pinned: Any, tmp_path: Path, role: str, contents: dict[str, str]
) -> None:
    """A zip that unpacks cleanly and holds nothing loadable is not a deploy."""
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    folder = "TakaroTerrariaEvents" if role == "plugin" else "TakaroTerrariaBridge"
    name = PLUGIN_ZIP if role == "plugin" else BRIDGE_ZIP
    (plugin_zip if role == "bridge" else bridge_zip)(directory)
    make_zip(directory / name, folder, contents)

    code, payload, _ = deploy(run, pinned.root, dest, manifest_for(run, pinned.root, directory))

    assert code == 7, payload
    assert "is missing" in json.dumps(payload), payload
    # Only the role under test: the deploy command applies roles one at a time, so the
    # other one may already have landed. That partial-upgrade window is the core's.
    landed = (
        dest / "plugins" / "TakaroTerrariaEvents.dll" if role == "plugin" else dest / "bridge" / "TakaroTerrariaBridge"
    )
    assert not landed.exists()


@pytest.mark.parametrize("role", ["plugin", "bridge"])
@pytest.mark.parametrize("body", [b"not a zip at all", b"PK\x03\x04truncated"])
def test_an_artifact_that_is_not_a_zip_leaves_the_deployed_connector_alone(
    run: Any, pinned: Any, tmp_path: Path, role: str, body: bytes
) -> None:
    """The manifest's sha256 says the bytes are the built ones, not that they are a zip."""
    dest = tmp_path / "terraria"
    installed(run, pinned, dest)
    directory = tmp_path / "dist"
    plugin_zip(directory)
    bridge_zip(directory)
    assert deploy(run, pinned.root, dest, manifest_for(run, pinned.root, directory))[0] == 0
    landed = (
        dest / "plugins" / "TakaroTerrariaEvents.dll"
        if role == "plugin"
        else dest / "bridge" / "TakaroTerrariaBridge" / "dist" / "index.js"
    )
    before = landed.read_bytes()

    (directory / (PLUGIN_ZIP if role == "plugin" else BRIDGE_ZIP)).write_bytes(body)
    code, payload, _ = deploy(run, pinned.root, dest, manifest_for(run, pinned.root, directory))

    assert code == 7, payload
    assert "is not a zip archive" in json.dumps(payload)
    assert landed.read_bytes() == before
    assert not list(dest.glob(".*.staging*"))


def test_prerelease_tags_sort_by_number_not_by_string() -> None:
    """A suffix is a prerelease of the version it hangs off, so it sorts before it."""
    from takaro_maint.providers.oci_registry import _sort_key

    tags = ["6.0.0-pre9", "6.0.0-pre10", "6.0.0-pre2", "6.0.0"]

    assert sorted(tags, key=_sort_key) == ["6.0.0-pre2", "6.0.0-pre9", "6.0.0-pre10", "6.0.0"]
    assert max(tags, key=_sort_key) == "6.0.0"
    # And a deeper version still beats the one it extends.
    assert sorted(["6.1.0", "6.1.0.1", "6.1.0-pre3"], key=_sort_key) == ["6.1.0-pre3", "6.1.0", "6.1.0.1"]


def test_the_identify_line_the_harness_waits_for_is_the_one_the_bridge_writes() -> None:
    """The contract is across two languages, so it has to be bound rather than restated.

    `hooks.IDENTIFIED_LINE` is a Python regex; the line it waits for is built by a
    TypeScript template. The test binds their literal text so a rename cannot leave the
    other side looking correct while the check waits out its budget for a different line.
    """
    source = (REPO_ROOT / "games/terraria/bridge/src/takaro/connectionLog.ts").read_text(encoding="utf-8")

    prefixes = re.findall(r"^export const IDENTIFIED_PREFIX = '([^']+)';$", source, re.MULTILINE)
    assert len(prefixes) == 1, "exactly one IDENTIFIED_PREFIX, or this test is reading the wrong thing"
    # `identifiedLine` interpolates the id after the prefix; the harness matches the prefix.
    sample = f"info: {prefixes[0]} (gameServerId=gs_terraria)"

    assert hooks.IDENTIFIED_LINE.search(sample), f"{hooks.IDENTIFIED_LINE.pattern!r} does not match {sample!r}"
