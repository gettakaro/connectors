"""The Valheim target end to end: the pack provider, the catalog, install, build, deploy.

Valheim is the first target with two pinned inputs — a Steam depot set *and* a Thunderstore
package that has to be unpacked into it — and the first with two artifact roles. Everything
here runs the real command against the DepotDownloader stand-in and a local HTTP server
standing in for Thunderstore, so what is asserted is what a maintainer, the rig and CI see.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import subprocess
import zipfile
from pathlib import Path
from typing import Any

import pytest

import fake_depotdownloader as fake
from fake_upstream import FakeUpstream
from fake_verify import CannedSocket, FakeRun
from takaro_maint.games import adapter_for
from takaro_maint.games.valheim import verify as hooks
from takaro_maint.providers import provider_for
from takaro_maint.publish.manifest import artifact_row, write_manifest, write_meta

GAME = "valheim"
TARGET = "linux-1.0.17"
REVISION = "1.0.17"
VERSION = "3.0.3-dev.abc1234"
DEPOT = "896661"
PINNED_MANIFEST = "6686760496212527200"
HEAD_MANIFEST = "1900000000000000002"
PACK_VERSION = "5.4.2351"
PACK_PATH = f"/package/download/denikson/BepInExPack_Valheim/{PACK_VERSION}/"
PACK_API = "/api/experimental/package/denikson/BepInExPack_Valheim/"
MANAGED = "valheim_server_Data/Managed"
PLUGIN_ZIP = f"takaro-valheim-plugin-{TARGET}-{VERSION}.zip"
COMPANION_ZIP = f"takaro-valheim-inventory-companion-{TARGET}-{VERSION}.zip"

FIXTURES = Path(__file__).parent / "fixtures"
VALHEIM_FIXTURES = FIXTURES / "games" / "valheim"
DEPOTS = VALHEIM_FIXTURES / "depots"
PACK_SOURCE = VALHEIM_FIXTURES / "bepinex-pack"
THUNDERSTORE_DOCUMENT = FIXTURES / "providers" / "thunderstore" / "bepinexpack-valheim.json"
REPO_ROOT = Path(__file__).resolve().parents[2]


def test_every_valheim_verification_body_has_pass_and_failure_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    run = FakeRun(tmp_path)
    fake = CannedSocket(
        {
            "listItems": [{"code": "SwordBronze", "name": "Bronze Sword"}],
            "listEntities": [{"code": "Greydwarf_Elite", "name": "Greydwarf Brute"}],
            "executeConsoleCommand": {"success": True},
            "testReachability": {"connectable": True},
            "shutdown": {},
        },
        identify_count=1,
    )
    monkeypatch.setattr(
        hooks.checks, "wait_for_line", lambda *args, **kwargs: (1, "listItems returned 1 item prefab(s)")
    )
    monkeypatch.setattr(hooks.checks, "find_line", lambda *args, **kwargs: (1, "Loading [Takaro Valheim 1.0.0]"))
    monkeypatch.setattr(hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=1))
    monkeypatch.setattr(hooks.checks_lifecycle, "wait_for_count", lambda *args, **kwargs: 2)
    monkeypatch.setattr(
        hooks.subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(args[0], 0, "", ""),
    )

    assert asyncio.run(hooks._check_handshake(run, fake, lambda: True)).status == "pass"
    assert (
        asyncio.run(hooks._check_catalogue(run, fake, lambda: True, "listItems", "items", hooks.ITEM_SPOT)).status
        == "pass"
    )
    assert asyncio.run(hooks._check_action(run, fake)).status == "pass"
    assert asyncio.run(hooks._check_reconnect(run, fake, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_stop(run, fake, [])).status == "pass"
    asyncio.run(hooks.after_protocol(run, fake, lambda: True))
    asyncio.run(hooks.after_shutdown(run, fake, run.ws_url, []))

    failed_run = FakeRun(tmp_path / "failed", wanted=set())
    failed_run.container = None
    failed = CannedSocket(
        {
            "listItems": [],
            "executeConsoleCommand": {"success": False},
            "testReachability": None,
            "shutdown": RuntimeError("closed"),
        },
        reconnects=False,
    )
    monkeypatch.setattr(hooks.checks, "wait_for_line", lambda *args, **kwargs: None)
    monkeypatch.setattr(hooks.checks, "find_line", lambda *args, **kwargs: None)
    monkeypatch.setattr(
        hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=None)
    )
    monkeypatch.setattr(hooks.checks_lifecycle, "wait_for_count", lambda *args, **kwargs: 0)

    assert asyncio.run(hooks._check_handshake(failed_run, failed, lambda: False)).status == "fail"
    assert (
        asyncio.run(
            hooks._check_catalogue(failed_run, failed, lambda: False, "listItems", "items", hooks.ITEM_SPOT)
        ).status
        == "fail"
    )
    assert asyncio.run(hooks._check_action(failed_run, failed)).status == "fail"
    assert asyncio.run(hooks._check_reconnect(failed_run, failed, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_stop(failed_run, failed, [{"path": "missing"}])).status == "fail"
    asyncio.run(hooks.after_protocol(failed_run, failed, lambda: False))
    asyncio.run(hooks.after_shutdown(failed_run, failed, failed_run.ws_url, []))
    assert {check for check, _ in failed_run.skips} == {*hooks.CHECK_IDS, "stop"}


BUILD_SCRIPT = "games/valheim/scripts/build-release.sh"


# --------------------------------------------------------------------------- fixtures


def build_pack_zip(destination: Path, *, version: str = PACK_VERSION, escape: bool = False) -> bytes:
    """The fixture pack as a Thunderstore zip, with fixed timestamps so the bytes are stable."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w") as archive:
        for source in sorted(path for path in PACK_SOURCE.rglob("*") if path.is_file()):
            name = source.relative_to(PACK_SOURCE).as_posix()
            payload = source.read_bytes()
            if name == "manifest.json":
                document = json.loads(payload)
                document["version_number"] = version
                payload = (json.dumps(document, indent=2) + "\n").encode("utf-8")
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            archive.writestr(info, payload)
        if escape:
            archive.writestr(zipfile.ZipInfo("BepInExPack_Valheim/../escaped.txt", (1980, 1, 1, 0, 0, 0)), "nope")
    return destination.read_bytes()


def make_repo(tmp_path: Path, upstream: FakeUpstream, *, pack: bytes) -> Path:
    """A repository copy whose Valheim record points at the fixture depot and this server."""
    root = tmp_path / "repo"
    (root / "catalog").mkdir(parents=True, exist_ok=True)
    for source in sorted(path for path in (REPO_ROOT / "catalog").rglob("*") if path.is_file()):
        destination = root / "catalog" / source.relative_to(REPO_ROOT / "catalog")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(source.read_bytes())
    (root / "maintenance").mkdir(parents=True, exist_ok=True)
    (root / "maintenance" / "tools.lock.json").write_bytes((REPO_ROOT / "maintenance" / "tools.lock.json").read_bytes())
    script = root / BUILD_SCRIPT
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)

    game = root / "catalog" / GAME / "game.json"
    record = json.loads(game.read_text(encoding="utf-8"))
    record["sources"]["thunderstore"]["baseUrl"] = upstream.base_url
    game.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    repin(root, pack=pack)
    return root


def target_path(root: Path) -> Path:
    return root / "catalog" / GAME / "targets" / f"{TARGET}.json"


def read_target(root: Path) -> dict[str, Any]:
    return json.loads(target_path(root).read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def write_target(root: Path, record: dict[str, Any]) -> None:
    target_path(root).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def repin(root: Path, *, manifest: str = PINNED_MANIFEST, pack: bytes) -> dict[str, Any]:
    """Point the copied target at the fixture bytes: real hashes, fixture sizes."""
    tree = DEPOTS / DEPOT / manifest / "tree"
    contents = [path for path in tree.rglob("*") if path.is_file()]
    record = read_target(root)
    server = record["inputs"]["server"]
    server["depots"] = {
        DEPOT: {
            "manifest": manifest,
            "size": sum(path.stat().st_size for path in contents),
            "files": len(contents),
        }
    }
    server["files"] = {
        name: {"sha256": fake.sha256_of(tree / name), "size": (tree / name).stat().st_size}
        for name in sorted(server["files"])
    }
    record["inputs"]["bepinex"]["sha256"] = _sha256(pack)
    record["inputs"]["bepinex"]["size"] = len(pack)
    write_target(root, record)
    return record


@pytest.fixture
def upstream() -> Any:
    """Thunderstore, in process: the pinned download plus the package document."""
    with FakeUpstream() as server:
        yield server


@pytest.fixture
def pack(tmp_path: Path, upstream: FakeUpstream) -> bytes:
    payload = build_pack_zip(tmp_path / "pack" / "pack.zip")
    upstream.add(PACK_PATH, payload)
    document = json.loads(THUNDERSTORE_DOCUMENT.read_text(encoding="utf-8"))
    document["latest"]["download_url"] = f"{upstream.base_url}{PACK_PATH}"
    upstream.add_json(PACK_API, document)
    return payload


@pytest.fixture
def repo(tmp_path: Path, upstream: FakeUpstream, pack: bytes) -> Path:
    return make_repo(tmp_path, upstream, pack=pack)


@pytest.fixture
def dd_log(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    log = tmp_path / "depotdownloader-argv.jsonl"
    for key, value in fake.environment(tmp_path, log).items():
        monkeypatch.setenv(key, value)
    monkeypatch.setenv("FAKE_DD_ROOT", str(DEPOTS))
    return log


def resolve(run: Any, repo: Path) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, repo=repo)
    assert code == 0, err
    return dict(payload)


def install(run: Any, repo: Path, dest: Path, *extra: str) -> tuple[int, Any, str]:
    return run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), *extra, repo=repo)


def tree_hash(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        digest.update(path.relative_to(root).as_posix().encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


# --------------------------------------------------------------------------- the provider

PROVIDER_SOURCE = {
    "id": "thunderstore",
    "provider": "thunderstore",
    "watch": {
        "kind": "framework",
        "component": "valheim",
        "namespace": "denikson",
        "name": "BepInExPack_Valheim",
        "channels": {"latest": {"branch": "release"}},
    },
}


def provider_source(upstream: FakeUpstream, **overrides: Any) -> dict[str, Any]:
    source = {**PROVIDER_SOURCE, "baseUrl": upstream.base_url}
    source["watch"] = {**PROVIDER_SOURCE["watch"], **overrides}  # type: ignore[dict-item]
    return source


def pack_spec(sha256: str | None, size: int | None, *, version: str = PACK_VERSION) -> dict[str, Any]:
    return {
        "kind": "thunderstore-package",
        "source": "thunderstore",
        "namespace": "denikson",
        "name": "BepInExPack_Valheim",
        "version": version,
        "sha256": sha256,
        "size": size,
    }


def test_a_thunderstore_package_is_fetched_by_version_and_verified(
    tmp_path: Path, upstream: FakeUpstream, pack: bytes
) -> None:
    provider = provider_for("thunderstore")
    source = {"baseUrl": upstream.base_url}
    cache = tmp_path / "cache"

    landed = provider.fetch_input(pack_spec(_sha256(pack), len(pack)), source, tmp_path / "out" / "pack.zip", cache)

    assert landed.read_bytes() == pack
    assert upstream.requested == [PACK_PATH]


def test_a_thunderstore_package_with_the_wrong_hash_is_an_integrity_failure(
    tmp_path: Path, upstream: FakeUpstream, pack: bytes
) -> None:
    from takaro_maint.exit_codes import INTEGRITY, MaintError

    provider = provider_for("thunderstore")

    with pytest.raises(MaintError) as caught:
        provider.fetch_input(
            pack_spec("0" * 64, len(pack)), {"baseUrl": upstream.base_url}, tmp_path / "p.zip", tmp_path / "c"
        )

    assert caught.value.code == INTEGRITY


def test_an_unpublished_version_is_reported_as_unavailable(tmp_path: Path, upstream: FakeUpstream, pack: bytes) -> None:
    from takaro_maint.exit_codes import UPSTREAM, MaintError

    provider = provider_for("thunderstore")
    spec = pack_spec(_sha256(pack), len(pack), version="5.4.9999")

    with pytest.raises(MaintError) as caught:
        provider.fetch_input(spec, {"baseUrl": upstream.base_url}, tmp_path / "p.zip", tmp_path / "c")

    assert caught.value.code == UPSTREAM
    assert "404" in str(caught.value)


def test_input_url_names_the_pinned_download_and_never_latest(upstream: FakeUpstream) -> None:
    provider = provider_for("thunderstore")
    source = {"baseUrl": "https://thunderstore.io/"}

    url = provider.input_url(pack_spec(None, None), source)

    assert url == f"https://thunderstore.io{PACK_PATH}"
    assert "latest" not in url
    assert provider.input_url({**pack_spec(None, None), "resolvedCoordinate": "https://cdn/x.zip"}, source) == (
        "https://cdn/x.zip"
    )
    assert provider.input_url({"kind": "steam-depots"}, source) is None


def test_the_latest_version_is_observed_as_a_heads_only_framework(upstream: FakeUpstream, pack: bytes) -> None:
    result = provider_for("thunderstore").observe(provider_source(upstream))

    assert result.status == "ok"
    assert result.history == "heads-only"
    assert result.heads == {"release": PACK_VERSION}
    assert len(result.observations) == 1
    observation = result.observations[0]
    assert observation.rev == PACK_VERSION
    assert observation.kind == "framework"
    assert observation.facts["releaseTime"] == "2026-09-24T06:11:48.030447Z"
    assert observation.facts["artifact"]["url"].endswith(PACK_PATH)
    assert observation.facts["artifact"]["sha256"] is None
    assert "heads-only" in observation.facts["observationLimit"]
    assert observation.identity


def test_a_watch_block_without_namespace_or_name_is_a_usage_error(upstream: FakeUpstream) -> None:
    from takaro_maint.exit_codes import USAGE, MaintError

    source = provider_source(upstream)
    del source["watch"]["name"]

    with pytest.raises(MaintError) as caught:
        provider_for("thunderstore").observe(source)

    assert caught.value.code == USAGE
    assert "name" in str(caught.value)


def test_a_channel_other_than_latest_is_a_usage_error(upstream: FakeUpstream) -> None:
    from takaro_maint.exit_codes import USAGE, MaintError

    source = provider_source(upstream, channels={"beta": {"branch": "beta"}})

    with pytest.raises(MaintError) as caught:
        provider_for("thunderstore").observe(source)

    assert caught.value.code == USAGE
    assert "one published head" in str(caught.value)


def test_a_malformed_latest_version_fails_the_source(upstream: FakeUpstream, pack: bytes) -> None:
    from takaro_maint.exit_codes import UPSTREAM, MaintError

    document = json.loads(THUNDERSTORE_DOCUMENT.read_text(encoding="utf-8"))
    document["latest"]["version_number"] = "5.4"
    upstream.files.pop(PACK_API)
    upstream.add_json(PACK_API, document)

    with pytest.raises(MaintError) as caught:
        provider_for("thunderstore").observe(provider_source(upstream))

    assert caught.value.code == UPSTREAM
    assert "5.4" in str(caught.value)


# --------------------------------------------------------------------------- catalog


def test_catalog_validate_accepts_the_valheim_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert {check["id"] for check in rows} >= {"input-kind-schema", "build-system-schema", "build-script-exists"}
    assert all(check["status"] == "pass" for check in rows), rows
    assert all(check["status"] == "pass" for check in payload["checks"] if check["id"] == "no-floating-words")


def test_targets_resolve_env_for_valheim(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "VALHEIM")

    assert code == 0, payload
    env = payload["env"]
    assert env["VALHEIM_STEAM_DEPOTS"] == f"{DEPOT}:3707659388211752971"
    assert env["VALHEIM_STEAM_APP"] == "896660"
    assert env["VALHEIM_STEAM_BRANCH"] == "public"
    assert env["VALHEIM_ARTIFACT_SERVER_PLUGIN"] == f"takaro-valheim-plugin-{TARGET}-{{version}}.zip"
    assert env["VALHEIM_ARTIFACT_CLIENT_COMPANION"] == f"takaro-valheim-inventory-companion-{TARGET}-{{version}}.zip"
    assert env["VALHEIM_BEPINEX_PACK_VERSION"] == PACK_VERSION
    assert env["VALHEIM_BEPINEX_PACKAGE"] == "denikson/BepInExPack_Valheim"
    assert env["VALHEIM_BEPINEX_URL"].endswith(f"/{PACK_VERSION}/")
    assert env["VALHEIM_REFERENCES_DIR"].endswith(payload["fp16"])
    assert env["VALHEIM_BEPINEX_DIR"].endswith(payload["fp16"])
    assert env["VALHEIM_DEP_SYSTEM_TEXT_JSON_SHA256"]
    references = payload["inputs"]["server"]["files"]
    expected_reference_keys = {
        "assembly_valheim.dll": "VALHEIM_REFERENCE_ASSEMBLY_VALHEIM_SHA256",
        "assembly_utils.dll": "VALHEIM_REFERENCE_ASSEMBLY_UTILS_SHA256",
        "Splatform.dll": "VALHEIM_REFERENCE_SPLATFORM_SHA256",
        "UnityEngine.dll": "VALHEIM_REFERENCE_UNITYENGINE_SHA256",
        "UnityEngine.CoreModule.dll": "VALHEIM_REFERENCE_UNITYENGINE_COREMODULE_SHA256",
    }
    for assembly, key in expected_reference_keys.items():
        path = f"valheim_server_Data/Managed/{assembly}"
        assert env[key] == references[path]["sha256"]
    assert not any(key.endswith("_JAVA") for key in env)
    assert payload["resolvedUrls"]["server"].startswith("steam://app/896660/branch/public/")
    assert payload["resolvedUrls"]["bepinex"].endswith(f"/{PACK_VERSION}/")


# --------------------------------------------------------------------------- steam pin


def test_steam_pin_reports_the_valheim_head_and_flags_the_changed_manifest(run: Any, repo: Path, dd_log: Path) -> None:
    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, repo=repo)

    assert code == 0, payload
    assert payload["changed"] == [DEPOT]
    assert payload["depots"][DEPOT]["manifest"] == HEAD_MANIFEST
    assert "-manifest-only" in fake.argv_log(dd_log)[0]


def test_steam_pin_reports_an_unavailable_manifest_as_upstream(
    run: Any, repo: Path, dd_log: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", HEAD_MANIFEST)

    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, repo=repo)

    assert code == 4, payload
    assert "not falling back to branch head" in payload["error"]


# --------------------------------------------------------------------------- install


def test_install_places_the_exact_depot_and_the_pinned_pack(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, upstream: FakeUpstream
) -> None:
    dest = tmp_path / "server"

    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "installed"
    assert payload["depots"][DEPOT]["manifest"] == PINNED_MANIFEST
    assert (dest / "valheim_server.x86_64").is_file()
    assert (dest / "BepInEx" / "core" / "BepInEx.dll").is_file()
    assert (dest / "doorstop_libs" / "libdoorstop_x64.so").is_file()
    assert (dest / "start_server_bepinex.sh").is_file()
    # The pack's own dotfile is part of the pack; a record-shaped path grammar would drop it.
    assert (dest / ".doorstop_version").is_file()
    # Thunderstore's own packaging is not part of the server install.
    assert not (dest / "manifest.json").exists()
    assert not (dest / "README.md").exists()
    archive = dest / ".takaro" / "inputs" / f"denikson-BepInExPack_Valheim-{PACK_VERSION}.zip"
    assert archive.is_file()
    names = {row["name"] for row in payload["inputs"]}
    assert any(name.startswith("server:") for name in names)
    assert f"bepinex:denikson-BepInExPack_Valheim-{PACK_VERSION}.zip" in names
    assert "bepinex:BepInEx.dll" in names
    for folder in ("config", "plugins", "patchers"):
        assert (dest / "BepInEx" / folder).is_dir()
    assert (dest / "takaro-companion").is_dir()

    before = len(upstream.requested)
    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "already-installed"
    assert len(fake.argv_log(dd_log)) == 1
    assert len(upstream.requested) == before


def test_install_refuses_a_pack_whose_manifest_disagrees_with_the_pin(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, upstream: FakeUpstream
) -> None:
    other = build_pack_zip(tmp_path / "other" / "pack.zip", version="5.4.2333")
    upstream.files[PACK_PATH] = other
    record = read_target(repo)
    record["inputs"]["bepinex"]["sha256"] = _sha256(other)
    record["inputs"]["bepinex"]["size"] = len(other)
    write_target(repo, record)
    dest = tmp_path / "server"

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert "5.4.2333" in payload["error"] and PACK_VERSION in payload["error"]
    assert not (dest / "valheim_server.x86_64").exists()
    assert not list(dest.parent.glob(".staging-*"))


def test_a_pack_entry_that_escapes_the_install_directory_is_refused(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, upstream: FakeUpstream
) -> None:
    """An entry escaping the install directory is refused without changing the install."""
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    before = tree_hash(dest)

    hostile = build_pack_zip(tmp_path / "hostile" / "pack.zip", escape=True)
    upstream.files[PACK_PATH] = hostile
    record = read_target(repo)
    record["inputs"]["bepinex"]["sha256"] = _sha256(hostile)
    record["inputs"]["bepinex"]["size"] = len(hostile)
    write_target(repo, record)

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert not list(tmp_path.rglob("escaped.txt")), "nothing was written outside the archive"
    assert tree_hash(dest) == before, "the existing install is byte-identical"
    assert not list(dest.parent.glob(".staging-*"))


def test_install_refuses_a_pack_with_the_wrong_hash(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, upstream: FakeUpstream
) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    before = tree_hash(dest)
    upstream.files[PACK_PATH] = b"not the pack at all"
    record = read_target(repo)
    record["inputs"]["server"]["files"]["valheim_server.x86_64"]["size"] += 1
    write_target(repo, record)

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert tree_hash(dest) == before


def test_an_unavailable_pack_download_is_upstream(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, upstream: FakeUpstream
) -> None:
    upstream.status_overrides[PACK_PATH] = 503
    dest = tmp_path / "server"

    code, payload, _ = install(run, repo, dest)

    assert code == 4, payload
    assert not dest.exists() or not (dest / "valheim_server.x86_64").exists()


def test_install_reports_an_unavailable_manifest_as_upstream(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", PINNED_MANIFEST)

    code, payload, _ = install(run, repo, tmp_path / "server")

    assert code == 4, payload
    assert "not falling back to branch head" in payload["error"]


def test_ledger_check_refuses_a_replaced_bepinex_core(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    (dest / "BepInEx" / "core" / "BepInEx.dll").write_bytes(b"a different loader")

    code, payload, _ = run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)

    assert code == 7, payload
    assert "BepInEx/core/BepInEx.dll" in json.dumps(payload)


def test_an_upgrade_preserves_config_and_plugins_and_can_be_rolled_back(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, upstream: FakeUpstream
) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    config = dest / "BepInEx" / "config" / "com.takaro.valheim.cfg"
    config.write_text("[Takaro]\nserverName = keep me\n", encoding="utf-8")
    plugin = dest / "BepInEx" / "plugins" / "TakaroValheim" / "TakaroValheim.dll"
    plugin.parent.mkdir(parents=True, exist_ok=True)
    plugin.write_bytes(b"a deployed plugin")
    first = json.loads((dest / ".takaro" / "installed-target.json").read_text())

    repin(repo, manifest=HEAD_MANIFEST, pack=upstream.files[PACK_PATH])
    record = read_target(repo)
    record["inputs"]["server"]["depots"][DEPOT]["manifest"] = HEAD_MANIFEST
    write_target(repo, record)

    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "installed"
    assert config.read_text() == "[Takaro]\nserverName = keep me\n"
    assert plugin.read_bytes() == b"a deployed plugin"
    assert (dest.parent / f"{dest.name}.previous").is_dir()

    code, payload, err = install(run, repo, dest, "--rollback")

    assert code == 0, f"{err}\n{payload}"
    assert json.loads((dest / ".takaro" / "installed-target.json").read_text())["target"] == first["target"]


# --------------------------------------------------------------------------- build


def write_build_stub(repo: Path, fingerprint: str, *, names: tuple[str, ...] = (PLUGIN_ZIP, COMPANION_ZIP)) -> None:
    """A stand-in for the release script: the same contract, none of the .NET SDK."""
    roles = {PLUGIN_ZIP: "server-plugin", COMPANION_ZIP: "client-companion"}
    lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        'version="$1"; out="$2"',
        'mkdir -p "$out"',
    ]
    for name in names:
        folder = "TakaroValheim" if "plugin" in name else "TakaroValheimInventoryCompanion"
        role = roles.get(name, "server-plugin" if "plugin" in name else "client-companion")
        lines += [
            f'stage="$out/stage-{folder}"',
            f'mkdir -p "$stage/{folder}"',
            f"printf 'assembly\\n' > \"$stage/{folder}/{folder}.dll\"",
            f'printf \'{{"name": "{folder}", "productVersion": "%s"}}\\n\' "$version"'
            f' > "$stage/{folder}/manifest.json"',
            f"( cd \"$stage\" && python3 -c \"import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.',"
            f' \'{folder}\')" "$out/{name[:-4]}" )',
            f'cat > "$out/{name}.meta.json" <<JSON',
            f'{{"target": "{TARGET}", "fingerprint": "{fingerprint}", "connectorVersion": "$version",'
            f' "game": "valheim", "platform": "linux", "revision": "{REVISION}", "role": "{role}"}}',
            "JSON",
        ]
    script = repo / BUILD_SCRIPT
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    script.chmod(0o755)


def test_build_selects_both_role_zips_by_exact_name(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(repo, resolved["fingerprint"])
    out = tmp_path / "dist"

    code, payload, err = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(out), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert sorted(row["role"] for row in payload["artifacts"]) == ["client-companion", "server-plugin"]
    assert {row["file"] for row in payload["artifacts"]} == {PLUGIN_ZIP, COMPANION_ZIP}
    for name in (PLUGIN_ZIP, COMPANION_ZIP):
        assert (out / name).is_file()
        assert (out / f"{name}.meta.json").is_file()


def test_the_build_re_execs_into_the_pinned_image_unless_the_caller_asks_for_the_host(
    run: Any, repo: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """``--toolchain`` is the CLI's default, not a promise: only the environment picks host."""
    resolved = resolve(run, repo)
    write_build_stub(repo, resolved["fingerprint"])
    script = repo / BUILD_SCRIPT
    script.write_text(
        script.read_text(encoding="utf-8").replace(
            'mkdir -p "$out"',
            'mkdir -p "$out"\nprintf \'%s\\n\' "${VALHEIM_BUILD_TOOLCHAIN:-unset}" > "$out/toolchain"',
        ),
        encoding="utf-8",
    )
    # The script writes into the target's own dist directory; --out is where the CLI
    # copies the selected artifacts afterwards.
    chosen = repo / "games/valheim/_data/dist" / resolved["fp16"] / "toolchain"

    code, payload, err = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "dist"), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert chosen.read_text(encoding="utf-8").strip() == "container"

    monkeypatch.setenv("VALHEIM_BUILD_TOOLCHAIN", "host")
    code, payload, err = run(
        "build",
        "--game",
        GAME,
        "--target",
        TARGET,
        "--version",
        VERSION,
        "--out",
        str(tmp_path / "dist-host"),
        repo=repo,
    )

    assert code == 0, f"{err}\n{payload}"
    assert chosen.read_text(encoding="utf-8").strip() == "host"


def test_a_build_missing_the_companion_is_refused(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(repo, resolved["fingerprint"], names=(PLUGIN_ZIP,))

    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "dist"), repo=repo
    )

    assert code == 7, payload
    assert "did not produce" in payload["error"]


def test_a_build_that_writes_the_legacy_names_is_refused(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(
        repo, resolved["fingerprint"], names=("takaro-valheim-plugin.zip", "takaro-valheim-inventory-companion.zip")
    )

    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "dist"), repo=repo
    )

    assert code == 7, payload
    assert "did not produce" in payload["error"]


# --------------------------------------------------------------------------- deploy


def role_zip(path: Path, folder: str, *, escape: bool = False, second_folder: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(f"{folder}/{folder}.dll", "assembly")
        archive.writestr(f"{folder}/manifest.json", json.dumps({"name": folder, "productVersion": VERSION}))
        if escape:
            archive.writestr("../escaped.txt", "nope")
        if second_folder:
            archive.writestr("Other/Other.dll", "nope")


def manifest_for(run: Any, repo: Path, directory: Path) -> Path:
    resolved = resolve(run, repo)
    rows = [
        artifact_row("client-companion", TARGET, resolved["fingerprint"], directory / COMPANION_ZIP),
        artifact_row("server-plugin", TARGET, resolved["fingerprint"], directory / PLUGIN_ZIP),
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


def test_deploy_unpacks_the_plugin_and_parks_the_companion(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    stale_cache = dest / "BepInEx" / "cache" / "chainloader_typeloader.dat"
    stale_cache.parent.mkdir(parents=True, exist_ok=True)
    stale_cache.write_bytes(b"stale type metadata")
    (dest / "BepInEx" / "plugins" / "takaro-valheim-plugin-linux-1.0.15-3.0.2.zip").write_bytes(b"older")
    directory = tmp_path / "dist"
    role_zip(directory / PLUGIN_ZIP, "TakaroValheim")
    role_zip(directory / COMPANION_ZIP, "TakaroValheimInventoryCompanion")
    manifest = manifest_for(run, repo, directory)

    code, payload, err = run(
        "deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert (dest / "BepInEx" / "plugins" / "TakaroValheim" / "TakaroValheim.dll").is_file()
    assert not stale_cache.exists()
    assert not (dest / "BepInEx" / "plugins" / "takaro-valheim-plugin-linux-1.0.15-3.0.2.zip").exists()
    parked = dest / "takaro-companion" / COMPANION_ZIP
    assert parked.is_file()
    assert not (dest / "takaro-companion" / "TakaroValheimInventoryCompanion").exists()
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    by_role = {row["role"]: row["path"] for row in ledger["artifacts"]}
    assert by_role["server-plugin"].endswith(PLUGIN_ZIP)
    assert by_role["client-companion"].endswith(COMPANION_ZIP)


def test_deploy_refuses_a_role_valheim_does_not_ship(tmp_path: Path) -> None:
    from takaro_maint.exit_codes import ConflictError

    dest = tmp_path / "server"
    artifact = tmp_path / "dist" / "takaro-valheim-bogus.zip"
    role_zip(artifact, "TakaroValheimBogus")

    with pytest.raises(ConflictError, match="not 'bogus'"):
        adapter_for(GAME).after_deploy(dest, {"role": "bogus", "installDir": "BepInEx/plugins"}, artifact)

    assert not (dest / "BepInEx" / "plugins" / "TakaroValheimBogus").exists()
    assert not (dest / "BepInEx" / "plugins" / "TakaroValheim").exists()


def test_a_plugin_zip_with_a_second_top_level_folder_is_refused(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    role_zip(directory / PLUGIN_ZIP, "TakaroValheim", second_folder=True)
    role_zip(directory / COMPANION_ZIP, "TakaroValheimInventoryCompanion")
    manifest = manifest_for(run, repo, directory)

    code, payload, _ = run(
        "deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo
    )

    assert code == 7, payload
    assert not (dest / "BepInEx" / "plugins" / "TakaroValheim").exists()
    assert not (dest / "BepInEx" / "plugins" / "Other").exists()


def test_a_plugin_zip_that_fails_half_way_leaves_the_installed_plugin_in_place(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    """The extraction is staged: an archive that dies mid-read must not take the live plugin."""
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    role_zip(directory / PLUGIN_ZIP, "TakaroValheim")
    role_zip(directory / COMPANION_ZIP, "TakaroValheimInventoryCompanion")
    manifest = manifest_for(run, repo, directory)
    assert (
        run("deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo)[0]
        == 0
    )
    installed = dest / "BepInEx" / "plugins" / "TakaroValheim" / "TakaroValheim.dll"
    assert installed.read_text() == "assembly"

    # A well-formed zip whose stored bytes no longer match their CRC: the entry list reads
    # fine, so the refusal can only come out of the extraction itself.
    stored = b"a" * 4096
    with zipfile.ZipFile(directory / PLUGIN_ZIP, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("TakaroValheim/TakaroValheim.dll", stored)
        archive.writestr("TakaroValheim/manifest.json", json.dumps({"name": "TakaroValheim"}))
    raw = bytearray((directory / PLUGIN_ZIP).read_bytes())
    raw[raw.index(stored) + 16] ^= 0xFF
    (directory / PLUGIN_ZIP).write_bytes(bytes(raw))
    manifest = manifest_for(run, repo, directory)

    code, payload, _ = run(
        "deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo
    )

    assert code == 7, payload
    assert installed.read_text() == "assembly"
    assert sorted(path.name for path in installed.parent.iterdir()) == ["TakaroValheim.dll", "manifest.json"]
    assert list((dest / ".takaro" / "deploy").iterdir()) == []


# --------------------------------------------------------------------------- hooks


def test_verify_hooks_render_config_and_use_the_valheim_lines(tmp_path: Path) -> None:
    written = hooks.render_config(
        tmp_path,
        {
            "TAKARO_WS_URL": "ws://host.docker.internal:27148/",
            "TAKARO_IDENTITY_TOKEN": "takaro-verify-tm148-161",
            "TAKARO_REGISTRATION_TOKEN": "a-throwaway-registration-token",
        },
    )

    assert written == tmp_path / "BepInEx" / "config" / "com.takaro.valheim.cfg"
    assert oct(written.stat().st_mode)[-3:] == "600"
    body = written.read_text()
    assert "companion" not in body.lower()
    assert "takaroWsUrl = ws://host.docker.internal:27148/" in body
    assert "commandAllowlistExact = help" in body
    assert hooks.RECONNECT_BUDGET >= 120


def test_the_hook_patterns_match_the_lines_the_server_really_writes() -> None:
    # Captured from an isolated run on this exact pin.
    assert hooks.READY_LINE.search("09/21/2026 17:52:48: Game server connected")
    assert hooks.HANDSHAKE_LINE.search("[Info   :Takaro Valheim] Takaro Valheim identified as gameServerId=3f0c..")
    loaded = hooks.LOADED_LINE.search("[Info   :   BepInEx] Loading [Takaro Valheim 3.0.3]")
    assert loaded is not None and loaded.group("version") == "3.0.3"
    counted = hooks.LIST_ITEMS_LINE.search(
        "[Info   :Takaro Valheim] Takaro Valheim listItems returned 512 item prefab(s)."
    )
    assert counted is not None and counted.group("count") == "512"


def test_the_runtime_identity_comes_from_the_valheim_banner() -> None:
    adapter = adapter_for(GAME)

    banner = adapter.parse_runtime_identity("09/21/2026 17:52:20: Valheim version: l-1.0.15 (network version 40)")
    loader = adapter.parse_runtime_identity("[Message:   BepInEx] BepInEx 5.4.23.5 - valheim_server (9/21/2026)")

    assert banner == {"gameVersion": "1.0.15", "loader": "bepinex", "loaderVersion": None}
    assert loader == {"loader": "bepinex", "loaderVersion": "5.4.23.5"}
    assert adapter.parse_runtime_identity("world version 34") is None


def test_the_check_ids_add_the_valheim_lifecycle_checks() -> None:
    from takaro_maint.verify.runner import check_ids

    assert {"handshake", "items", "entities", "action", "reconnect", "stop"} <= set(check_ids(GAME))


# --------------------------------------------------------------------------- the release record


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


def test_compat_record_carries_both_roles_the_steam_pin_and_the_pack(run: Any, repo: Path, tmp_path: Path) -> None:
    git(repo, "init", "-q", "-b", "main")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "fixture")
    commit = git(repo, "rev-parse", "HEAD")
    resolved = resolve(run, repo)
    directory = tmp_path / "dist" / TARGET
    role_zip(directory / PLUGIN_ZIP, "TakaroValheim")
    role_zip(directory / COMPANION_ZIP, "TakaroValheimInventoryCompanion")
    rows = [
        artifact_row("client-companion", TARGET, resolved["fingerprint"], directory / COMPANION_ZIP),
        artifact_row("server-plugin", TARGET, resolved["fingerprint"], directory / PLUGIN_ZIP),
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
        "pr-1-valheim",
        "--dist",
        str(tmp_path / "dist"),
        "--out",
        str(out),
        "--repo",
        "o/r",
        repo=repo,
    )

    assert code == 0, f"{err}\n{payload}"
    record = json.loads((out / f"takaro-valheim-{VERSION}.compat.json").read_text())
    entry = record["targets"][TARGET]
    assert sorted(row["role"] for row in entry["artifacts"]) == ["client-companion", "server-plugin"]
    assert {row["name"] for row in entry["artifacts"]} == {PLUGIN_ZIP, COMPANION_ZIP}
    assert entry["inputs"]["server"]["url"].startswith("steam://app/896660/")
    assert entry["inputs"]["bepinex"]["url"].endswith(f"/{PACK_VERSION}/")
    assert entry["verification"]["required"] == "contract"
    assert entry["verification"]["executed"] is None
    for name in (PLUGIN_ZIP, COMPANION_ZIP):
        assert (out / name).is_file()
    assert (out / "takaro-valheim-plugin.zip").read_bytes() == (out / PLUGIN_ZIP).read_bytes()
    assert (out / "takaro-valheim-inventory-companion.zip").read_bytes() == (out / COMPANION_ZIP).read_bytes()


# --------------------------------------------------------------------------- the rig

DS_ROOT = REPO_ROOT / "dev-servers"


def bash(script: str) -> str:
    completed = subprocess.run(["bash", "-c", script], cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


def test_dev_servers_valheim_dispatches_through_the_target() -> None:
    completed = subprocess.run(
        ["bash", "-n", str(DS_ROOT / "lib" / "games" / "valheim.sh")], capture_output=True, text=True, check=False
    )
    assert completed.returncode == 0, completed.stderr

    assert bash(". dev-servers/lib/common.sh; ds_target_prefix valheim").strip() == "VALHEIM"
    assert bash(". dev-servers/lib/common.sh; ds_target_dest valheim").strip().endswith("/valheim/server")
    for step in ("install", "deploy"):
        found = bash(f'. dev-servers/lib/common.sh; declare -F "{step}_valheim" >/dev/null && echo yes')
        assert found.strip() == "yes", f"valheim has no {step} step"
    assert "ds_valheim_builder_image" not in (DS_ROOT / "lib" / "games" / "valheim.sh").read_text()


# ------------------------------------------------------- the catalogue check, directly


class _CatalogueFake:
    """Only what `_check_catalogue` reaches for: one request and one canned answer."""

    def __init__(self, entries: Any) -> None:
        self.entries = entries
        self.identify_count = 1

    async def request(self, action: str, params: Any, timeout: float | None = None) -> Any:
        del action, params, timeout
        return self.entries


def _catalogue_run(tmp_path: Path) -> Any:
    from types import SimpleNamespace

    log = tmp_path / "server.log"
    log.write_text("boot\n", encoding="utf-8")
    return SimpleNamespace(server_log=log, wanted=lambda check_id: True)


def _catalogue(tmp_path: Path, entries: Any, *, check_id: str = "entities") -> Any:
    request = "listItems" if check_id == "items" else "listEntities"
    spot = hooks.ITEM_SPOT if check_id == "items" else hooks.ENTITY_SPOT
    return asyncio.run(
        hooks._check_catalogue(_catalogue_run(tmp_path), _CatalogueFake(entries), lambda: True, request, check_id, spot)
    )


def test_the_catalogue_check_passes_on_display_names(tmp_path: Path) -> None:
    result = _catalogue(
        tmp_path,
        [{"code": "Greydwarf_Elite", "name": "Greydwarf Brute"}, {"code": "Boar", "name": "Boar"}],
    )

    # "Boar" is both the code and the name, which is what a real display name looks like
    # when the prefab happens to be spelled the way a player reads it.
    assert result.status == "pass", result.detail["problems"]
    assert result.detail["humanNames"] is True


def test_the_catalogue_check_requires_the_spot_code_and_expected_name(tmp_path: Path) -> None:
    missing = _catalogue(tmp_path, [{"code": "Boar", "name": "Boar"}])
    assert missing.status == "fail"
    assert missing.detail["spotCheck"] == {
        "code": "Greydwarf_Elite",
        "expected": "Greydwarf Brute",
        "actual": None,
    }

    wrong = _catalogue(tmp_path, [{"code": "Greydwarf_Elite", "name": "Greydwarf Elite"}])
    assert wrong.status == "fail"
    assert "expected 'Greydwarf Brute'" in " ".join(wrong.detail["problems"])


def test_a_compound_code_handed_back_as_its_own_name_is_a_dev_name(tmp_path: Path) -> None:
    """`SwordBronze` is not a translation of `SwordBronze`; `Boar` is a word."""
    result = _catalogue(tmp_path, [{"code": "SwordBronze", "name": "SwordBronze"}], check_id="items")

    assert result.status == "fail"
    assert "SwordBronze -> SwordBronze" in " ".join(result.detail["problems"])


def test_the_catalogue_check_fails_on_translation_keys(tmp_path: Path) -> None:
    """The check fails on translation keys."""
    result = _catalogue(
        tmp_path,
        [
            {"code": "SwordBronze", "name": "$item_sword_bronze"},
            {"code": "Greydwarf_Elite", "name": "$enemy_greydwarfbrute"},
            {"code": "Deer", "name": "Deer"},
        ],
    )

    assert result.status == "fail"
    problems = " ".join(result.detail["problems"])
    assert "translation keys or class names" in problems
    assert "2 of 3" in problems, "`Deer` is the word a player reads, not a dev name"
    assert "SwordBronze -> $item_sword_bronze" in problems
    assert result.detail["humanNames"] is False


def test_the_catalogue_check_fails_on_a_class_name(tmp_path: Path) -> None:
    result = _catalogue(tmp_path, [{"code": "Greydwarf", "name": "enemy_greydwarf"}])

    assert result.status == "fail"
    assert "enemy_greydwarf" in " ".join(result.detail["problems"])


def test_valheim_runs_its_catalogue_checks_by_default() -> None:
    """Known translation-key failures remain visible in a default verification report."""
    from takaro_maint.verify.runner import check_ids, game_hooks

    unsupported = game_hooks(GAME).unsupported_checks
    assert {"items", "entities"}.isdisjoint(unsupported)
    assert {"items", "entities"} <= set(check_ids(GAME))
