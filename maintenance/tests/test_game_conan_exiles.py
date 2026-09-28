"""The Conan Exiles target end to end: catalog, discovery, pin, install, build, deploy.

Everything here runs the real command against the DepotDownloader and steamcmd stand-ins
and a stub build script, so the assertions are about what a maintainer, the rig and CI
observe. Two depots make this game different from 7D2D: the game's own Linux content
(443032) and Valve's Steamworks redistributable (1006), which is pinned because the
proven install carries it and watched by nobody because it belongs to another app.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import subprocess
import zipfile
from pathlib import Path
from typing import Any

import pytest

import fake_depotdownloader as fake
import fake_steamcmd
from fake_verify import CannedSocket, FakeContainer, FakeRun
from takaro_maint.games import adapter_for
from takaro_maint.games.conan_exiles import verify as hooks
from takaro_maint.publish.manifest import artifact_row, write_manifest, write_meta
from takaro_maint.steam import steamcmd

GAME = "conan-exiles"


def test_every_conan_verification_body_has_pass_and_failure_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    target = type("Target", (), {"id": TARGET, "fp16": "0123456789abcdef", "record": {"revision": "1"}})()
    run = FakeRun(tmp_path, target=target)
    bridge_log = run.out / "bridge.log"
    bridge = FakeContainer(bridge_log)
    run.bridge = bridge
    fake = CannedSocket(
        {
            "testReachability": {"connectable": True},
            "getPlayers": [],
            "executeConsoleCommand": {"success": True, "rawResult": "No players"},
            "sendMessage": {"success": False, "error": "helper absent"},
        },
        identify_count=1,
    )
    stamp = f"Takaro target: {TARGET} (0123456789abcdef) revision 1 connector 1.0.0"
    monkeypatch.setattr(hooks, "start_bridge", lambda *args, **kwargs: bridge)
    monkeypatch.setattr(hooks, "_retain_server_logs", lambda run: None)
    monkeypatch.setattr(hooks, "_wait_for_rcon_command", lambda *args, **kwargs: ["help", "listplayers"])
    monkeypatch.setattr(hooks.checks, "wait_for_line", lambda *args, **kwargs: (1, "Identified with Takaro"))
    monkeypatch.setattr(hooks.checks, "find_line", lambda path, pattern: (1, stamp))
    monkeypatch.setattr(hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=1))
    monkeypatch.setattr(hooks.checks_lifecycle, "wait_for_count", lambda *args, **kwargs: 2)
    monkeypatch.setattr(
        hooks.subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(args[0], 0, "", ""),
    )

    assert asyncio.run(hooks._check_identify(run, fake, bridge, bridge.alive)).status == "pass"
    assert asyncio.run(hooks._check_reachability(run, fake, bridge.alive)).status == "pass"
    assert asyncio.run(hooks._check_players(run, fake, bridge.alive)).status == "pass"
    assert asyncio.run(hooks._check_console(run, fake)).status == "pass"
    assert asyncio.run(hooks._check_reconnect(run, fake, bridge.alive)).status == "pass"
    assert asyncio.run(hooks._check_stop(run, [])).status == "pass"
    asyncio.run(hooks.after_protocol(run, fake, bridge.alive))
    asyncio.run(hooks.after_shutdown(run, fake, run.ws_url, []))

    failed_run = FakeRun(tmp_path / "failed", target=target, wanted=set())
    failed = CannedSocket(
        {
            "testReachability": None,
            "getPlayers": {"bad": True},
            "executeConsoleCommand": None,
            "sendMessage": {"success": True},
        },
        reconnects=False,
    )
    monkeypatch.setattr(hooks, "_wait_for_rcon_command", lambda *args, **kwargs: [])
    monkeypatch.setattr(hooks.checks, "wait_for_line", lambda *args, **kwargs: None)
    monkeypatch.setattr(hooks.checks, "find_line", lambda *args, **kwargs: None)
    monkeypatch.setattr(
        hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=None)
    )
    monkeypatch.setattr(hooks.checks_lifecycle, "wait_for_count", lambda *args, **kwargs: 0)

    assert asyncio.run(hooks._check_identify(failed_run, failed, bridge, bridge.alive)).status == "fail"
    assert asyncio.run(hooks._check_reachability(failed_run, failed, bridge.alive)).status == "fail"
    assert asyncio.run(hooks._check_players(failed_run, failed, bridge.alive)).status == "fail"
    assert asyncio.run(hooks._check_console(failed_run, failed)).status == "fail"
    failed_run.bridge = bridge
    assert asyncio.run(hooks._check_reconnect(failed_run, failed, bridge.alive)).status == "fail"
    failed_run.bridge = None
    assert asyncio.run(hooks._check_stop(failed_run, [{"path": "missing"}])).status == "fail"
    asyncio.run(hooks.after_protocol(failed_run, failed, bridge.alive))
    asyncio.run(hooks.after_shutdown(failed_run, failed, failed_run.ws_url, []))
    assert set(check for check, _ in failed_run.skips) == set(hooks.CHECK_IDS)


TARGET = "linux-25488622"
APP = 443030
CONTENT_DEPOT = "443032"
REDIST_DEPOT = "1006"
PINNED = {CONTENT_DEPOT: "2572292872952587850", REDIST_DEPOT: "4559160656493359681"}
MOVED = {CONTENT_DEPOT: "2600000000000000001", REDIST_DEPOT: "4600000000000000001"}
# What the shipped target (the real repository, not the fixture depot trees below) actually
# pins today -- used only by the tests that run against the real repo (``repo=None``).
SHIPPED = {CONTENT_DEPOT: "2389917983000609164", REDIST_DEPOT: PINNED[REDIST_DEPOT]}
VERSION = "1.0.2-dev.abc1234"
ZIP_NAME = f"takaro-conan-exiles-bridge-{TARGET}-{VERSION}.zip"
BRIDGE_FOLDER = "TakaroConanExiles"
INSTALL_DIR = "TakaroBridge"
BUILD_SCRIPT = "games/conan-exiles/scripts/build-release.sh"
SHIPPING = "ConanSandbox/Binaries/Linux/ConanSandboxServer-Linux-Shipping"

DEPOTS = Path(__file__).parent / "fixtures" / "games" / "conan-exiles" / "depots"
TARGET_PATH = Path("catalog") / GAME / "targets" / f"{TARGET}.json"


# -- the repository copy these tests drive -----------------------------------------------


def read_target(root: Path) -> dict[str, Any]:
    return json.loads((root / TARGET_PATH).read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def write_target(root: Path, record: dict[str, Any]) -> None:
    (root / TARGET_PATH).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def _tree(manifests: dict[str, str], depot: str) -> Path:
    return DEPOTS / depot / manifests[depot] / "tree"


def repin_conan(root: Path, manifests: dict[str, str] | None = None) -> dict[str, Any]:
    """Point the copied Conan target at the fixture depots and record their real hashes.

    A declared file may live in either depot -- ``linux64/steamclient.so`` comes from the
    redistributable -- so each one is looked up where it actually is rather than assumed
    to be in the content depot.
    """
    manifests = manifests or PINNED
    record = read_target(root)
    server = record["inputs"]["server"]
    server["depots"] = {
        depot: {
            "manifest": manifests[depot],
            "size": sum(f.stat().st_size for f in _tree(manifests, depot).rglob("*") if f.is_file()),
            "files": len([f for f in _tree(manifests, depot).rglob("*") if f.is_file()]),
        }
        for depot in sorted(manifests)
    }
    files: dict[str, Any] = {}
    for name in sorted(server["files"]):
        for depot in sorted(manifests):
            candidate = _tree(manifests, depot) / name
            if candidate.is_file():
                files[name] = {"sha256": fake.sha256_of(candidate), "size": candidate.stat().st_size}
                break
        else:  # pragma: no cover - a fixture that lost a declared file
            raise AssertionError(f"no fixture depot serves the declared file {name}")
    server["files"] = files
    write_target(root, record)
    return record


def write_conan_build_stub(repo: Path, fingerprint: str, *, name: str = ZIP_NAME, target: str = TARGET) -> None:
    """A stand-in for the real release script: the same contract, none of the containers."""
    script = repo / BUILD_SCRIPT
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        'version="$1"; out="$2"\n'
        f'stage="$out/stage"; pkg="$stage/{BRIDGE_FOLDER}"\n'
        'mkdir -p "$out" "$pkg/dist/mod"\n'
        "printf 'bridge\\n' > \"$pkg/dist/index.js\"\n"
        "printf 'helper\\n' > \"$pkg/dist/mod/pollerCli.js\"\n"
        'printf \'{"name":"conan-exiles-takaro-bridge"}\\n\' > "$pkg/package.json"\n'
        f'cat > "$pkg/takaro-target.json" <<STAMP\n'
        f'{{"target": "{target}", "fingerprint": "{fingerprint}", "game": "conan-exiles", '
        f'"platform": "linux", "revision": "25488622", "connectorVersion": "$version", '
        f'"sourceRevision": "deadbeef"}}\n'
        "STAMP\n"
        '( cd "$stage" && python3 -c '
        "\"import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[2])\" "
        f'"$out/{name[:-4]}" {BRIDGE_FOLDER} )\n'
        f'cat > "$out/{name}.meta.json" <<JSON\n'
        f'{{"target": "{target}", "fingerprint": "{fingerprint}", "connectorVersion": "$version", '
        f'"sourceRevision": "deadbeef", "game": "conan-exiles", "platform": "linux", '
        f'"revision": "25488622"}}\n'
        "JSON\n",
        encoding="utf-8",
    )
    script.chmod(0o755)


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    """The shared fixture repository, with the Conan target pinned at the fixture depots."""
    root = fake.make_repo(tmp_path)
    script = root / BUILD_SCRIPT
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)
    repin_conan(root)
    return root


@pytest.fixture
def dd_log(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Point the real steam package at the DepotDownloader stand-in and the Conan depots."""
    log = tmp_path / "depotdownloader-argv.jsonl"
    for key, value in fake.environment(tmp_path, log, FAKE_DD_ROOT=str(DEPOTS)).items():
        monkeypatch.setenv(key, value)
    return log


def resolve(run: Any, repo: Path) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", TARGET, repo=repo)
    assert code == 0, err
    return dict(payload)


def argv_rows(log: Path) -> list[list[str]]:
    return fake.argv_log(log)


# -- catalog -----------------------------------------------------------------------------


def test_catalog_validate_accepts_the_conan_target(run: Any) -> None:
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


def test_catalog_validate_refuses_a_changed_conan_lockfile(run: Any, catalog_copy: Path) -> None:
    lockfile = catalog_copy / "games/conan-exiles/bridge/package-lock.json"
    lockfile.write_bytes(lockfile.read_bytes() + b"\n")

    code, payload, _ = run("catalog", "validate", repo=catalog_copy)

    assert code == 2
    failure = next(check for check in payload["failures"] if check["id"] == "lockfile-pinned")
    assert "expected ea07d7c7" in failure["detail"]
    assert "actual " in failure["detail"]


def test_targets_resolve_env_for_conan_exiles(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "CONAN_EXILES")

    assert code == 0, payload
    env = payload["env"]
    assert env["CONAN_EXILES_STEAM_APP"] == "443030"
    assert env["CONAN_EXILES_STEAM_BRANCH"] == "public"
    assert env["CONAN_EXILES_STEAM_BUILDID"] == "25488622"
    # Sorted by depot id, so the string is the same whatever order the record lists them in.
    assert env["CONAN_EXILES_STEAM_DEPOTS"] == (
        f"{REDIST_DEPOT}:{SHIPPED[REDIST_DEPOT]};{CONTENT_DEPOT}:{SHIPPED[CONTENT_DEPOT]}"
    )
    assert env["CONAN_EXILES_ARTIFACT"] == f"takaro-conan-exiles-bridge-{TARGET}-{{version}}.zip"
    assert env["CONAN_EXILES_BRIDGE_DIR"] == f"{INSTALL_DIR}/{BRIDGE_FOLDER}"
    # One pinned image is both the server runtime and the build toolchain.
    assert env["CONAN_EXILES_IMAGE"] == env["CONAN_EXILES_TOOLCHAIN"]
    assert env["CONAN_EXILES_IMAGE"].startswith("node:22.23.2-bookworm-slim@sha256:")
    assert env["CONAN_EXILES_DEP_WS_URL"].endswith("ws-8.21.0.tgz")
    assert len(env["CONAN_EXILES_DEP_WS_SHA256"]) == 64
    assert env["CONAN_EXILES_LOCKFILE_PATH"].endswith("games/conan-exiles/bridge/package-lock.json")
    assert env["CONAN_EXILES_LOCKFILE_SHA256"] == "ea07d7c7d65d57765279815990fd77ad74a0bef8c1cea326f05bd103f727c1b8"
    declared = payload["inputs"]["server"]["files"]
    assert env["CONAN_EXILES_LAUNCHER_SHA256"] == declared["ConanSandboxServer.sh"]["sha256"]
    assert env["CONAN_EXILES_SERVER_BINARY_SHA256"] == declared[SHIPPING]["sha256"]
    assert not any(key.endswith("_JAVA") for key in env)

    url = payload["resolvedUrls"]["server"]
    assert url.startswith("steam://app/443030/branch/public/build/25488622/depot/")
    assert f"{REDIST_DEPOT}/manifest/{SHIPPED[REDIST_DEPOT]}" in url
    assert f"{CONTENT_DEPOT}/manifest/{SHIPPED[CONTENT_DEPOT]}" in url


# -- discovery ---------------------------------------------------------------------------


def rev_of(buildid: int, manifests: dict[str, str], branch: str = "public") -> str:
    return f"{buildid}.{steamcmd.manifest_digest(manifests)}+{branch}"


@pytest.fixture
def steam(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Any:
    """The fake steamcmd serving a mutable copy of the recorded 443030 document."""

    class Rig:
        def __init__(self) -> None:
            self.root = tmp_path / "steam-root"
            self.log = tmp_path / "steamcmd-argv.jsonl"
            self.document = fake_steamcmd.recorded(APP)
            self.root.mkdir(parents=True, exist_ok=True)
            self.serve()

        def serve(self) -> None:
            fake_steamcmd.serve(self.root, APP, self.document)
            for name, value in fake_steamcmd.environment(self.root, self.log).items():
                monkeypatch.setenv(name, value)

    rig = Rig()
    return rig


@pytest.fixture
def tracker(steam: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> Any:
    """The catalog as it ships, a tracker stand-in, and the clock pinned."""
    import test_scan_support as scan_support
    from fake_github import FakeGitHub

    scan_support.frozen_clock(monkeypatch)
    monkeypatch.setenv("GH_TOKEN", scan_support.TOKEN)

    class Rig(scan_support.Rig):
        def scan(self, run: Any, *flags: str) -> tuple[int, Any, str]:
            return run(
                "scan",
                "--game",
                GAME,
                "--source",
                "steam",
                "--repo",
                scan_support.REPO,
                "--api-url",
                self.fake.api_url,
                *flags,
                repo=self.root,
            )

        def titles(self) -> list[str]:
            return [str(issue["title"]) for issue in self.support_issues()]

    with FakeGitHub() as github:
        yield Rig(root=catalog_copy, upstream=None, fake=github)  # type: ignore[arg-type]


def test_scan_observes_the_public_head_and_the_target_covers_it(run: Any, tracker: Any, steam: Any) -> None:
    """The shipped target pins exactly this app, branch, build and watched manifest."""
    key = f"{GAME}/steam"
    # The recorded app_info fixture is a static snapshot older than the shipped target; move the
    # fake head to what the target actually pins before asserting it is covered.
    shipped = json.loads((REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json").read_text(encoding="utf-8"))
    server = shipped["inputs"]["server"]
    steam.document = fake_steamcmd.move_head(
        steam.document,
        "public",
        server["buildid"],
        {CONTENT_DEPOT: server["depots"][CONTENT_DEPOT]["manifest"]},
        timeupdated=1790074315,
    )
    steam.serve()

    code, payload, err = tracker.scan(run, "--bootstrap")

    assert code == 0, err
    source = payload["sources"][key]
    assert source["status"] == "ok"
    assert source["history"] == "heads-only"
    # Only 443032 is watched, so only it is in the head identity: the redistributable is
    # pinned in the target and belongs to app 1007.
    assert source["heads"] == {
        "public": rev_of(int(server["buildid"]), {CONTENT_DEPOT: server["depots"][CONTENT_DEPOT]["manifest"]})
    }
    assert payload["observations"] == []
    assert [entry["action"] for entry in payload["plan"] if entry["action"] == "create-issue"] == []
    assert tracker.support_issues() == []
    assert tracker.fake.writes == 0


def test_a_moved_public_head_is_filed_and_the_legacy_branch_is_not(run: Any, tracker: Any, steam: Any) -> None:
    fake_steamcmd.move_head(
        steam.document, "public", 25400000, {CONTENT_DEPOT: MOVED[CONTENT_DEPOT]}, timeupdated=1789900000
    )
    steam.serve()

    code, payload, err = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 0, err
    expected = rev_of(25400000, {CONTENT_DEPOT: MOVED[CONTENT_DEPOT]})
    assert [observation["rev"] for observation in payload["observations"]] == [expected]
    assert [observation["component"] for observation in payload["observations"]] == [GAME]
    assert tracker.titles() == ["Conan Exiles public: build 25400000 needs a target"]
    body = str(tracker.support_issues()[0]["body"])
    assert f"| Depot {CONTENT_DEPOT} manifest | `{MOVED[CONTENT_DEPOT]}`" in body
    assert "| App | `443030` (Conan Exiles Dedicated Server) |" in body
    # The UE4 server is declared and disabled: it is never observed and never filed.
    assert all("conan-exiles-legacy" not in title for title in tracker.titles())
    assert "conan-exiles-legacy" not in payload["sources"][f"{GAME}/steam"]["heads"]


def test_steam_branches_classifies_the_legacy_branch_as_declared(run: Any, steam: Any, repo: Path) -> None:
    code, payload, err = run("steam", "branches", "--game", GAME, repo=repo)

    assert code == 0, err
    rows = {row["label"]: row for row in payload["branches"]}
    assert rows["public"]["classification"] == "watched"
    assert rows["public"]["buildid"] == 25356024
    assert rows["conan-exiles-legacy"]["classification"] == "declared"
    assert rows["conan-exiles-legacy"]["buildid"] == 24269196
    # Only the watched depot is listed: `--depot` defaults to the watch block.
    assert [depot["id"] for depot in payload["depots"]] == [CONTENT_DEPOT]


# -- steam pin ---------------------------------------------------------------------------


def test_steam_pin_reads_both_depots_and_flags_the_moved_manifests(run: Any, repo: Path, dd_log: Path) -> None:
    code, payload, err = run("steam", "pin", "--game", GAME, "--target", TARGET, repo=repo)

    assert code == 0, err
    assert payload["changed"] == [REDIST_DEPOT, CONTENT_DEPOT]
    assert payload["depots"][CONTENT_DEPOT]["manifest"] == MOVED[CONTENT_DEPOT]
    assert payload["depots"][REDIST_DEPOT]["manifest"] == MOVED[REDIST_DEPOT]
    rows = argv_rows(dd_log)
    assert len(rows) == 2
    assert all("-manifest-only" in row for row in rows)
    assert {row[row.index("-depot") + 1] for row in rows} == {CONTENT_DEPOT, REDIST_DEPOT}


def test_steam_pin_refuses_to_write_hashes_it_has_not_recorded(run: Any, repo: Path, dd_log: Path) -> None:
    before = read_target(repo)

    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, "--write", repo=repo)

    assert code == 7, payload
    assert "--record-files" in payload["error"]
    # Every declared file moved with the manifests, so every one of them is named.
    assert set(payload["files"]) == set(before["inputs"]["server"]["files"])
    assert read_target(repo) == before


def test_steam_pin_writes_only_the_input_it_re_pinned(run: Any, repo: Path, dd_log: Path) -> None:
    before = read_target(repo)
    record_args: list[str] = []
    for path in before["inputs"]["server"]["files"]:
        record_args += ["--record-files", path]

    code, payload, err = run(
        "steam", "pin", "--game", GAME, "--target", TARGET, "--buildid", "25400000", "--write", *record_args, repo=repo
    )

    assert code == 0, err
    after = read_target(repo)
    server = after["inputs"]["server"]
    assert server["buildid"] == 25400000
    assert server["depots"][CONTENT_DEPOT]["manifest"] == MOVED[CONTENT_DEPOT]
    assert server["depots"][REDIST_DEPOT]["manifest"] == MOVED[REDIST_DEPOT]
    assert server["files"][SHIPPING]["sha256"] != before["inputs"]["server"]["files"][SHIPPING]["sha256"]
    assert {key: value for key, value in after.items() if key != "inputs"} == {
        key: value for key, value in before.items() if key != "inputs"
    }


def test_steam_pin_reports_an_unavailable_manifest_as_upstream(
    run: Any, repo: Path, dd_log: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", MOVED[CONTENT_DEPOT])

    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, repo=repo)

    assert code == 4, payload
    assert "not falling back to branch head" in payload["error"]


# -- install -----------------------------------------------------------------------------


def install(run: Any, repo: Path, dest: Path, *extra: str) -> tuple[int, Any, str]:
    return run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), *extra, repo=repo)


def tree_hash(root: Path) -> str:
    from takaro_maint.commands.install import tree_hash as _tree_hash

    return _tree_hash(root)


def test_install_places_the_pinned_build_and_the_redist(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"

    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "installed"
    assert (dest / "ConanSandboxServer.sh").is_file()
    assert os.access(dest / "ConanSandboxServer.sh", os.X_OK), "the depot ships no executable bit"
    assert (dest / "linux64" / "steamclient.so").is_file(), "the Steamworks redistributable is part of the install"
    assert (dest / "ConanSandbox" / "Saved").is_dir(), "the server needs somewhere to put its world"
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    assert len(ledger["inputs"]) == 6
    assert {row["path"] for row in ledger["inputs"]} == set(read_target(repo)["inputs"]["server"]["files"])

    rows = argv_rows(dd_log)
    assert {row[row.index("-depot") + 1] for row in rows} == {CONTENT_DEPOT, REDIST_DEPOT}
    for row in rows:
        assert "-manifest" in row, "a depot is never fetched by branch head alone"
        assert row[row.index("-manifest") + 1] in set(PINNED.values())
        assert "-validate" in row

    before = len(rows)
    code, payload, _ = install(run, repo, dest)
    assert code == 0
    assert payload["status"] == "already-installed"
    assert len(argv_rows(dd_log)) == before, "an installed target re-downloads nothing"


def test_a_dry_run_writes_nothing(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"

    code, payload, err = install(run, repo, dest, "--dry-run")

    assert code == 0, err
    assert payload["status"] == "dry-run"
    assert not dest.exists()
    assert argv_rows(dd_log) == []


def test_a_wrong_hash_or_missing_manifest_leaves_the_install_untouched(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    before = tree_hash(dest)

    record = read_target(repo)
    record["inputs"]["server"]["files"][SHIPPING]["sha256"] = "0" * 64
    write_target(repo, record)

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert tree_hash(dest) == before
    assert not list(dest.parent.glob(f"{dest.name}.staging-*"))

    # A manifest this cache has never held, which Steam then refuses to serve: the install
    # is an upstream failure rather than a quiet fall back to whatever the branch head is.
    repin_conan(repo, MOVED)
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", MOVED[CONTENT_DEPOT])

    code, payload, _ = install(run, repo, dest)

    assert code == 4, payload
    assert "not falling back to branch head" in payload["error"]
    assert tree_hash(dest) == before
    assert not list(dest.parent.glob(f"{dest.name}.staging-*"))


def test_preserve_keeps_saved_data_and_the_bridge_across_a_repin_and_rollback_restores(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    world = dest / "ConanSandbox" / "Saved" / "game_0.db"
    world.parent.mkdir(parents=True, exist_ok=True)
    world.write_bytes(b"a world somebody played in")
    ini = dest / "ConanSandbox" / "Saved" / "Config" / "LinuxServer" / "Game.ini"
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text("[RconPlugin]\nRconEnabled=1\n", encoding="utf-8")
    bridge = dest / INSTALL_DIR / BRIDGE_FOLDER / "dist" / "index.js"
    bridge.parent.mkdir(parents=True, exist_ok=True)
    bridge.write_text("the deployed bridge\n", encoding="utf-8")
    old_fingerprint = json.loads((dest / ".takaro" / "installed-target.json").read_text())["fingerprint"]

    repin_conan(repo, MOVED)
    code, payload, err = install(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert world.read_bytes() == b"a world somebody played in"
    assert "[RconPlugin]" in ini.read_text()
    assert bridge.read_text() == "the deployed bridge\n"
    assert (dest.with_name(dest.name + ".previous") / "ConanSandboxServer.sh").is_file()
    assert json.loads((dest / ".takaro" / "installed-target.json").read_text())["fingerprint"] != old_fingerprint

    code, payload, err = install(run, repo, dest, "--rollback")

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "rolled-back"
    assert json.loads((dest / ".takaro" / "installed-target.json").read_text())["fingerprint"] == old_fingerprint

    # The rollback keeps what it replaced, so rolling back again is the round trip.
    code, payload, err = install(run, repo, dest, "--rollback")
    assert code == 0, f"{err}\n{payload}"
    assert json.loads((dest / ".takaro" / "installed-target.json").read_text())["fingerprint"] != old_fingerprint

    shutil.rmtree(dest.with_name(dest.name + ".previous"))
    code, payload, _ = install(run, repo, dest, "--rollback")
    assert code == 7, payload
    assert "there is nothing to roll back to" in payload["error"]


# -- build and deploy --------------------------------------------------------------------


def test_build_selects_the_exact_zip_name_and_meta(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_conan_build_stub(repo, resolved["fingerprint"])
    out = tmp_path / "dist"

    code, payload, err = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(out), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert [row["role"] for row in payload["artifacts"]] == ["bridge"]
    assert payload["artifacts"][0]["file"] == ZIP_NAME
    assert (out / ZIP_NAME).is_file()
    assert (out / f"{ZIP_NAME}.meta.json").is_file()
    manifest = json.loads((out / "build-manifest.json").read_text())
    assert manifest["artifacts"][0]["fingerprint"] == resolved["fingerprint"]
    with zipfile.ZipFile(out / ZIP_NAME) as archive:
        names = set(archive.namelist())
    assert f"{BRIDGE_FOLDER}/dist/index.js" in names
    assert f"{BRIDGE_FOLDER}/dist/mod/pollerCli.js" in names, "the chat helper travels inside the bridge zip"


def test_a_build_that_writes_the_legacy_name_is_refused(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_conan_build_stub(repo, resolved["fingerprint"], name="takaro-conan-exiles-bridge.zip")

    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "dist"), repo=repo
    )

    assert code == 7, payload
    assert "did not produce" in payload["error"]


def test_a_build_stamped_for_another_target_is_refused(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_conan_build_stub(repo, resolved["fingerprint"], target="linux-99999999")

    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "dist"), repo=repo
    )

    assert code == 7, payload
    assert "does not carry this target's identity" in payload["error"]


def bridge_zip(path: Path, *, version: str = VERSION, escape: bool = False, root: str = BRIDGE_FOLDER) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(f"{root}/", "")
        archive.writestr(f"{root}/dist/index.js", "bridge\n")
        archive.writestr(f"{root}/dist/mod/pollerCli.js", "helper\n")
        archive.writestr(
            f"{root}/takaro-target.json",
            json.dumps({"target": TARGET, "connectorVersion": version, "revision": "25488622"}),
        )
        if escape:
            archive.writestr("../escaped.txt", "nope")


def manifest_for(run: Any, repo: Path, directory: Path, zip_path: Path, *, target: str = TARGET) -> Path:
    resolved = resolve(run, repo)
    row = artifact_row("bridge", target, resolved["fingerprint"], zip_path)
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


def deploy(run: Any, repo: Path, dest: Path, manifest: Path) -> tuple[int, Any, str]:
    return run("deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo)


def test_deploy_unpacks_the_bridge_folder_and_removes_older_zips(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    stale = dest / INSTALL_DIR / f"takaro-conan-exiles-bridge-{TARGET}-1.0.1.zip"
    stale.parent.mkdir(parents=True, exist_ok=True)
    stale.write_bytes(b"an older deploy")
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME)
    manifest = manifest_for(run, repo, directory, directory / ZIP_NAME)

    code, payload, err = deploy(run, repo, dest, manifest)

    assert code == 0, f"{err}\n{payload}"
    unpacked = dest / INSTALL_DIR / BRIDGE_FOLDER
    assert (unpacked / "dist" / "index.js").is_file()
    assert (unpacked / "dist" / "mod" / "pollerCli.js").is_file()
    assert json.loads((unpacked / "takaro-target.json").read_text())["target"] == TARGET
    assert not stale.exists()
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    assert ledger["artifacts"][0]["path"] == f"{INSTALL_DIR}/{ZIP_NAME}"


@pytest.mark.parametrize("body", [b"not a zip at all", b"PK\x03\x04truncated"])
def test_an_artifact_that_is_not_a_zip_leaves_the_deployed_bridge_and_its_config_alone(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, body: bytes
) -> None:
    """The manifest's sha256 says the bytes are the built ones, not that they are a zip."""
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME)
    assert deploy(run, repo, dest, manifest_for(run, repo, directory, directory / ZIP_NAME))[0] == 0
    unpacked = dest / INSTALL_DIR / BRIDGE_FOLDER
    config = unpacked / "TakaroConfig.txt"
    config.write_text("registrationToken=the-operators-own\n")
    before = sorted(path.relative_to(unpacked).as_posix() for path in unpacked.rglob("*"))

    (directory / ZIP_NAME).write_bytes(body)
    code, payload, _ = run(
        "deploy",
        "--game",
        GAME,
        "--target",
        TARGET,
        "--dest",
        str(dest),
        "--from",
        str(manifest_for(run, repo, directory, directory / ZIP_NAME)),
        repo=repo,
    )

    assert code == 7, payload
    assert "is not a zip archive" in json.dumps(payload)
    assert config.read_text() == "registrationToken=the-operators-own\n"
    assert sorted(path.relative_to(unpacked).as_posix() for path in unpacked.rglob("*")) == before
    assert not list((dest / INSTALL_DIR).glob(".TakaroConanExiles.staging*"))


def test_a_valid_zip_missing_the_bridge_tree_leaves_the_deployed_bridge_alone(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    """A valid zip is not necessarily a complete release; validate it before the swap."""
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME)
    assert deploy(run, repo, dest, manifest_for(run, repo, directory, directory / ZIP_NAME))[0] == 0
    live = dest / INSTALL_DIR / BRIDGE_FOLDER / "dist" / "index.js"
    before = live.read_bytes()
    installed_archive = dest / INSTALL_DIR / ZIP_NAME
    archive_before = installed_archive.read_bytes()

    with zipfile.ZipFile(directory / ZIP_NAME, "w"):
        pass
    code, payload, _ = deploy(run, repo, dest, manifest_for(run, repo, directory, directory / ZIP_NAME))

    assert code == 7, payload
    assert "is missing" in payload["error"]
    assert live.read_bytes() == before
    assert installed_archive.read_bytes() == archive_before


def test_a_failed_final_directory_swap_restores_the_deployed_bridge(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME)
    manifest = manifest_for(run, repo, directory, directory / ZIP_NAME)
    assert deploy(run, repo, dest, manifest)[0] == 0
    unpacked = dest / INSTALL_DIR / BRIDGE_FOLDER
    before = sorted(
        (path.relative_to(unpacked).as_posix(), path.read_bytes()) for path in unpacked.rglob("*") if path.is_file()
    )
    installed_archive = dest / INSTALL_DIR / ZIP_NAME
    archive_before = installed_archive.read_bytes()

    real_replace = os.replace
    refused = False

    def refuse_incoming_tree(src: Any, dst: Any, *args: Any, **kwargs: Any) -> None:
        nonlocal refused
        if not refused and Path(dst) == unpacked and Path(src).parent.name == ".TakaroConanExiles.staging":
            refused = True
            raise OSError(28, "No space left on device")
        real_replace(src, dst, *args, **kwargs)

    monkeypatch.setattr(os, "replace", refuse_incoming_tree)

    code, _, _ = deploy(run, repo, dest, manifest)

    assert code != 0
    assert refused
    assert (
        sorted(
            (path.relative_to(unpacked).as_posix(), path.read_bytes()) for path in unpacked.rglob("*") if path.is_file()
        )
        == before
    )
    assert installed_archive.read_bytes() == archive_before
    assert not (unpacked.parent / f".{BRIDGE_FOLDER}.previous").exists()


def test_deploy_keeps_the_operators_config_and_drops_the_previous_release(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    """``TakaroConfig.txt`` is the operator's, is never in the zip, and README promises it survives."""
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    unpacked = dest / INSTALL_DIR / BRIDGE_FOLDER
    unpacked.mkdir(parents=True, exist_ok=True)
    config = unpacked / "TakaroConfig.txt"
    config.write_text("registrationToken=the-operators-own\nrconPassword=theirs\n")
    config.chmod(0o600)
    (unpacked / "dist").mkdir(parents=True, exist_ok=True)
    stale_code = unpacked / "dist" / "removed-in-the-new-release.js"
    stale_code.write_text("// from the previous version")
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME)
    manifest = manifest_for(run, repo, directory, directory / ZIP_NAME)

    code, payload, err = deploy(run, repo, dest, manifest)

    assert code == 0, f"{err}\n{payload}"
    assert config.read_text() == "registrationToken=the-operators-own\nrconPassword=theirs\n"
    assert oct(config.stat().st_mode)[-3:] == "600"
    assert not stale_code.exists(), "everything the new artifact does not carry is still replaced"
    assert (unpacked / "dist" / "index.js").is_file()


def test_a_zip_that_escapes_the_bridge_folder_is_refused(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME, escape=True)
    manifest = manifest_for(run, repo, directory, directory / ZIP_NAME)

    code, payload, _ = deploy(run, repo, dest, manifest)

    assert code == 7, payload
    assert not (dest / INSTALL_DIR / BRIDGE_FOLDER).exists()
    assert not (tmp_path / "escaped.txt").exists()


def test_a_zip_rooted_somewhere_else_is_refused(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME, root="Other")
    manifest = manifest_for(run, repo, directory, directory / ZIP_NAME)

    code, payload, _ = deploy(run, repo, dest, manifest)

    assert code == 7, payload
    assert f"outside the single {BRIDGE_FOLDER}/ folder" in payload["error"]
    assert not (dest / INSTALL_DIR / "Other").exists()


def test_a_manifest_built_for_another_target_is_refused(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    assert install(run, repo, dest)[0] == 0
    directory = tmp_path / "dist"
    bridge_zip(directory / ZIP_NAME)
    manifest = manifest_for(run, repo, directory, directory / ZIP_NAME, target="linux-99999999")

    code, payload, _ = deploy(run, repo, dest, manifest)

    assert code == 7, payload
    assert not (dest / INSTALL_DIR / BRIDGE_FOLDER).exists()


# -- verification hooks ------------------------------------------------------------------

BANNER_BUILD = "[2026.09.21-18.00.00:000][  0]LogInit: Build: ++exiles+release-CL-373655"
BANNER_ENGINE = "[2026.09.21-18.00.00:000][  0]LogInit: Engine Version: 5.6.1-373655+++exiles+release"


def test_the_runtime_identity_comes_from_the_two_server_banners(tmp_path: Path) -> None:
    adapter = adapter_for(GAME)

    assert adapter.parse_runtime_identity(BANNER_BUILD) == {
        "gameVersion": "exiles+release-CL-373655",
        "loader": "unreal",
        "loaderVersion": None,
    }
    assert adapter.parse_runtime_identity(BANNER_ENGINE) == {
        "gameVersion": None,
        "loader": "unreal",
        "loaderVersion": "5.6.1",
    }
    assert adapter.parse_runtime_identity("LogInit: Display: Starting Game.") is None

    log = tmp_path / "server.log"
    log.write_text(f"noise\n{BANNER_BUILD}\n{BANNER_ENGINE}\nmore noise\n", encoding="utf-8")
    assert hooks.scan_runtime_identity(adapter, log) == {
        "gameVersion": "exiles+release-CL-373655",
        "loader": "unreal",
        "loaderVersion": "5.6.1",
    }


def test_verify_hooks_know_the_conan_log_lines() -> None:
    assert hooks.READY_LINE.search(
        "[2026.09.21-18.02.11:123][  0]LogInit: Display: Engine is initialized. Leaving FEngineLoop::Init()"
    )
    rcon = hooks.RCON_READY_LINE.search(
        "[2026.09.21-18.01.50:001][  0]LogRcon: Display: Rcon is ready for client connections on 0.0.0.0:25575!"
    )
    assert rcon and rcon.group("port") == "25575"
    assert hooks.IDENTIFIED_LINE.search("info: Identified with Takaro as gameServerId=00000000-0000-0000-0000-0")
    assert hooks.CLOSED_LINE.search("info: Takaro WebSocket closed code=1001 reason=going away")
    stamp = hooks.STAMP_LINE.search(
        "info: Takaro target: linux-25488622 (0123456789abcdef) revision 25488622 connector 1.0.2 source deadbeef"
    )
    assert stamp and stamp.group("target") == TARGET and stamp.group("fp16") == "0123456789abcdef"
    command = hooks.RCON_COMMAND_LINE.search("IP PeerAddr: 172.17.0.3:51000 used rcon command: listplayers")
    assert command and command.group("command") == "listplayers"
    assert hooks.RECONNECT_BUDGET >= 60.0


def test_catalogue_exclusions_match_the_documented_fresh_save_limit() -> None:
    readme = (REPO_ROOT / "games/conan-exiles/README.md").read_text(encoding="utf-8")
    for check_id, readme_phrase in (("catalog-items", "item ids"), ("catalog-entities", "creature/actor classes")):
        reason = hooks.UNSUPPORTED_CHECKS[check_id]
        assert re.search(r"save database", reason, re.I)
        assert "fresh save" in reason
        assert f"only the {readme_phrase}" in readme


def test_before_boot_writes_the_rcon_settings_the_server_reads(tmp_path: Path) -> None:
    class Run:
        data_dir = tmp_path
        ws_url = ""

    run = Run()
    hooks.before_boot(run, {"TAKARO_WS_URL": "ws://host.docker.internal:34567/"})

    assert run.ws_url == "ws://host.docker.internal:34567/"
    ini = tmp_path / "ConanSandbox" / "Saved" / "Config" / "LinuxServer" / "Game.ini"
    assert oct(ini.stat().st_mode)[-3:] == "600"
    body = ini.read_text()
    assert "[RconPlugin]" in body
    assert "RconEnabled=1" in body
    assert "RconMaxKarma=1000" in body
    password = re.search(r"RconPassword=(\S+)", body)
    assert password and len(password.group(1)) >= 16
    secret = tmp_path / ".takaro" / "runtime" / "rcon-password"
    assert oct(secret.stat().st_mode)[-3:] == "600"
    assert hooks.rcon_password(tmp_path) == password.group(1)
    assert (tmp_path / ".takaro" / "home").is_dir()

    # Run twice on the same (preserved) data directory: the existing section is rewritten,
    # so the password the sidecar is handed is the one the server actually reads.
    hooks.before_boot(run, {"TAKARO_WS_URL": "ws://host.docker.internal:34567/"})
    second = ini.read_text()
    assert second.count("[RconPlugin]") == 1
    rewritten = re.search(r"RconPassword=(\S+)", second)
    assert rewritten and rewritten.group(1) != password.group(1)
    assert hooks.rcon_password(tmp_path) == rewritten.group(1)
    assert oct(ini.stat().st_mode)[-3:] == "600"


def test_before_boot_leaves_the_servers_other_ini_sections_alone(tmp_path: Path) -> None:
    class Run:
        data_dir = tmp_path
        ws_url = ""

    ini = tmp_path / "ConanSandbox" / "Saved" / "Config" / "LinuxServer" / "Game.ini"
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text("[ServerSettings]\nMaxNudity=0\n\n[RconPlugin]\nRconPassword=the-previous-run\n")

    hooks.before_boot(Run(), {"TAKARO_WS_URL": "ws://host.docker.internal:34567/"})

    body = ini.read_text()
    assert "[ServerSettings]" in body and "MaxNudity=0" in body
    assert "the-previous-run" not in body
    assert body.count("[RconPlugin]") == 1
    assert "RconMaxKarma=1000" in body


def test_the_bridge_config_names_the_game_container_and_carries_both_tokens(tmp_path: Path) -> None:
    written = hooks.render_bridge_config(
        tmp_path,
        registration="a-throwaway-registration-token",
        identity="takaro-verify-tm148-162",
        server_name="takaro-verify-tm148-162",
        url="ws://host.docker.internal:34567/",
        rcon_host="172.17.0.4",
        rcon_pw="a-throwaway-rcon-password",
    )

    assert written == tmp_path / ".takaro" / "runtime" / "bridge" / "TakaroConfig.txt"
    assert oct(written.stat().st_mode)[-3:] == "600"
    body = written.read_text()
    assert "rconHost=172.17.0.4" in body
    assert "rconPort=25575" in body
    assert "takaroWsUrl=ws://host.docker.internal:34567/" in body
    assert "registrationToken=a-throwaway-registration-token" in body
    assert "identityToken=takaro-verify-tm148-162" in body
    assert "logFiles=/bridge/logs/ConanSandbox.log" in body


def test_the_container_command_starts_the_launcher_without_the_password(run: Any, repo: Path, tmp_path: Path) -> None:
    adapter = adapter_for(GAME)
    resolved = resolve(run, repo)

    command = adapter.container_command(resolved, tmp_path)

    assert command == [
        "/conan/ConanSandboxServer.sh",
        "-log",
        "-server",
        "-nosteamclient",
        "-Port=7777",
        "-QueryPort=27015",
        "-RconEnabled=1",
        "-RconPort=25575",
    ]
    assert not any("Password" in part for part in command), "a container argv is logged and inspected"

    options = adapter.container_options(resolved, tmp_path)
    assert options[:2] == ["--memory", "14g"], "the runner's 3g default would kill a 10 GB server"
    assert options[2] == "--user" and options[3] == f"{os.getuid()}:{os.getgid()}"

    mounts = adapter.container_mounts(resolved, tmp_path)
    assert mounts == [f"{tmp_path}:/conan"]
    assert (tmp_path / "ConanSandbox" / "Saved" / "Logs").is_dir()
    assert adapter.runtime_env(resolved, {}) == {"HOME": "/conan/.takaro/home"}


def test_the_check_ids_add_the_conan_bridge_checks() -> None:
    from takaro_maint.verify.runner import check_ids

    ids = check_ids(GAME)

    assert set(hooks.CHECK_IDS) <= set(ids)
    assert {
        "bridge-identify",
        "bridge-reachability",
        "bridge-players",
        "bridge-console",
        "bridge-reconnect",
        "bridge-stop",
    } <= set(ids)


def test_the_rcon_command_log_is_read_in_order(tmp_path: Path) -> None:
    log = tmp_path / "ConanSandbox" / "Saved" / "Logs" / "RconCommandLog.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(
        "IP PeerAddr: 172.17.0.3:51000 used rcon command: help\n"
        "not an rcon line at all\n"
        "IP PeerAddr: 172.17.0.3:51002 used rcon command: listplayers\n"
        "IP PeerAddr: 172.17.0.3:51004 used rcon command: shutdown\n",
        encoding="utf-8",
    )

    assert hooks.rcon_commands_seen(tmp_path) == ["help", "listplayers", "shutdown"]
    assert hooks.rcon_commands_seen(tmp_path / "nowhere") == []


# -- the release record ------------------------------------------------------------------


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


def test_compat_record_carries_the_steam_pin_and_the_legacy_alias(run: Any, repo: Path, tmp_path: Path) -> None:
    git(repo, "init", "-q", "-b", "main")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "fixture")
    commit = git(repo, "rev-parse", "HEAD")
    resolved = resolve(run, repo)
    directory = tmp_path / "dist" / TARGET
    bridge_zip(directory / ZIP_NAME)
    row = artifact_row("bridge", TARGET, resolved["fingerprint"], directory / ZIP_NAME)
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
    assert entry["verification"] == {
        "required": "contract",
        "executed": None,
        "report": None,
        "outcome": None,
        "takaro": None,
    }
    url = entry["inputs"]["server"]["url"]
    assert url.startswith("steam://app/443030/")
    assert len(re.findall(r"manifest/[0-9]+", url)) == 2, "both depots are named in the pseudo-URL"
    assert (out / ZIP_NAME).is_file()
    assert (out / "takaro-conan-exiles-bridge.zip").read_bytes() == (out / ZIP_NAME).read_bytes()


# -- the dev-servers rig -----------------------------------------------------------------

DS_ROOT = Path(__file__).resolve().parents[2] / "dev-servers"
REPO_ROOT = Path(__file__).resolve().parents[2]


def bash(script: str) -> str:
    completed = subprocess.run(["bash", "-c", script], cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


def test_dev_servers_conan_functions_dispatch() -> None:
    scripts = (DS_ROOT / "lib/games/conan-exiles.sh", DS_ROOT / "images/conan-exiles/entrypoint.sh")
    for script in scripts:
        completed = subprocess.run(["bash", "-n", str(script)], capture_output=True, text=True, check=False)
        assert completed.returncode == 0, f"{script.name}: {completed.stderr}"

    assert bash(". dev-servers/lib/common.sh; ds_target_prefix conan-exiles").strip() == "CONAN_EXILES"
    assert bash(". dev-servers/lib/common.sh; ds_target_game conan-exiles").strip() == "conan-exiles"
    assert bash(". dev-servers/lib/common.sh; ds_target_dest conan-exiles").strip().endswith("/conan-exiles/server")
    for step in ("install", "deploy"):
        found = bash(f'. dev-servers/lib/common.sh; declare -F "{step}_conan_exiles" >/dev/null && echo yes')
        assert found.strip() == "yes", f"conan-exiles has no {step} step"


def test_the_rig_runs_the_resolved_target_and_never_steamcmd() -> None:
    compose = (DS_ROOT / "compose" / "conan-exiles.yml").read_text(encoding="utf-8")

    assert 'image: "${CONAN_EXILES_IMAGE:-target-not-resolved}"' in compose
    assert 'image: "${CONAN_EXILES_TOOLCHAIN:-target-not-resolved}"' in compose
    assert "build:" not in compose, "the rig runs a pinned image, never one it builds here"
    assert not (DS_ROOT / "images" / "conan-exiles" / "Dockerfile").exists()

    owned = [
        "games/conan-exiles",
        "dev-servers/lib/games/conan-exiles.sh",
        "dev-servers/compose/conan-exiles.yml",
        "dev-servers/images/conan-exiles",
        ".github/workflows/conan-exiles.yml",
    ]
    found = subprocess.run(
        ["git", "grep", "-nE", r"app_update|steamcmd|node:22-slim|:latest", "--", *owned],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    # git grep exits 1 when nothing matched, which is exactly what this asserts.
    assert found.returncode == 1, f"a floating coordinate survived:\n{found.stdout}"


def test_the_server_container_gets_the_memory_and_the_user_the_adapter_asks_for(tmp_path: Path) -> None:
    """The server container runs with the memory cap and the user ``container_options`` asks for."""
    import os

    from takaro_maint import paths
    from takaro_maint.catalog.loader import load
    from takaro_maint.games.conan_exiles import MEMORY
    from takaro_maint.verify.runner import RunOptions, TargetRun

    paths.set_repo_root(REPO_ROOT)
    catalog = load()
    options = RunOptions(artifacts=tmp_path / "dist", out=tmp_path / "out", run_id="argv")
    run = TargetRun(catalog, catalog.select(GAME, target_id=TARGET), options)
    try:
        argv = run.container_argv("ws://host.docker.internal:1/")
    finally:
        run.cleanup()

    memory = [index for index, item in enumerate(argv) if item == "--memory"]
    assert [argv[index + 1] for index in memory] == ["3g", MEMORY], "docker takes the last one"
    user = argv.index("--user")
    assert argv[user + 1] == f"{os.getuid()}:{os.getgid()}"
    assert user > memory[0], "the adapter's options come after the runner's own"


def test_the_stamp_line_the_harness_reads_is_the_one_the_bridge_writes() -> None:
    """The contract is across two languages, so it has to be bound rather than restated.

    `hooks.STAMP_LINE` is a Python regex with named groups; the line it reads is built by
    a TypeScript template. The test binds their literal text so a rename in either cannot
    leave the other looking correct while `bridge-identify` reports an unstamped bridge.
    """
    source = (REPO_ROOT / "games/conan-exiles/bridge/src/targetStamp.ts").read_text(encoding="utf-8")

    body = source[source.index("export function describeStamp") :]
    body = body[: body.index("\n}")]
    templates = re.findall(r"`([^`]*)`", body)
    assert templates, "describeStamp builds its line from template literals"
    # Every `${...}` becomes a value, and the literal text between them is the contract.
    sample = "".join(templates)
    values = {
        "stamp.target": "linux-25488622",
        "stamp.fingerprint.slice(0, 16)": "0123456789abcdef",
        "stamp.revision": "25488622",
        "stamp.connectorVersion": "1.2.3",
        "stamp.sourceRevision": "deadbeef",
    }
    for expression, value in values.items():
        sample = sample.replace("${" + expression + "}", value)
    assert "${" not in sample, f"describeStamp interpolates something this test does not know: {sample}"

    found = hooks.STAMP_LINE.search(sample)
    assert found, f"{hooks.STAMP_LINE.pattern!r} does not match {sample!r}"
    assert found.group("target") == "linux-25488622"
    assert found.group("fp16") == "0123456789abcdef"
    assert found.group("revision") == "25488622"
    assert found.group("version") == "1.2.3"
