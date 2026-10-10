"""The Rust target end to end: catalog, resolution, pin, install, build, deploy, scan.

Rust is the first game whose install has two halves — Steam depots plus a framework that
ships as a GitHub release asset — so most of what is asserted here is about the seam
between them: that the framework is verified before anything is swapped into place, that
the ledger guards it afterwards, and that a run which cannot get it leaves the existing
install exactly as it was.

Everything drives the real command against the DepotDownloader stand-in, a real HTTP
upstream serving the fixture archive, and the recorded Steam document — so the assertions
are about what a maintainer, the rig and CI observe.
"""

from __future__ import annotations

import asyncio
import io
import json
import os
import re
import shutil
import subprocess
import tarfile
from pathlib import Path
from typing import Any

import pytest

import fake_depotdownloader as fake
import fake_steamcmd
from fake_github import FakeGitHub
from fake_upstream import FakeUpstream
from fake_verify import CannedSocket, FakeRun
from takaro_maint.games import adapter_for
from takaro_maint.games.rust import verify as hooks
from takaro_maint.install.ledger import read_ledger
from takaro_maint.publish.manifest import artifact_row, write_manifest, write_meta
from takaro_maint.steam import vdf
from takaro_maint.tracker import identity

GAME = "rust"
TARGET = "carbon-25823813"
APP = 258550
VERSION = "0.0.6-dev.abc1234"
ARTIFACT = f"takaro-rust-plugin-{TARGET}-{VERSION}.cs"
MANAGED = "RustDedicated_Data/Managed"
BUILD_SCRIPT = "games/rust/scripts/build-release.sh"

REPO_ROOT = Path(__file__).resolve().parents[2]


def test_every_rust_verification_body_has_pass_and_failure_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    run = FakeRun(tmp_path)
    fake = CannedSocket({"sendMessage": {"success": True}, "shutdown": {}}, identify_count=1)
    loaded = "Loaded plugin TakaroConnector v1.2.3 by Takaro [42ms]"
    monkeypatch.setattr(hooks, "_deployed_version", lambda run: "1.2.3")
    monkeypatch.setattr(hooks.checks, "wait_for_line", lambda *args, **kwargs: (1, loaded))
    monkeypatch.setattr(hooks.checks, "find_line", lambda *args, **kwargs: None)

    assert asyncio.run(hooks._check_carbon_compile(run, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_action(run, fake, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_stop(run, fake)).status == "pass"
    assert run.container is not None
    run.container.exit_code = -1
    timed_out = asyncio.run(hooks._check_stop(run, fake))
    assert timed_out.status == "fail"
    assert timed_out.detail["exitCode"] == -1
    assert "still running" in " ".join(timed_out.detail["problems"])
    run.container.exit_code = 0

    async def passed_catalog(*args: Any, **kwargs: Any) -> Any:
        del kwargs
        return hooks.checks.CheckResult(str(args[2]), "pass", 0, {"problems": []})

    async def passed_reconnect(*args: Any, **kwargs: Any) -> Any:
        del args, kwargs
        return hooks.checks.CheckResult("reconnect", "pass", 0, {"problems": []})

    monkeypatch.setattr(hooks.checks, "check_catalog", passed_catalog)
    monkeypatch.setattr(hooks.checks_lifecycle, "check_reconnect", passed_reconnect)
    asyncio.run(hooks.after_protocol(run, fake, lambda: True))
    asyncio.run(hooks.after_shutdown(run, fake, run.ws_url, []))

    failed_run = FakeRun(tmp_path / "failed", wanted=set())
    failed_run.container = None
    failed = CannedSocket({"sendMessage": RuntimeError("no action"), "shutdown": RuntimeError("closed")})
    monkeypatch.setattr(hooks.checks, "wait_for_line", lambda *args, **kwargs: None)
    monkeypatch.setattr(hooks.checks, "find_line", lambda *args, **kwargs: (1, "error CS1000"))

    assert asyncio.run(hooks._check_carbon_compile(failed_run, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_action(failed_run, failed, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_stop(failed_run, failed)).status == "fail"
    asyncio.run(hooks.after_protocol(failed_run, failed, lambda: False))
    asyncio.run(hooks.after_shutdown(failed_run, failed, failed_run.ws_url, []))
    assert {check for check, _ in failed_run.skips} == {*hooks.CHECK_IDS, "stop"}


FIXTURES = Path(__file__).parent / "fixtures" / "games" / "rust"
DEPOTS = FIXTURES / "depots"
CARBON_TARBALL = FIXTURES / "carbon" / "Carbon.Linux.Release.tar.gz"
STEAM_DOCUMENT = FIXTURES / "steam" / "app_info_258550.vdf"

PINNED = {"258552": "3352454092778561960", "258554": "4408100835840826754"}
HEADS = json.loads((DEPOTS / "head.json").read_text(encoding="utf-8"))

#: Where the fake upstream serves Carbon from, in both of its roles: the release listing
#: the scan reads and the asset byte stream the install fetches.
LISTING_PATH = "/repos/CarbonCommunity/Carbon/releases?per_page=10"
ASSET_PATH = "/CarbonCommunity/Carbon/releases/download/production_build/Carbon.Linux.Release.tar.gz"
CARBON_LISTING = Path(__file__).parent / "fixtures/providers/github_release/carbon-releases.json"

TRACKER_REPO = "gettakaro/connectors"
TOKEN = "token-for-tests"


# --------------------------------------------------------------------------- the rig


def target_path(root: Path) -> Path:
    return root / "catalog" / GAME / "targets" / f"{TARGET}.json"


def read_target(root: Path) -> dict[str, Any]:
    return json.loads(target_path(root).read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def write_target(root: Path, record: dict[str, Any]) -> None:
    target_path(root).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def _fixture_tree(depot: str, manifest: str) -> Path:
    return DEPOTS / depot / manifest / "tree"


def _find(name: str, manifests: dict[str, str]) -> Path:
    for depot, manifest in manifests.items():
        candidate = _fixture_tree(depot, manifest) / name
        if candidate.is_file():
            return candidate
    raise AssertionError(f"no fixture depot holds {name}")


def repin(root: Path, manifests: dict[str, str] | None = None) -> dict[str, Any]:
    """Point the copied Rust target at the fixture depots and the fixture Carbon archive."""
    manifests = manifests or PINNED
    record = read_target(root)
    server = record["inputs"]["server"]
    # The synthetic depot tree models the earlier build that supplied these fixtures.
    server["buildid"] = 25353106
    record["revision"] = "25353106"
    server["depots"] = {
        depot: {
            "manifest": manifest,
            "size": sum(f.stat().st_size for f in _fixture_tree(depot, manifest).rglob("*") if f.is_file()),
            "files": len([f for f in _fixture_tree(depot, manifest).rglob("*") if f.is_file()]),
        }
        for depot, manifest in manifests.items()
    }
    server["files"] = {
        name: {"sha256": fake.sha256_of(_find(name, manifests)), "size": _find(name, manifests).stat().st_size}
        for name in sorted(server["files"])
    }
    carbon = record["inputs"]["carbon"]
    carbon["sha256"] = fake.sha256_of(CARBON_TARBALL)
    carbon["size"] = CARBON_TARBALL.stat().st_size
    write_target(root, record)
    return record


def point_at(root: Path, base_url: str) -> None:
    """Both Carbon sources — the release listing and the download — at the fake upstream."""
    path = root / "catalog" / GAME / "game.json"
    game = json.loads(path.read_text(encoding="utf-8"))
    for source_id in ("carbon-api", "carbon-download"):
        game["sources"][source_id]["baseUrl"] = base_url
    path.write_text(json.dumps(game, indent=2) + "\n", encoding="utf-8")


def make_rust_repo(tmp_path: Path, base_url: str | None = None) -> Path:
    """A repository copy holding the real catalog, the tool lock and a stub build script."""
    root = fake.make_repo(tmp_path, repin_target=False)
    script = root / BUILD_SCRIPT
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)
    repin(root)
    if base_url is not None:
        point_at(root, base_url)
    return root


@pytest.fixture
def upstream() -> Any:
    """The fake github.com/api.github.com: the Carbon asset and the release listing."""
    with FakeUpstream() as server:
        server.add_file(ASSET_PATH, CARBON_TARBALL)
        server.add_json(LISTING_PATH, json.loads(CARBON_LISTING.read_text(encoding="utf-8")))
        yield server


@pytest.fixture
def repo(tmp_path: Path, upstream: Any) -> Path:
    return make_rust_repo(tmp_path, upstream.base_url)


@pytest.fixture
def dd_log(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    log = tmp_path / "depotdownloader-argv.jsonl"
    for key, value in fake.environment(tmp_path, log, FAKE_DD_ROOT=str(DEPOTS)).items():
        monkeypatch.setenv(key, value)
    return log


def resolve(run: Any, repo: Path) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, repo=repo)
    assert code == 0, err
    return dict(payload)


def install(run: Any, repo: Path, dest: Path, *extra: str) -> tuple[int, Any, str]:
    return run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), *extra, repo=repo)


def tree_hash(root: Path) -> str:
    from takaro_maint.commands.install import tree_hash as _tree_hash

    return _tree_hash(root)


def staging_siblings(dest: Path) -> list[Path]:
    return sorted(dest.parent.glob(f"{dest.name}.staging-*"))


# --------------------------------------------------------------------------- catalog


def test_catalog_validate_accepts_the_rust_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert {check["id"] for check in rows} >= {
        "input-kind-schema",
        "build-system-schema",
        "build-script-exists",
        "no-null-hash",
        "immutable-tag-and-digest",
        "no-floating-words",
    }
    assert all(check["status"] == "pass" for check in rows), [row for row in rows if row["status"] != "pass"]


def test_targets_resolve_env_for_rust(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "RUST")

    assert code == 0, payload
    env = payload["env"]
    assert env["RUST_STEAM_APP"] == "258550"
    assert env["RUST_STEAM_BRANCH"] == "public"
    assert env["RUST_STEAM_BUILDID"] == "25823813"
    assert env["RUST_STEAM_DEPOTS"] == "258552:7153781140680300876;258554:6106791652778979593"
    assert env["RUST_ARTIFACT"] == "takaro-rust-plugin-carbon-25823813-{version}.cs"
    assert env["RUST_CARBON_ASSET"] == "Carbon.Linux.Release.tar.gz"
    assert env["RUST_CARBON_TAG"] == "production_build"
    assert env["RUST_CARBON_SHA256"] == "ff0e1bb8c81edd2a4a31df39776f231004840af4c2f71ba9acbbe465b9479037"
    assert env["RUST_CARBON_DOWNLOAD_URL"] == (
        "https://github.com/CarbonCommunity/Carbon/releases/download/production_build/Carbon.Linux.Release.tar.gz"
    )
    assert env["RUST_REFERENCES_DIR"].endswith(payload["fp16"])
    assert env["RUST_CARBON_REFERENCES_DIR"].endswith(payload["fp16"])
    assert env["RUST_DEP_BEPINEX_ASSEMBLYPUBLICIZER_MSBUILD_SHA256"]
    # This server ships its own runtime; there is no JVM anywhere in the record.
    assert not any(key.endswith("_JAVA") for key in env)

    assert payload["resolvedUrls"]["server"].startswith(
        "steam://app/258550/branch/public/build/25823813/depot/258552/manifest/7153781140680300876"
    )
    assert "258554/manifest/6106791652778979593" in payload["resolvedUrls"]["server"]
    # The Carbon asset is addressed by the URL that actually serves the bytes: the API's
    # asset-id URL answers with JSON unless the request asks for octet-stream, which
    # `catalog validate --online` (and anything else that re-hashes it) cannot do.
    assert payload["resolvedUrls"]["carbon"] == env["RUST_CARBON_DOWNLOAD_URL"]
    assert payload["containerRef"] == (
        "mcr.microsoft.com/dotnet/runtime-deps:8.0.31-noble@sha256:"
        "55cde4ac72e935f8a1ac5bd34f900127e6983d420be979a4b407d59ae17f883e"
    )


# --------------------------------------------------------------------------- steam pin


def test_steam_pin_reports_both_depot_heads_and_flags_the_moved_ones(run: Any, repo: Path, dd_log: Path) -> None:
    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, repo=repo)

    assert code == 0, payload
    assert sorted(payload["changed"]) == ["258552", "258554"]
    assert {depot: entry["manifest"] for depot, entry in payload["depots"].items()} == HEADS
    assert {depot: entry["manifest"] for depot, entry in payload["snippet"]["depots"].items()} == HEADS
    assert all("-manifest-only" in argv for argv in fake.argv_log(dd_log))


# --------------------------------------------------------------------------- install


def test_install_places_the_game_and_carbon_and_writes_the_ledger(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "rust_dedicated"

    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "installed"
    # The Steam half.
    assert (dest / "RustDedicated").is_file()
    assert (dest / MANAGED / "Assembly-CSharp.dll").is_file()
    assert (dest / "Bundles/shared/items.preload.bundle").is_file()
    # The Carbon half, unpacked out of the verified archive.
    assert (dest / "carbon/managed/Carbon.dll").is_file()
    assert (dest / "libdoorstop.so").is_file()
    assert (dest / "carbon/tools/environment.sh").is_file()
    assert (dest / "carbon/plugins").is_dir()
    assert (dest / "takaro/Carbon.Linux.Release.tar.gz").is_file()
    assert (dest / "takaro/home").is_dir()
    assert (dest / "server").is_dir()
    # Steam's manifests carry no POSIX mode and DepotDownloader writes 0644, so the server
    # binary and the scripts beside it arrive unrunnable unless the install fixes them.
    assert os.access(dest / "RustDedicated", os.X_OK)
    assert os.access(dest / "runds.sh", os.X_OK)
    assert os.access(dest / "carbon/tools/environment.sh", os.X_OK)
    # Carbon's self-updater would replace the pinned assemblies on the first boot.
    # Carbon's own shape: an object, not a flag. A document its preloader cannot
    # deserialise takes the framework down silently and the server boots unmodded.
    assert json.loads((dest / "carbon/config.json").read_text())["SelfUpdating"] == {
        "Enabled": False,
        "HookUpdates": False,
        "RedirectUri": None,
    }

    ledger = read_ledger(dest)
    assert ledger is not None
    names = {row["name"] for row in ledger.data["inputs"]}
    assert names == {f"server:{path}" for path in read_target(repo)["inputs"]["server"]["files"]} | {
        "carbon:takaro/Carbon.Linux.Release.tar.gz",
        "carbon:carbon/managed/Carbon.dll",
        "carbon:carbon/managed/Carbon.Common.dll",
        "carbon:libdoorstop.so",
        "carbon:carbon/tools/environment.sh",
    }
    assert run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)[0] == 0

    downloads = len(fake.argv_log(dd_log))
    requests = len(upstream.requested)

    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "already-installed"
    assert len(fake.argv_log(dd_log)) == downloads, "a second install ran DepotDownloader again"
    assert len(upstream.requested) == requests, "a second install fetched Carbon again"


def test_an_altered_carbon_asset_is_refused_and_the_install_is_untouched(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "rust_dedicated"
    assert install(run, repo, dest)[0] == 0
    before = tree_hash(dest)
    # What a re-uploaded rolling tag looks like from here: the same URL, and bytes that are
    # not the ones the record pins. (The cache is content-addressed, so the way to say
    # "these are not those bytes" is to ask for a digest the upstream does not serve.)
    record = read_target(repo)
    record["inputs"]["carbon"]["sha256"] = "0" * 64
    write_target(repo, record)

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert "carbon" in json.dumps(payload).lower()
    assert tree_hash(dest) == before
    assert staging_siblings(dest) == []


def test_an_unavailable_carbon_asset_exits_four(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "rust_dedicated"
    assert install(run, repo, dest)[0] == 0
    before = tree_hash(dest)
    record = read_target(repo)
    record["inputs"]["carbon"]["asset"] = "Carbon.Linux.Nothing.tar.gz"
    write_target(repo, record)
    # The blob cache is content-addressed, so it would answer this happily; the question
    # here is what happens when the bytes have to come from upstream and upstream has none.
    shutil.rmtree(Path(os.environ["TAKARO_MAINT_CACHE"]) / "blobs", ignore_errors=True)

    code, payload, _ = install(run, repo, dest)

    assert code == 4, payload
    assert tree_hash(dest) == before
    assert staging_siblings(dest) == []


def test_an_unavailable_depot_manifest_exits_four_and_never_falls_back(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", PINNED["258552"])
    dest = tmp_path / "rust_dedicated"

    code, payload, _ = install(run, repo, dest)

    assert code == 4, payload
    assert "not falling back to branch head" in payload["error"]
    assert not dest.exists()
    assert staging_siblings(dest) == []


def test_a_reinstall_preserves_server_data_plugins_and_configs_and_keeps_previous(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "rust_dedicated"
    assert install(run, repo, dest)[0] == 0
    first = read_ledger(dest)
    assert first is not None
    (dest / "server/takaro").mkdir(parents=True, exist_ok=True)
    (dest / "server/takaro/x.sav").write_bytes(b"a world nobody wants to lose")
    (dest / "carbon/plugins/Other.cs").write_text("// somebody else's plugin\n")
    (dest / "carbon/configs/a.json").write_text('{"kept": true}\n')
    (dest / "carbon/config.json").write_text('{"SelfUpdating": false, "edited": "by an operator"}\n')
    carbon_before = (dest / "carbon/managed/Carbon.dll").read_bytes()

    repin(repo, HEADS)  # Steam moved; the same Carbon, a different game build

    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "installed"
    assert (dest / "server/takaro/x.sav").read_bytes() == b"a world nobody wants to lose"
    assert (dest / "carbon/plugins/Other.cs").read_text() == "// somebody else's plugin\n"
    assert (dest / "carbon/configs/a.json").read_text() == '{"kept": true}\n'
    assert json.loads((dest / "carbon/config.json").read_text())["edited"] == "by an operator"
    assert (dest / "carbon/managed/Carbon.dll").read_bytes() == carbon_before
    assert (dest / MANAGED / "Assembly-CSharp.dll").read_text() == (
        _find(f"{MANAGED}/Assembly-CSharp.dll", HEADS).read_text()
    )
    previous = json.loads((dest.with_name(dest.name + ".previous") / ".takaro/installed-target.json").read_text())
    assert previous["fingerprint"] == first.fingerprint

    code, payload, err = install(run, repo, dest, "--rollback")

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "rolled-back"
    rolled = read_ledger(dest)
    assert rolled is not None
    assert rolled.fingerprint == first.fingerprint


def test_a_changed_managed_assembly_or_carbon_dll_fails_ledger_check(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "rust_dedicated"
    assert install(run, repo, dest)[0] == 0

    def check() -> int:
        return int(run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)[0])

    for relative in (f"{MANAGED}/Assembly-CSharp.dll", "carbon/managed/Carbon.dll"):
        path = dest / relative
        original = path.read_bytes()
        path.write_bytes(original + b"patched in place")
        assert check() == 7, f"{relative} was changed and ledger check still passed"
        path.write_bytes(original)
        assert check() == 0


# --------------------------------------------------------------------------- references


def test_steam_references_fetches_only_the_managed_assemblies(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "rust-binaries" / "fp"

    def references(*extra: str) -> tuple[int, Any, str]:
        return run("steam", "references", "--game", GAME, "--target", TARGET, "--dest", str(dest), *extra, repo=repo)

    code, payload, err = references()

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "fetched"
    assert any("-filelist" in argv for argv in fake.argv_log(dd_log))
    # Flat, and only the Managed assemblies: no RustDedicated, no bundles, no directories.
    assert sorted(path.name for path in dest.iterdir() if path.is_file()) == [
        "Assembly-CSharp.dll",
        "Facepunch.Network.dll",
        "Rust.Data.dll",
        "Rust.Global.dll",
        "UnityEngine.dll",
        "mscorlib.dll",
    ]
    marker_path = dest / ".takaro" / "references.json"
    assert json.loads(marker_path.read_text())["fingerprint"] == resolve(run, repo)["fingerprint"]

    assert references()[1]["status"] == "up-to-date"

    marker = json.loads(marker_path.read_text())
    marker["fingerprint"] = "9" * 64
    marker_path.write_text(json.dumps(marker))
    code, payload, _ = references()
    assert code == 7, payload
    assert references("--force")[0] == 0


# --------------------------------------------------------------------------- build, deploy


def write_build_stub(repo: Path, fingerprint: str, *, name: str = ARTIFACT) -> None:
    """A stand-in for the real release script: the same contract, none of the containers."""
    script = repo / BUILD_SCRIPT
    script.write_text(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        'version="$1"; out="$2"\n'
        'mkdir -p "$out"\n'
        f'printf \'// takaro-rust-plugin %s\\n[Info("TakaroConnector", "Takaro", "%s")]\\n\' '
        f'"$version" "$version" > "$out/{name}"\n'
        f'cat > "$out/{name}.meta.json" <<JSON\n'
        f'{{"target": "{TARGET}", "fingerprint": "{fingerprint}", "connectorVersion": "$version", '
        f'"provenance": "{name}.provenance.json"}}\n'
        "JSON\n"
        # The second document is the point of the pair: `takaro-maint build` writes its own
        # generic sidecar over the .meta.json name, so the build's pins live beside it.
        f'cat > "$out/{name}.provenance.json" <<JSON\n'
        f'{{"schemaVersion": 1, "target": "{TARGET}", "fingerprint": "{fingerprint}", '
        f'"game": "rust", "platform": "carbon", "revision": "25353106", '
        f'"carbon": {{"tag": "production_build", "sha256": "{"b" * 64}"}}, '
        f'"gameBuild": {{"buildid": 25353106}}, "toolchain": "sdk@sha256:abc"}}\n'
        "JSON\n",
        encoding="utf-8",
    )
    script.chmod(0o755)


def test_build_selects_the_exact_cs_name_and_meta(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(repo, resolved["fingerprint"])
    out = tmp_path / "dist"

    code, payload, err = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(out), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert [row["role"] for row in payload["artifacts"]] == ["plugin"]
    assert payload["artifacts"][0]["file"] == ARTIFACT
    assert (out / ARTIFACT).is_file()
    assert ARTIFACT in (out / "SHA256SUMS").read_text()

    # The generic sidecar is written over the script's .meta.json name, so the build's own
    # pins -- the Carbon asset, the depot build, the toolchain digest -- would be lost if
    # they lived only there. They travel in the .provenance.json beside it instead.
    generic = json.loads((out / f"{ARTIFACT}.meta.json").read_text())
    assert generic["fingerprint"] == resolved["fingerprint"]
    assert "carbon" not in generic
    provenance = json.loads((out / f"{ARTIFACT}.provenance.json").read_text())
    assert provenance["fingerprint"] == resolved["fingerprint"]
    assert provenance["carbon"]["tag"] == "production_build"
    assert provenance["gameBuild"]["buildid"] == 25353106
    assert provenance["toolchain"]
    manifest = json.loads((out / "build-manifest.json").read_text())
    assert manifest["artifacts"][0]["fingerprint"] == resolved["fingerprint"]

    # The legacy class-named file is what Carbon loads, not what a release publishes.
    shutil.rmtree(repo / "games/rust/_data/dist", ignore_errors=True)
    write_build_stub(repo, resolved["fingerprint"], name="TakaroConnector.cs")
    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "d2"), repo=repo
    )
    assert code == 7, payload
    assert "did not produce" in payload["error"]


def _plugin_source(path: Path, version: str = VERSION) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f'// takaro-rust-plugin {version}\n[Info("TakaroConnector", "Takaro", "{version}")]\n')


def _manifest_for(run: Any, repo: Path, directory: Path, artifact: Path) -> Path:
    resolved = resolve(run, repo)
    row = artifact_row("plugin", TARGET, resolved["fingerprint"], artifact)
    write_meta(directory, row, connector=GAME, version=VERSION, revision="deadbeef")
    return write_manifest(
        directory,
        connector=GAME,
        version=VERSION,
        revision="deadbeef",
        dirty=False,
        toolchain=resolved["build"]["toolchain"],
        mode="container",
        artifacts=[row],
    )


def test_deploy_installs_the_plugin_as_takaroconnector_cs_and_removes_stale_versions(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    dest = tmp_path / "rust_dedicated"
    assert install(run, repo, dest)[0] == 0
    stale = dest / "takaro" / f"takaro-rust-plugin-{TARGET}-0.0.5.cs"
    stale.write_text("// an older deploy\n")
    directory = tmp_path / "dist"
    _plugin_source(directory / ARTIFACT)
    manifest = _manifest_for(run, repo, directory, directory / ARTIFACT)

    code, payload, err = run(
        "deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert (dest / "carbon/plugins/TakaroConnector.cs").read_bytes() == (directory / ARTIFACT).read_bytes()
    assert (dest / "takaro" / ARTIFACT).is_file()
    assert not stale.exists()
    ledger = read_ledger(dest)
    assert ledger is not None
    assert ledger.data["artifacts"][0]["path"] == f"takaro/{ARTIFACT}"
    assert run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)[0] == 0

    # A build for a target this directory does not hold is refused, and writes nothing.
    moved = read_target(repo)
    moved["inputs"]["carbon"]["updatedAt"] = "2026-09-20T00:00:00Z"
    write_target(repo, moved)
    before = (dest / "carbon/plugins/TakaroConnector.cs").read_bytes()

    code, payload, _ = run(
        "deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo
    )

    assert code == 7, payload
    assert "holds target" in payload["error"]
    assert (dest / "carbon/plugins/TakaroConnector.cs").read_bytes() == before


# --------------------------------------------------------------------------- verification hooks


def test_verify_hooks_patterns_identity_and_container_shape(tmp_path: Path) -> None:
    # The lines a real boot wrote, copied out of the rig's log.
    loaded = hooks.LOADED_LINE.search("Loaded plugin TakaroConnector v0.0.3 by Takaro [1667ms]")
    assert loaded and loaded.group("version") == "0.0.3" and loaded.group("ms") == "1667"
    assert hooks.READY_LINE.search("Server startup complete")
    assert hooks.PROTOCOL_LINE.search("Protocol: 2632.287.1")
    assert hooks.CARBON_LINE.search("Initialized Carbon.Startup 2.0.257.0")
    assert hooks.COMPILE_FAILED.search("Failed compiling 'TakaroConnector':")
    assert hooks.COMPILE_FAILED.search("TakaroConnector.cs(12,5): error CS0103: something")
    assert not hooks.COMPILE_FAILED.search("Loaded plugin TakaroConnector v0.0.3 by Takaro [1667ms]")
    # Carbon talks about self-updating on every boot, so only the boots it happened on may
    # match. All five lines are copied out of real boots.
    for quiet in (
        "Skipped self-updating process as it's disabled in the config.",
        "Carbon Release is up to date, no self-updating necessary. Running Production build [2.0.259].",
    ):
        assert not hooks.SELF_UPDATE_LINE.search(quiet), quiet
    for noisy in (
        " Carbon Release is out of date and now self-updating - Production [production_build] "
        "on Linux [2.0.257 -> 2.0.259]",
        "Updating Carbon... ",
        " Carbon Release finished self-updating 76 files. You're now running the latest Production build.",
    ):
        assert hooks.SELF_UPDATE_LINE.search(noisy), noisy

    adapter = adapter_for(GAME)
    assert adapter.parse_runtime_identity("Protocol: 2632.287.1") == {
        "gameVersion": "2632.287.1",
        "loader": "carbon",
        "loaderVersion": None,
    }
    assert adapter.parse_runtime_identity("Initialized Carbon.Startup 2.0.259.0") == {
        "gameVersion": None,
        "loader": "carbon",
        "loaderVersion": "2.0.259",
    }
    assert adapter.parse_runtime_identity("nothing to see here") is None

    log = tmp_path / "server.log"
    log.write_text("Initialized Carbon.Startup 2.0.259.0\nProtocol: 2632.287.1\nServer startup complete\n")
    assert hooks.scan_runtime_identity(adapter, log) == {
        "gameVersion": "2632.287.1",
        "loader": "carbon",
        "loaderVersion": "2.0.259",
    }

    # Oxide's VersionNumber parses [Info]'s version as three integers, so the build stamps
    # the numeric head of the connector version and the check compares like with like.
    assert hooks.plugin_version("0.0.5-dev.abc1234") == "0.0.5"
    assert hooks.plugin_version("1.2.3") == "1.2.3"
    assert hooks.plugin_version("2.0") == "2.0.0"
    assert hooks.plugin_version("not-a-version") == "0.0.0"

    from takaro_maint.verify.runner import check_ids

    assert set(hooks.CHECK_IDS) <= set(check_ids(GAME))
    assert "connector-load" not in hooks.CHECK_IDS

    data = tmp_path / "data"
    data.mkdir()
    resolved: dict[str, Any] = {}
    assert adapter.container_mounts(resolved, data) == [
        f"{data}:/rust",
        f"{REPO_ROOT}/games/rust/start.sh:/takaro/start.sh:ro",
    ]
    assert adapter.container_command(resolved, data) == ["/bin/bash", "/takaro/start.sh"]
    environment = adapter.runtime_env(
        {"runtime": {"container": {"env": {"RUST_SERVER_PORT": "28015"}}}},
        {"TAKARO_WS_URL": "ws://host.docker.internal:27128/"},
    )
    assert environment["TAKARO_WS_URL"] == "ws://host.docker.internal:27128/"
    assert environment["HOME"] == "/rust/takaro/home"
    assert environment["RCON_PASSWORD"]
    assert environment["RUST_SERVER_PORT"] == "28015"


def test_the_plugin_source_keeps_its_release_markers() -> None:
    source = (REPO_ROOT / "games/rust/mod/TakaroConnector.cs").read_text(encoding="utf-8")

    info = re.findall(r'\[Info\("TakaroConnector", "Takaro", "([^"]+)"\)\].*x-release-please-version', source)
    assert len(info) == 1, "release-please stamps exactly one [Info] line"
    assert info[0] == (REPO_ROOT / "games/rust/version.txt").read_text(encoding="utf-8").strip()
    # The line the shared identify/reconnect checks and the rig's success pattern share.
    assert "Identified successfully" in source
    assert "Identified and connected" not in source
    # Identify is the first frame out, like every other connector here: a peer that expects
    # the client to speak first must not be left waiting for a greeting that never comes.
    assert re.search(r'LogInfo\("WebSocket connected"\);\s*(?://[^\n]*\n\s*)*SendIdentify\(\);', source)
    # Rust's console echoes neither a command it was handed nor a broadcast, so the
    # connector is what records them -- the console one by verb and argument count only,
    # because the arguments are whatever Takaro was asked to run.
    assert 'LogInfo($"console: {CommandSummary(command)}")' in source
    assert 'LogInfo($"console: {command}")' not in source
    assert 'LogInfo($"broadcast: {message}")' in source
    # listEntities answers with display names, never the dev short name. What those names
    # come out as is `games/rust/tests/names/run.sh`, which compiles the region below and
    # runs it; all this file can say is that the region exists and is what is called.
    assert not re.search(r'\["name"\]\s*=\s*shortName', source)
    assert "EntityNames.EntityDisplayName(shortName)" in source
    region = source[source.index("// takaro:names-begin") : source.index("// takaro:names-end")]
    assert "RustPlugin" not in region and "UnityEngine" not in region, "the region has to compile alone"
    vectors = dict(
        line.split("\t", 1)
        for line in (REPO_ROOT / "games/rust/tests/names/names.tsv").read_text(encoding="utf-8").splitlines()
        if line
    )
    table = dict(re.findall(r'\{\s*"([^"]+)",\s*"([^"]+)"\s*\}', region[: region.index("// Prefab words")]))
    assert table, "the curated table is what the harness covers"
    missing = sorted(code for code in table if code not in vectors)
    assert not missing, f"names.tsv does not cover {missing}"
    assert all(vectors[code] == name for code, name in table.items())


# --------------------------------------------------------------------------- scan


def steam_document() -> dict[str, Any]:
    """The recorded Steam document, freshly parsed so each test owns its own copy."""
    return vdf.parse(STEAM_DOCUMENT.read_text(encoding="utf-8"))[str(APP)]  # type: ignore[no-any-return]


@pytest.fixture
def steam(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Any:
    root = tmp_path / "steam-root"
    log = tmp_path / "steamcmd-argv.jsonl"

    class Rig:
        document = steam_document()

        def serve(self) -> None:
            fake_steamcmd.serve(root, APP, self.document)

    rig = Rig()
    rig.serve()
    for name, value in fake_steamcmd.environment(root, log).items():
        monkeypatch.setenv(name, value)
    return rig


def scan(run: Any, repo: Path, fake_tracker: FakeGitHub, *flags: str) -> tuple[int, Any, str]:
    return run("scan", "--game", GAME, "--repo", TRACKER_REPO, "--api-url", fake_tracker.api_url, *flags, repo=repo)


def support_issues(fake_tracker: FakeGitHub) -> list[dict[str, Any]]:
    return [
        issue
        for issue in fake_tracker.issues
        if (identity.parse_marker(str(issue.get("body") or "")) or {}).get("kind") == "support"
    ]


def test_scan_covers_the_pinned_head_and_files_a_moved_head_as_blocked_upstream(
    run: Any, repo: Path, upstream: Any, steam: Any, monkeypatch: pytest.MonkeyPatch
) -> None:
    # The fixture repo is repinned at the fixture depots; the scan is about the real record.
    current = json.loads((REPO_ROOT / "catalog/rust/targets" / f"{TARGET}.json").read_text())
    write_target(repo, current)
    server = current["inputs"]["server"]
    steam.document = fake_steamcmd.move_head(
        steam.document,
        "public",
        server["buildid"],
        {depot: row["manifest"] for depot, row in server["depots"].items()},
        timeupdated=1790074315,
    )
    steam.serve()
    monkeypatch.setenv("GH_TOKEN", TOKEN)

    with FakeGitHub() as tracker:
        code, payload, err = scan(run, repo, tracker, "--bootstrap")

        assert code == 0, err
        assert payload["mode"] == "read-only"
        assert payload["sources"][f"{GAME}/carbon-api"]["heads"] == {"release": "2.0.259.bfc3cf3d"}
        assert payload["sources"][f"{GAME}/carbon-api"]["history"] == "heads-only"
        assert [entry["action"] for entry in payload["plan"]].count("create-issue") == 0, (
            "the pinned head is covered by the catalog target"
        )
        assert payload["applied"] == []
        assert tracker.writes == 0

    steam.document = fake_steamcmd.move_head(
        steam.document,
        "public",
        25500000,
        {"258552": HEADS["258552"], "258554": HEADS["258554"]},
        timeupdated=1790100000,
    )
    steam.serve()

    with FakeGitHub() as tracker:
        code, payload, err = scan(run, repo, tracker, "--bootstrap", "--publish")

        assert code == 0, err
        created = [entry for entry in payload["plan"] if entry["action"] == "create-issue"]
        assert len(created) == 1, payload["plan"]
        filed = support_issues(tracker)
        assert len(filed) == 1
        body = str(filed[0]["body"])
        assert "provider=steam" in body.splitlines()[0]
        assert f"component={GAME}" in body.splitlines()[0]
        assert "branch=public" in body.splitlines()[0]
        assert "| Build id | 25500000 |" in body
        # Carbon publishes nothing that says which Rust build it targets, so the framework
        # row cannot be anything but `missing` — and that is the blocked state.
        assert re.search(r"^\| carbon \| missing \|", body, re.MULTILINE), body
        assert "<!-- takaro-maint:state=blocked-upstream -->" in body


def test_scan_treats_declared_disabled_known_and_unknown_branches_apart(
    run: Any, repo: Path, upstream: Any, steam: Any, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_target(repo, json.loads((REPO_ROOT / "catalog/rust/targets" / f"{TARGET}.json").read_text()))
    monkeypatch.setenv("GH_TOKEN", TOKEN)
    branches = fake_steamcmd.branches(steam.document)
    branches["beta-test"] = {"buildid": "25500000", "timeupdated": "1790200000"}
    steam.serve()

    with FakeGitHub() as tracker:
        code, payload, err = scan(run, repo, tracker, "--bootstrap")

        assert code == 0, err
        observed = {(observation["kind"], observation["branch"]) for observation in payload["observations"]}
        # `release` and `staging` are declared but disabled: neither observed nor reviewed.
        assert ("game", "release") not in observed
        assert ("game", "staging") not in observed
        assert ("branch-review", "release") not in observed
        assert ("branch-review", "staging") not in observed
        # `aux02`, `debug` and `last-month` are known branches: they produce nothing.
        assert not any(branch in ("aux02", "debug", "last-month") for _, branch in observed)
        # An undeclared branch is a review candidate.
        assert ("branch-review", "beta-test") in observed


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


def test_compat_record_carries_the_steam_and_carbon_pins(run: Any, repo: Path, tmp_path: Path) -> None:
    git(repo, "init", "-q", "-b", "main")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "fixture")
    commit = git(repo, "rev-parse", "HEAD")
    resolved = resolve(run, repo)
    directory = tmp_path / "dist" / TARGET
    _plugin_source(directory / ARTIFACT)
    row = artifact_row("plugin", TARGET, resolved["fingerprint"], directory / ARTIFACT)
    write_meta(directory, row, connector=GAME, version=VERSION, revision=commit)
    write_manifest(
        directory,
        connector=GAME,
        version=VERSION,
        revision=commit,
        dirty=False,
        toolchain=resolved["build"]["toolchain"],
        mode="container",
        artifacts=[row],
    )
    out = tmp_path / "assembled"

    code, payload, err = run(
        "release", "assemble",
        "--connector", GAME,
        "--version", VERSION,
        "--channel", "pr",
        "--tag", f"pr-1-{GAME}",
        "--dist", str(tmp_path / "dist"),
        "--out", str(out),
        "--repo", "o/r",
        repo=repo,
    )  # fmt: skip

    assert code == 0, f"{err}\n{payload}"
    record = json.loads((out / f"takaro-{GAME}-{VERSION}.compat.json").read_text())
    entry = record["targets"][TARGET]
    assert entry["verification"]["required"] == "contract"
    assert entry["verification"]["executed"] is None
    server_url = entry["inputs"]["server"]["url"]
    assert server_url.startswith("steam://app/258550/branch/public/build/25353106/")
    assert "258554/manifest/" in server_url
    assert entry["inputs"]["carbon"]["url"] == resolved["resolvedUrls"]["carbon"]
    assert (out / ARTIFACT).is_file()
    assert (out / "TakaroConnector.cs").read_bytes() == (out / ARTIFACT).read_bytes()
    assert ARTIFACT in (out / "SHA256SUMS").read_text()


# --------------------------------------------------------------------------- the rig and CI

DS_ROOT = REPO_ROOT / "dev-servers"
REGISTRY_FIXTURE = Path(__file__).parent / "fixtures" / "games" / "7d2d" / "dev-servers-registry.expected"


def bash(script: str) -> str:
    completed = subprocess.run(["bash", "-c", script], cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


def test_dev_servers_rust_rig_is_target_driven() -> None:
    for script in [
        DS_ROOT / "lib/games/rust.sh",
        REPO_ROOT / "games/rust/start.sh",
        *sorted((REPO_ROOT / "games/rust/scripts").glob("*.sh")),
    ]:
        completed = subprocess.run(["bash", "-n", str(script)], capture_output=True, text=True, check=False)
        assert completed.returncode == 0, f"{script.name}: {completed.stderr}"

    assert bash(". dev-servers/lib/common.sh; ds_target_prefix rust").strip() == "RUST"
    assert bash(". dev-servers/lib/common.sh; ds_target_dest rust").strip().endswith("/rust/rust_dedicated")
    assert bash(". dev-servers/lib/common.sh; ds_success_pattern_rust").strip() == "Identified successfully"

    registry = bash(". dev-servers/lib/common.sh; ds_registry")
    expected = [line for line in REGISTRY_FIXTURE.read_text().splitlines() if line.startswith("rust|")]
    assert [line for line in registry.splitlines() if line.startswith("rust|")] == expected

    compose = (DS_ROOT / "compose" / "rust.yml").read_text()
    assert "${RUST_IMAGE" in compose
    assert "/takaro/start.sh" in compose
    assert "build:" not in compose

    # Nothing tracked reaches for the branch head or the moving Carbon alias. The
    # prose that explains the pin lives in DEVELOPMENT.md, which is excluded.
    audited = subprocess.run(
        [
            "grep", "-rnE", "app_update|production_build|steamcmd_linux|latest/download",
            "games/rust", "dev-servers/compose/rust.yml", "dev-servers/lib/games/rust.sh",
            ".github/workflows/rust.yml",
            "--exclude-dir=_data", "--exclude=DEVELOPMENT.md", "--exclude=CHANGELOG.md",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )  # fmt: skip
    assert audited.returncode == 1, f"a tracked Rust entry point still floats:\n{audited.stdout}"
    assert not (REPO_ROOT / "games/rust/Dockerfile").exists(), "no image is built for Rust any more"


def test_rust_workflow_delegates_to_connector_release() -> None:
    workflow = (REPO_ROOT / ".github/workflows/rust.yml").read_text()

    assert "uses: ./.github/workflows/connector-release.yml" in workflow
    assert "connector: rust" in workflow
    assert "runtime: false" in workflow
    assert "compile-check.sh" in workflow
    assert "setup-environment.sh" in workflow
    assert "rust-refs-${{ steps.target.outputs.fp16 }}" in workflow
    assert "app_update" not in workflow
    assert "docker build" not in workflow


def test_a_bare_verify_run_selects_only_the_checks_this_target_proves() -> None:
    """The generic ladder carries four checks Rust cannot pass; nobody has to remember them.

    Without this, `takaro-maint verify --game rust` -- exactly as DEVELOPMENT.md documents it,
    with no `--checks` -- would select `connector-load` (a line only the Minecraft connector
    writes), `catalog-items`/`catalog-entities` (Minecraft spot values) and the base
    `shutdown` (an exit code Rust's Unity teardown does not give), and fail on all four. The
    narrowing itself is the runner's (`test_verify_selection.py`); what Rust owes is the
    declaration, and that it agrees with the target record.
    """
    from takaro_maint.verify.runner import check_ids, game_hooks

    record = json.loads((REPO_ROOT / f"catalog/{GAME}/targets/{TARGET}.json").read_text(encoding="utf-8"))

    declared = game_hooks(GAME)
    selection = [check for check in check_ids(GAME) if check not in declared.unsupported_checks]
    # Same rows as the record names, in the ladder's own order rather than the record's.
    assert set(selection) == {"build", *record["verification"]["separate"]}
    assert selection == [check for check in check_ids(GAME) if check in selection]

    for unreachable in ("connector-load", "catalog-items", "catalog-entities", "shutdown"):
        assert unreachable in check_ids(GAME), unreachable
        assert unreachable not in selection, unreachable
        assert declared.unsupported_checks[unreachable], unreachable
    # Everything the hooks add, and the base checks Rust does pass, are in.
    assert set(hooks.CHECK_IDS) <= set(selection)
    assert {"startup", "identify", "heartbeat", "players", "console"} <= set(selection)


def test_a_failed_carbon_repair_leaves_the_install_and_its_ledger_intact(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    """The one path that re-installs a tree the ledger still describes has to be safe too.

    A drifted Carbon half is the only way `install` asks the Steam layer to redo an install
    whose fingerprint already matches, so it is the only path on which the ledger of a good
    install is at risk. AC4 asks for a failed preparation to leave the install as it was --
    which includes leaving it able to say what it is.
    """
    dest = tmp_path / "rust_dedicated"
    assert install(run, repo, dest)[0] == 0
    ledger_file = dest / ".takaro" / "installed-target.json"
    ledger_before = ledger_file.read_bytes()

    witness = dest / "carbon" / "managed" / "Carbon.dll"
    witness.write_bytes(witness.read_bytes() + b"carbon self-updated under the pin")
    before = tree_hash(dest)
    # The record is untouched, so the fingerprint still matches and the repair path is taken;
    # what fails is the fetch. (The blob cache is content-addressed and would answer happily.)
    upstream.status_overrides[ASSET_PATH] = 500
    shutil.rmtree(Path(os.environ["TAKARO_MAINT_CACHE"]) / "blobs", ignore_errors=True)

    code, payload, _ = install(run, repo, dest)

    assert code != 0, payload
    assert tree_hash(dest) == before
    assert ledger_file.read_bytes() == ledger_before
    assert list(dest.parent.glob(f"{dest.name}.stale-ledger")) == []
    assert staging_siblings(dest) == []
    # And it is still a tree `ledger check` can answer for: it fails on the drifted Carbon
    # DLL, not on a missing ledger.
    code, payload, _ = run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)
    assert code == 7, payload
    assert "Carbon.dll" in json.dumps(payload)

    # Once upstream answers again the repair completes and takes the set-aside ledger with it.
    upstream.status_overrides.pop(ASSET_PATH)
    assert install(run, repo, dest)[0] == 0
    assert list(dest.parent.glob(f"{dest.name}.stale-ledger")) == []
    assert run("ledger", "check", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)[0] == 0


# ------------------------------------------------------------------- archive containment


def _carbon_like_tar(path: Path, entries: list[tarfile.TarInfo], payloads: dict[str, bytes]) -> None:
    """A gzip tarball shaped like the Carbon asset, carrying the given entries verbatim."""
    with tarfile.open(path, "w:gz") as tar:
        for info in entries:
            if info.isreg():
                data = payloads.get(info.name, b"")
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
            else:
                tar.addfile(info)


def _link(name: str, linkname: str, *, hard: bool) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type = tarfile.LNKTYPE if hard else tarfile.SYMTYPE
    info.linkname = linkname
    return info


def _regular(name: str, data: bytes = b"x") -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type = tarfile.REGTYPE
    info.size = len(data)
    return info


def _members(tmp_path: Path, entries: list[tarfile.TarInfo], root: Path) -> list[tarfile.TarInfo]:
    from takaro_maint.games.rust import _safe_members

    archive = tmp_path / "carbon-under-test.tar.gz"
    _carbon_like_tar(archive, entries, {})
    with tarfile.open(archive, "r:gz") as tar:
        return _safe_members(tar, root)


def test_a_symlink_resolving_beside_the_install_is_refused(tmp_path: Path) -> None:
    """`<root>-evil` is not inside `<root>`, however much of a string prefix it is.

    The containment test this replaces compared resolved paths as strings, so a link
    pointing at a sibling directory whose name merely starts with the install's name was
    accepted -- and the extraction then wrote the operator's own files through it.
    """
    root = tmp_path / "install"
    root.mkdir()
    (tmp_path / "install-evil").mkdir()
    (tmp_path / "install-evil" / "pwned.txt").write_text("host bytes", encoding="utf-8")

    with pytest.raises(Exception) as caught:
        _members(tmp_path, [_link("carbon/link", "../../install-evil/pwned.txt", hard=False)], root)

    assert "outside the install directory" in str(caught.value)
    assert (tmp_path / "install-evil" / "pwned.txt").read_text(encoding="utf-8") == "host bytes"
    assert list(root.rglob("*")) == []


def test_a_hard_link_is_resolved_against_the_archive_root_not_the_entry(tmp_path: Path) -> None:
    """A hard link's target is archive-relative, so it is resolved against the archive root."""
    root = tmp_path / "install"
    root.mkdir()

    with pytest.raises(Exception) as caught:
        _members(tmp_path, [_link("carbon/managed/link", "../outside", hard=True)], root)

    assert "outside the install directory" in str(caught.value)


def test_a_link_inside_the_install_is_accepted(tmp_path: Path) -> None:
    root = tmp_path / "install"
    root.mkdir()

    entries = [
        _regular("carbon/managed/Carbon.dll"),
        _link("carbon/managed/alias.dll", "Carbon.dll", hard=False),
        _link("carbon/hard.dll", "carbon/managed/Carbon.dll", hard=True),
    ]

    assert [member.name for member in _members(tmp_path, entries, root)] == [
        "carbon/managed/Carbon.dll",
        "carbon/managed/alias.dll",
        "carbon/hard.dll",
    ]


def test_an_escaping_link_in_the_carbon_asset_fails_the_install(
    run: Any, repo: Path, dd_log: Path, upstream: Any, tmp_path: Path
) -> None:
    """End to end: a hostile asset exits 5 and writes nothing outside the install."""
    outside = tmp_path / "rust_dedicated-evil"
    outside.mkdir()
    (outside / "pwned.txt").write_text("host bytes", encoding="utf-8")

    hostile = tmp_path / "hostile-carbon.tar.gz"
    _carbon_like_tar(
        hostile,
        [_regular("carbon/managed/Carbon.dll"), _link("carbon/escape", "../../rust_dedicated-evil", hard=False)],
        {},
    )
    upstream.add_file(ASSET_PATH, hostile)
    record = read_target(repo)
    record["inputs"]["carbon"]["sha256"] = fake.sha256_of(hostile)
    record["inputs"]["carbon"]["size"] = hostile.stat().st_size
    write_target(repo, record)

    dest = tmp_path / "rust_dedicated"
    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert "outside the install directory" in json.dumps(payload)
    assert sorted(p.name for p in outside.iterdir()) == ["pwned.txt"]
    assert (outside / "pwned.txt").read_text(encoding="utf-8") == "host bytes"
    assert not dest.exists()
    assert staging_siblings(dest) == []


# ------------------------------------------------------- the entity catalogue, directly


class _EntityFake:
    """Only what `check_catalog` reaches for: one canned answer."""

    def __init__(self, entries: Any) -> None:
        self.entries = entries

    async def request(self, action: str, params: Any, timeout: float | None = None) -> Any:
        del action, params, timeout
        return self.entries


def _entities(entries: Any) -> Any:
    import asyncio

    from takaro_maint.verify import checks

    return asyncio.run(
        checks.check_catalog(
            _EntityFake(entries), "listEntities", "entities", hooks.ENTITY_SPOT, hooks._prefab_name_problems
        )
    )


def test_the_entity_check_passes_on_curated_display_names() -> None:
    result = _entities(
        [
            {"code": "scientistnpc_heavy", "name": "Heavy Scientist"},
            {"code": "scientistnpc_full_lr300", "name": "Scientist (LR-300)"},
            {"code": "wolf2", "name": "Wolf"},
        ]
    )

    assert result.status == "pass", result.detail["problems"]
    assert result.detail["spotCheck"] == {
        "code": "scientistnpc_heavy",
        "expected": "Heavy Scientist",
        "actual": "Heavy Scientist",
    }


def test_the_curated_table_covers_the_recorded_server_corpus() -> None:
    table = hooks._curated_entity_names()
    assert len(table) >= 60
    codes = [
        code
        for code in (REPO_ROOT / "games/rust/tests/names/entity-codes.txt").read_text(encoding="utf-8").splitlines()
        if code
    ]

    result = _entities([{"code": code, "name": table[code]} for code in codes])

    assert len(codes) == 67
    assert result.status == "pass", result.detail["problems"]


def test_the_entity_check_refuses_a_prefab_outside_the_curated_table() -> None:
    result = _entities(
        [
            {"code": "scientistnpc_heavy", "name": "Heavy Scientist"},
            {"code": "future_event_npc", "name": "Future Event NPC"},
        ]
    )

    assert result.status == "fail"
    assert any(
        "1 prefabs have no curated name" in problem and "future_event_npc" in problem
        for problem in result.detail["problems"]
    )


def test_the_entity_check_fails_on_a_formatted_prefab_code() -> None:
    """`Scientistnpc Heavy` is a formatted prefab code, and the check refuses it."""
    result = _entities(
        [
            {"code": "scientistnpc_heavy", "name": "Heavy Scientist"},
            {"code": "scientistnpc_cargo_turret_lr300", "name": "Scientistnpc Cargo Turret Lr300"},
            {"code": "wolf2", "name": "Wolf2"},
        ]
    )

    assert result.status == "fail"
    problems = " ".join(result.detail["problems"])
    assert "2 of 3 names are formatted prefab codes" in problems
    assert "scientistnpc_cargo_turret_lr300 -> Scientistnpc Cargo Turret Lr300" in problems


def test_the_entity_spot_check_is_a_name_the_old_derivation_could_not_produce() -> None:
    """`bear` -> `Bear` passed whether or not anything was curated; this one cannot."""
    assert hooks.ENTITY_SPOT == ("scientistnpc_heavy", "Heavy Scientist")

    result = _entities([{"code": "scientistnpc_heavy", "name": "Scientistnpc Heavy"}])

    assert result.status == "fail"
    assert any("Heavy Scientist" in problem for problem in result.detail["problems"])
