"""The Project Zomboid target end to end: catalog, resolution, pin, references, install,
build, deploy, verification hooks, the release record and the rig's plumbing.

Everything here runs the real command against the DepotDownloader stand-in and a stub
build script, so what is asserted is what a maintainer, the rig and CI observe — never a
helper's shape. The 7D2D helpers in ``fake_depotdownloader`` are 7D2D's own, so this
module carries its own repository copy and re-pinner for the Zomboid record.
"""

from __future__ import annotations

import asyncio
import hashlib
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
from fake_verify import CannedSocket, FakeRun
from takaro_maint.games import adapter_for
from takaro_maint.games.zomboid import verify as hooks
from takaro_maint.publish.manifest import artifact_row, write_manifest, write_meta

GAME = "zomboid"
TARGET = "linux-42.20.4"
VERSION = "1.0.2-dev.abc1234"
JAR_NAME = f"takaro-zomboid-agent-{TARGET}-{VERSION}.jar"
GAME_JAR = "java/projectzomboid.jar"
# The release builds only non-retired targets; the legacy TakaroConnector alias points here.
RELEASE_TARGET = "linux-42.21.0"
RELEASE_JAR = f"takaro-zomboid-agent-{RELEASE_TARGET}-{VERSION}.jar"
RELEASE_BUNDLE = f"takaro-zomboid-{RELEASE_TARGET}-{VERSION}.zip"

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures" / "games" / GAME
DEPOTS = FIXTURES / "depots"
BUILD_SCRIPT = "games/zomboid/scripts/build-release.sh"

PINNED = {
    "1006": "4559160656493359681",
    "380871": "8198713088050651526",
    "380873": "4894029153115054997",
}
# The common depot has moved on; the other two have not.
HEAD = {**PINNED, "380871": "1900000000000000002"}
DECLARED = ("java/projectzomboid.jar", "ProjectZomboid64", "start-server.sh", "jre64/release")


def test_every_zomboid_verification_body_has_pass_and_failure_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    digest = "a" * 64
    target = type(
        "Target",
        (),
        {
            "id": TARGET,
            "fingerprint": "b" * 64,
            "fp16": "b" * 16,
            "record": {"inputs": {"server": {"files": {GAME_JAR: {"sha256": digest}}}}},
        },
    )()
    run = FakeRun(tmp_path, target=target)
    marker = f"takaro-verify-{run.options.run_id}-action"
    target_check = {
        "result": "ok",
        "target": TARGET,
        "fingerprint": target.fingerprint,
        "expected": {"gameJarSha256": digest},
        "runtime": {"gameJarSha256": digest},
    }
    lines = [
        "Picked up JAVA_TOOL_OPTIONS: -javaagent:TakaroConnector.jar",
        "[Takaro] premain: hooks installed",
        "[Takaro] target-check: " + json.dumps(target_check),
        "takaro-maint: SteamCMD is disabled",
        *[f"[Takaro] listener: transformed {name}" for name in hooks.HOOKED_CLASSES],
        *[f"[Takaro] listener: transformed {name}" for name in hooks.LATE_HOOKED_CLASSES],
        "[Takaro] HOOK CONFIRMED: tick",
        "Identified successfully",
        "Identified successfully",
        marker,
        "SERVER SHUTDOWN",
    ]
    run.server_log.write_text("\n".join(lines) + "\n", encoding="utf-8")
    fake = CannedSocket(
        {
            "listItems": [{"code": "Base.Axe", "name": "Firefighter Axe"}],
            "listEntities": [{"code": "Zombie", "name": "Zombie"}],
            "executeConsoleCommand": {"success": True, "rawResult": "Players connected (0)"},
            "sendMessage": {"success": True},
            "listBans": [],
            "testReachability": {"connectable": True},
            "shutdown": {},
        },
        identify_count=1,
    )
    monkeypatch.setattr(hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=1))
    monkeypatch.setattr(
        hooks.subprocess, "run", lambda *args, **kwargs: subprocess.CompletedProcess(args[0], 0, "", "")
    )
    monkeypatch.setattr(hooks, "_reclaim", lambda run: None)

    assert asyncio.run(hooks._check_agent_load(run, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_pinned_install(run)).status == "pass"
    assert asyncio.run(hooks._check_hooks_bound(run, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_catalog(fake)).status == "pass"
    assert asyncio.run(hooks._check_rcon(fake)).status == "pass"
    assert asyncio.run(hooks._check_action(run, fake)).status == "pass"
    assert asyncio.run(hooks._check_reconnect(run, fake, lambda: True)).status == "pass"
    assert asyncio.run(hooks._check_stop(run, fake, [])).status == "pass"
    asyncio.run(hooks.after_protocol(run, fake, lambda: True))
    asyncio.run(hooks.after_shutdown(run, fake, run.ws_url, []))

    failed_run = FakeRun(tmp_path / "failed", target=target, wanted=set())
    failed_run.container = None
    failed = CannedSocket(
        {
            "listItems": [],
            "listEntities": RuntimeError("no catalog"),
            "executeConsoleCommand": None,
            "sendMessage": RuntimeError("no action"),
            "listBans": ["someone"],
            "testReachability": None,
            "shutdown": RuntimeError("closed"),
        },
        reconnects=False,
    )
    monkeypatch.setattr(
        hooks.checks_lifecycle, "identify_within", lambda *args, **kwargs: asyncio.sleep(0, result=None)
    )
    assert asyncio.run(hooks._check_agent_load(failed_run, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_pinned_install(failed_run)).status == "fail"
    assert asyncio.run(hooks._check_hooks_bound(failed_run, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_catalog(failed)).status == "fail"
    assert asyncio.run(hooks._check_rcon(failed)).status == "fail"
    assert asyncio.run(hooks._check_action(failed_run, failed)).status == "fail"
    assert asyncio.run(hooks._check_reconnect(failed_run, failed, lambda: False)).status == "fail"
    assert asyncio.run(hooks._check_stop(failed_run, failed, [{"path": "missing"}])).status == "fail"
    asyncio.run(hooks.after_protocol(failed_run, failed, lambda: False))
    asyncio.run(hooks.after_shutdown(failed_run, failed, failed_run.ws_url, []))
    assert {check for check, _ in failed_run.skips} == {*hooks.CHECK_IDS, "stop"}


# --------------------------------------------------------------------------- the repository copy


def _tree(depot: str, manifest: str) -> Path:
    return DEPOTS / depot / manifest / "tree"


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _locate(manifests: dict[str, str], relative: str) -> Path:
    for depot in sorted(manifests):
        candidate = _tree(depot, manifests[depot]) / relative
        if candidate.is_file():
            return candidate
    raise AssertionError(f"no fixture depot serves {relative}")


def repin(root: Path, manifests: dict[str, str] | None = None) -> dict[str, Any]:
    """Point the copied Zomboid target at the fixture depots and record their real hashes."""
    manifests = manifests or PINNED
    record = read_target(root)
    server = record["inputs"]["server"]
    server["depots"] = {
        depot: {
            "manifest": manifest,
            "size": sum(f.stat().st_size for f in _tree(depot, manifest).rglob("*") if f.is_file()),
            "files": len([f for f in _tree(depot, manifest).rglob("*") if f.is_file()]),
        }
        for depot, manifest in sorted(manifests.items())
    }
    server["files"] = {
        relative: {"sha256": _sha256(_locate(manifests, relative)), "size": _locate(manifests, relative).stat().st_size}
        for relative in sorted(server["files"])
    }
    write_target(root, record)
    return record


def read_target(root: Path) -> dict[str, Any]:
    path = root / "catalog" / GAME / "targets" / f"{TARGET}.json"
    return json.loads(path.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def write_target(root: Path, record: dict[str, Any]) -> Path:
    path = root / "catalog" / GAME / "targets" / f"{TARGET}.json"
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return path


def make_repo(tmp_path: Path) -> Path:
    """A repository copy holding the real catalog, the tool lock and a stub build script."""
    root = tmp_path / "repo"
    shutil.copytree(REPO_ROOT / "catalog", root / "catalog")
    (root / "maintenance").mkdir(parents=True, exist_ok=True)
    shutil.copy2(REPO_ROOT / "maintenance" / "tools.lock.json", root / "maintenance" / "tools.lock.json")
    for script in (BUILD_SCRIPT, "games/7d2d/scripts/build-release.sh"):
        path = root / script
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        path.chmod(0o755)
    repin(root)
    return root


def environment(log: Path, **extra: str) -> dict[str, str]:
    env = {
        "TAKARO_MAINT_DEPOTDOWNLOADER": str(Path(fake.__file__).resolve()),
        "FAKE_DD_ROOT": str(DEPOTS),
        "FAKE_DD_LOG": str(log),
    }
    env.update(extra)
    return env


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    return make_repo(tmp_path)


@pytest.fixture
def dd_log(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    log = tmp_path / "depotdownloader-argv.jsonl"
    for key, value in environment(log).items():
        monkeypatch.setenv(key, value)
    return log


def resolve(run: Any, repo: Path, target: str = TARGET) -> dict[str, Any]:
    code, payload, err = run("targets", "resolve", "--game", GAME, "--target", target, repo=repo)
    assert code == 0, err
    return dict(payload)


def tree_hash(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


# --------------------------------------------------------------------------- 1-2 catalog


def test_catalog_validate_accepts_the_zomboid_target(run: Any) -> None:
    code, payload, _ = run("catalog", "validate")

    assert code == 0, payload
    rows = [check for check in payload["checks"] if check["file"].endswith(f"{TARGET}.json")]
    assert {check["id"] for check in rows} >= {
        "input-kind-schema",
        "build-system-schema",
        "build-script-exists",
        "immutable-tag-and-digest",
        "no-null-hash",
    }
    assert all(check["status"] == "pass" for check in rows), rows


def test_targets_resolve_env_for_zomboid(run: Any) -> None:
    code, payload, _ = run("targets", "resolve", "--game", GAME, "--target", TARGET, "--prefix", "ZOMBOID")

    assert code == 0, payload
    env = payload["env"]
    assert env["ZOMBOID_STEAM_DEPOTS"] == (
        "1006:4559160656493359681;380871:8198713088050651526;380873:4894029153115054997"
    )
    assert env["ZOMBOID_STEAM_APP"] == "380870"
    assert env["ZOMBOID_STEAM_BRANCH"] == "public"
    assert env["ZOMBOID_JAVA"] == "25"
    assert env["ZOMBOID_ARTIFACT"] == "takaro-zomboid-agent-linux-42.20.4-{version}.jar"
    assert env["ZOMBOID_REFERENCES_DIR"].endswith(payload["fp16"])
    assert env["ZOMBOID_GAME_JAR_SHA256"] == payload["inputs"]["server"]["files"][GAME_JAR]["sha256"]
    assert env["ZOMBOID_AGENT_PATH"] == "/home/steam/ZomboidDedicatedServer/Takaro/TakaroConnector.jar"
    assert payload["resolvedUrls"]["server"].startswith("steam://app/380870/branch/public/")


# --------------------------------------------------------------------------- 3-5 steam pin


def test_steam_pin_reports_the_moved_common_depot(run: Any, repo: Path, dd_log: Path) -> None:
    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, repo=repo)

    assert code == 0, payload
    assert payload["changed"] == ["380871"]
    assert payload["snippet"]["depots"]["380871"]["manifest"] == HEAD["380871"]
    assert payload["depots"]["1006"]["manifest"] == PINNED["1006"]
    assert "-manifest-only" in fake.argv_log(dd_log)[0]


def test_steam_pin_refuses_to_write_without_re_recorded_hashes(run: Any, repo: Path, dd_log: Path) -> None:
    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, "--write", repo=repo)

    assert code == 7, payload
    assert "--record-files" in payload["error"]
    assert read_target(repo)["inputs"]["server"]["depots"]["380871"]["manifest"] == PINNED["380871"]


def test_steam_pin_reports_an_unavailable_manifest_as_upstream(
    run: Any, repo: Path, dd_log: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", HEAD["380871"])

    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", TARGET, repo=repo)

    assert code == 4, payload
    assert "not falling back to branch head" in payload["error"]


# --------------------------------------------------------------------------- 6-8 references


def references(run: Any, repo: Path, dest: Path, *extra: str) -> tuple[int, Any, str]:
    return run("steam", "references", "--game", GAME, "--target", TARGET, "--dest", str(dest), *extra, repo=repo)


def test_steam_references_fetches_only_the_game_jar(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "references" / "fp"

    code, payload, err = references(run, repo, dest)

    assert code == 0, f"{err}\n{payload}"
    assert payload["status"] == "fetched"
    assert sorted(path.name for path in dest.iterdir() if path.is_file()) == ["projectzomboid.jar"]
    calls = fake.argv_log(dd_log)
    assert all("-filelist" in argv for argv in calls)
    # Every pinned depot is asked for, each by its own manifest id: nothing is a branch head.
    assert {argv[argv.index("-depot") + 1]: argv[argv.index("-manifest") + 1] for argv in calls} == PINNED
    marker = json.loads((dest / ".takaro" / "references.json").read_text())
    assert marker["fingerprint"] == resolve(run, repo)["fingerprint"]
    assert [row["from"] for row in marker["files"]] == [GAME_JAR]

    code, payload, _ = references(run, repo, dest)
    assert code == 0
    assert payload["status"] == "up-to-date"


def test_a_references_directory_for_another_fingerprint_is_refused(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "references" / "fp"
    assert references(run, repo, dest)[0] == 0
    marker_path = dest / ".takaro" / "references.json"
    marker = json.loads(marker_path.read_text())
    marker["fingerprint"] = "9" * 64
    marker_path.write_text(json.dumps(marker))

    code, payload, _ = references(run, repo, dest)

    assert code == 7, payload
    assert "stale reference cache" in payload["error"]
    assert references(run, repo, dest, "--force")[0] == 0


def test_an_altered_reference_jar_is_refused(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "references" / "fp"
    assert references(run, repo, dest)[0] == 0
    (dest / "projectzomboid.jar").write_bytes(b"someone rebuilt this by hand")

    code, payload, _ = references(run, repo, dest)

    assert code == 5, payload


# --------------------------------------------------------------------------- 9-12 install


def installed(run: Any, repo: Path, dest: Path) -> dict[str, Any]:
    code, payload, err = run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)
    assert code == 0, f"{err}\n{payload}"
    return dict(payload)


def test_install_writes_the_steamcmd_stub_the_agent_dir_and_the_ledger(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "server"

    payload = installed(run, repo, dest)

    stub = dest / ".takaro" / "steamcmd-stub" / "steamcmd.sh"
    assert stub.is_file() and os.access(stub, os.X_OK)
    assert TARGET in stub.read_text()
    assert "SteamCMD is disabled" in stub.read_text()
    assert (dest / ".takaro" / "steamcmd-stub" / "README.txt").is_file()
    assert (dest / "Takaro").is_dir()
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    assert ledger["fingerprint"] == resolve(run, repo)["fingerprint"]
    assert {row["path"] for row in ledger["inputs"]} == set(DECLARED)
    assert ledger["container"]["digest"].startswith("sha256:")
    assert payload["status"] == "installed"


def test_the_install_makes_the_depots_programs_executable(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    """DepotDownloader writes every file 0644, and a 0644 start-server.sh cannot boot."""
    dest = tmp_path / "server"

    installed(run, repo, dest)

    launcher = dest / "start-server.sh"
    assert launcher.is_file() and os.access(launcher, os.X_OK), "the launcher script is not executable"
    data = dest / "media" / "x.txt"
    if data.is_file():
        assert not os.access(data, os.X_OK), "a plain data file was marked executable"


def test_a_corrupt_depot_file_leaves_the_install_untouched(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    dest = tmp_path / "server"
    installed(run, repo, dest)
    before = tree_hash(dest)
    repin(repo, HEAD)
    monkeypatch.setenv("FAKE_DD_CORRUPT", GAME_JAR)

    code, payload, _ = run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)

    assert code == 5, payload
    assert "not falling back to branch head" in payload["error"] or "sha256" in payload["error"]
    assert tree_hash(dest) == before


def test_reinstall_is_a_no_op_and_preserves_the_deployed_agent(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "server"
    installed(run, repo, dest)
    deployed = dest / "Takaro" / "TakaroConnector.jar"
    deployed.write_bytes(b"a deployed agent")

    code, payload, _ = run("install", "--game", GAME, "--target", TARGET, "--dest", str(dest), repo=repo)

    assert code == 0, payload
    assert payload["status"] == "already-installed"
    assert deployed.read_bytes() == b"a deployed agent"


def test_rollback_restores_the_previous_install(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "server"
    installed(run, repo, dest)
    first = json.loads((dest / ".takaro" / "installed-target.json").read_text())["fingerprint"]
    repin(repo, HEAD)
    installed(run, repo, dest)
    assert json.loads((dest / ".takaro" / "installed-target.json").read_text())["fingerprint"] != first

    code, payload, err = run(
        "install", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--rollback", repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert json.loads((dest / ".takaro" / "installed-target.json").read_text())["fingerprint"] == first


# --------------------------------------------------------------------------- 13-16 build


def make_jar(path: Path, *, target: str, fingerprint: str, version: str = VERSION, stamp: bool = True) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    attributes = {
        "Manifest-Version": "1.0",
        "Premain-Class": "io.takaro.zomboid.agent.TakaroAgent",
        "Implementation-Version": version,
        "Takaro-Target": target,
        "Takaro-Target-Fingerprint": fingerprint,
        "Takaro-Connector-Version": version,
        "Takaro-Source-Revision": "deadbeef",
        "Takaro-Game-Version": "42.20.4",
        "Takaro-Java-Release": "25",
    }
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("META-INF/MANIFEST.MF", "".join(f"{k}: {v}\n" for k, v in attributes.items()) + "\n")
        if stamp:
            archive.writestr(
                "META-INF/takaro-target.json",
                json.dumps({"target": target, "fingerprint": fingerprint, "game": GAME, "platform": "linux"}),
            )
        archive.writestr("io/takaro/zomboid/agent/TakaroAgent.class", b"\xca\xfe\xba\xbe")
    return path


def write_build_stub(repo: Path, fingerprint: str, *, name: str = JAR_NAME, stamp: bool = True) -> Path:
    """A stand-in for the real release script: the same contract, no Gradle, no container."""
    script = repo / BUILD_SCRIPT
    argv_log = repo / "build-argv.txt"
    helper = repo / "make_jar.py"
    helper.write_text(
        "import sys\n"
        f"sys.path.insert(0, {str(Path(__file__).parent)!r})\n"
        "from pathlib import Path\n"
        "from test_game_zomboid import make_jar\n"
        "make_jar(Path(sys.argv[1]), target=sys.argv[2], fingerprint=sys.argv[3], version=sys.argv[4], "
        "stamp=sys.argv[5] == 'yes')\n",
        encoding="utf-8",
    )
    script.write_text(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        f'printf "%s\\n" "$*" >> "{argv_log}"\n'
        'version="$1"; out="$2"\n'
        'mkdir -p "$out"\n'
        f'python3 "{helper}" "$out/{name}" "{TARGET}" "{fingerprint}" "$version" '
        f'"{"yes" if stamp else "no"}"\n',
        encoding="utf-8",
    )
    script.chmod(0o755)
    return argv_log


def test_build_selects_the_exact_jar_name_and_identity(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(repo, resolved["fingerprint"])
    out = tmp_path / "dist"

    code, payload, err = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(out), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert [row["role"] for row in payload["artifacts"]] == ["agent"]
    assert payload["artifacts"][0]["file"] == JAR_NAME
    assert (out / JAR_NAME).is_file()
    assert (out / f"{JAR_NAME}.meta.json").is_file()
    manifest = json.loads((out / "build-manifest.json").read_text())
    assert manifest["artifacts"][0]["fingerprint"] == resolved["fingerprint"]


def test_a_build_that_writes_the_legacy_jar_name_is_refused(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    write_build_stub(repo, resolved["fingerprint"], name=f"TakaroConnector-{VERSION}.jar")

    code, payload, _ = run(
        "build", "--game", GAME, "--target", TARGET, "--version", VERSION, "--out", str(tmp_path / "dist"), repo=repo
    )

    assert code == 7, payload
    assert "did not produce" in payload["error"]


def test_a_jar_without_target_identity_is_refused(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    jar = make_jar(tmp_path / JAR_NAME, target=TARGET, fingerprint=resolved["fingerprint"], stamp=False)

    code, payload, _ = run("artifact", "validate", "--game", GAME, "--target", TARGET, str(jar), repo=repo)

    assert code == 7, payload
    assert any("takaro-target.json" in problem for row in payload["files"] for problem in row["problems"])


def test_build_forwards_gradle_args_to_the_script(run: Any, repo: Path, tmp_path: Path) -> None:
    resolved = resolve(run, repo)
    argv_log = write_build_stub(repo, resolved["fingerprint"])

    code, payload, err = run(
        "build",
        "--game",
        GAME,
        "--target",
        TARGET,
        "--version",
        VERSION,
        "--out",
        str(tmp_path / "dist"),
        "--gradle-args",
        "--rerun-tasks",
        repo=repo,
    )

    assert code == 0, f"{err}\n{payload}"
    recorded = argv_log.read_text().strip()
    assert "-- --rerun-tasks" in recorded
    assert recorded.index("--target") < recorded.index("-- --rerun-tasks")


# --------------------------------------------------------------------------- 17-18 deploy


def manifest_for(run: Any, repo: Path, directory: Path, jar: Path) -> Path:
    resolved = resolve(run, repo)
    row = artifact_row("agent", TARGET, resolved["fingerprint"], jar)
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


def test_deploy_places_the_agent_and_a_stable_copy_and_removes_older_jars(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "server"
    installed(run, repo, dest)
    (dest / "Takaro" / "takaro-zomboid-agent-linux-42.20.4-0.9.0.jar").write_bytes(b"an older deploy")
    (dest / "Takaro" / "TakaroConnector-0.9.0.jar").write_bytes(b"an even older deploy")
    directory = tmp_path / "dist"
    jar = make_jar(directory / JAR_NAME, target=TARGET, fingerprint=resolve(run, repo)["fingerprint"])
    manifest = manifest_for(run, repo, directory, jar)

    code, payload, err = run(
        "deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo
    )

    assert code == 0, f"{err}\n{payload}"
    assert (dest / "Takaro" / JAR_NAME).read_bytes() == jar.read_bytes()
    assert (dest / "Takaro" / "TakaroConnector.jar").read_bytes() == jar.read_bytes()
    assert not (dest / "Takaro" / "takaro-zomboid-agent-linux-42.20.4-0.9.0.jar").exists()
    assert not (dest / "Takaro" / "TakaroConnector-0.9.0.jar").exists()
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    assert ledger["artifacts"][0]["path"] == f"Takaro/{JAR_NAME}"


def test_deploy_into_a_directory_holding_another_fingerprint_is_refused(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "server"
    installed(run, repo, dest)
    directory = tmp_path / "dist"
    jar = make_jar(directory / JAR_NAME, target=TARGET, fingerprint=resolve(run, repo)["fingerprint"])
    manifest = manifest_for(run, repo, directory, jar)
    # The record moves on after the install: the directory now holds another fingerprint.
    repin(repo, HEAD)
    before = tree_hash(dest)

    code, payload, _ = run(
        "deploy", "--game", GAME, "--target", TARGET, "--dest", str(dest), "--from", str(manifest), repo=repo
    )

    assert code == 7, payload
    assert tree_hash(dest) == before


# --------------------------------------------------------------------------- 19-20 verification hooks


def test_verify_hooks_regexes_and_check_ids() -> None:
    from takaro_maint.verify.runner import check_ids

    assert hooks.READY_LINE.search("1748442937232 *** SERVER STARTED ****")
    assert hooks.STUB_LINE.search(
        "takaro-maint: SteamCMD is disabled — this server is pinned to catalog target linux-42.20.4"
    )
    assert hooks.STEAMCMD_RAN.search(" Update state (0x61) downloading, progress: 12.34")
    assert hooks.JAVA_TOOL_OPTIONS_LINE.search(
        "Picked up JAVA_TOOL_OPTIONS: -javaagent:/home/steam/ZomboidDedicatedServer/Takaro/TakaroConnector.jar"
    )
    assert hooks.HOOKS_INSTALLED_LINE.search("[2026-09-21 10:00:00.000] [Takaro] premain: hooks installed")
    unbound = hooks.UNBOUND_HOOKS_LINE.search(
        "[2026-09-21 10:00:00.000] [Takaro] premain: hooks installed; unbound: [chat, death]"
    )
    assert unbound and unbound.group(1) == "chat, death"
    assert hooks.UNBOUND_HOOKS_LINE.search(
        "[2026-09-21 10:00:00.000] [Takaro] ERROR hook chat: matcher bound nothing on zombie.network.chat.ChatServer"
    )
    # Those two lines are the agent's, built in Java, and matched here by a Python regex.
    # The samples come out of the agent source rather than being restated, so a renamed
    # literal fails here instead of making a check wait out its budget in a live run.
    prefix = "[2026-09-21 10:00:00.000] "
    banner = _java_literal("AgentLog.java", r'String line = "\[" \+ TS.format\(ZonedDateTime.now\(\)\) \+ "(\] .*?) "')
    tick = _java_literal("hooks/Bridge.java", r'AgentLog\.log\("(HOOK CONFIRMED: tick[^"]*)"')
    target_check = _java_literal("TargetGuard.java", r'AgentLog\.log\("(target-check: )" \+ gson\.toJson')
    assert hooks.TICK_CONFIRMED_LINE.search(f"{prefix}{banner} {tick}thread=main")
    assert hooks.TARGET_CHECK_LINE.search(f'{prefix}{banner} {target_check}{{"result":"ok"}}')
    assert set(hooks.HOOKED_CLASSES) == {
        "zombie.network.RCONServer",
        "zombie.network.GameServer",
        "zombie.network.chat.ChatServer",
        "zombie.characters.IsoPlayer",
    }
    # Already loaded when premain runs, so each proves itself its own way.
    assert set(hooks.LATE_HOOKED_CLASSES) == {"zombie.characters.IsoZombie", "zombie.core.logger.ZLogger"}
    zombie = re.compile(hooks.LATE_HOOKED_CLASSES["zombie.characters.IsoZombie"])
    assert zombie.search("[Takaro] watchdog: retransformed zombie.characters.IsoZombie")
    assert not zombie.search("[Takaro] listener: discovery zombie.characters.IsoZombie (loaded=true)")
    logger = re.compile(hooks.LATE_HOOKED_CLASSES["zombie.core.logger.ZLogger"])
    assert logger.search("[Takaro] HOOK CONFIRMED: log (ZLogger.write)")
    assert logger.search("[Takaro] listener: transformed zombie.core.logger.ZLogger (loaded=false)")
    assert not logger.search("[Takaro] listener: discovery zombie.core.logger.ZLogger (loaded=true)")
    assert hooks.RECONNECT_BUDGET >= 60.0
    assert set(check_ids(GAME)) >= set(hooks.CHECK_IDS)
    # The base `console` check sends `say`, which this game has no command for, so this
    # game's console check carries its own id rather than colliding with it.
    assert "console" not in hooks.CHECK_IDS
    assert "rcon" in hooks.CHECK_IDS
    # The report has to say what the hooks were NOT proven to do.
    assert "BOUND, not FIRED" in hooks.COVERAGE_NOTE


def test_the_catalogue_rule_states_the_limit_instead_of_pretending_there_is_none() -> None:
    """Build 42 leaves a few hundred script rows unnamed; a broken lookup leaves thousands."""
    named = [{"code": f"Base.Item{n}", "name": f"Item {n}"} for n in range(100)]
    unnamed = [{"code": f"Base.Wound_{n}", "name": f"Base.Wound_{n}"} for n in range(4)]
    spot = {"code": "Base.Axe", "name": "Firefighter Axe"}

    problems, detail = hooks._catalogue_problems([*named, *unnamed, spot], "items", ("Base.Axe", "Firefighter Axe"))

    assert problems == []
    assert detail["unnamed"]["count"] == 4
    assert detail["unnamed"]["examples"][0].startswith("Base.Wound_")
    assert detail["spotCheck"]["actual"] == "Firefighter Axe"

    # Every row named by its own id: the display-name lookup is broken, not the game's catalogue.
    broken = [{"code": row["code"], "name": row["code"]} for row in [*named, spot]]
    problems, _ = hooks._catalogue_problems(broken, "items", ("Base.Axe", "Firefighter Axe"))
    assert any("display-name lookup looks broken" in problem for problem in problems)
    assert any("Base.Axe" in problem for problem in problems)


def test_a_catalogue_entry_that_keeps_its_script_id_as_a_name_is_named_in_the_detail() -> None:
    problems, detail = hooks._catalogue_problems(
        [{"code": "Zombie", "name": "Zombie"}], "entities", ("Zombie", "Zombie")
    )

    assert problems == []
    assert detail["unnamed"]["count"] == 0, "a code without a dot is not a script id"


def test_runtime_identity_comes_from_the_target_check_line() -> None:
    adapter = adapter_for(GAME)
    pinned = "80e405a4bfc42f6072e75b3735f458a6514143da011d3226007ded305a442f44"

    def line(result: str, actual: str) -> str:
        payload = {
            "result": result,
            "policy": "enforce",
            "target": TARGET,
            "fingerprint": "f" * 64,
            "expected": {"gameJarSha256": pinned, "gameVersion": "42.20.4"},
            "runtime": {"gameJarSha256": actual, "agentVersion": "1.0.2", "java": "25.0.1"},
            "reasons": [],
        }
        return f"[2026-09-21 10:00:00.000] [Takaro] target-check: {json.dumps(payload)}"

    assert adapter.parse_runtime_identity(line("ok", pinned)) == {
        "gameVersion": "42.20.4",
        "loader": "javaagent",
        "loaderVersion": "1.0.2",
    }
    refused = adapter.parse_runtime_identity(line("refuse", "1" * 64))
    assert refused is not None and refused["gameVersion"] is None
    assert adapter.parse_runtime_identity("[Takaro] premain: hooks installed") is None


# --------------------------------------------------------------------------- 21 the release record


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
    resolved = resolve(run, repo, RELEASE_TARGET)
    directory = tmp_path / "dist" / RELEASE_TARGET
    jar = make_jar(directory / RELEASE_JAR, target=RELEASE_TARGET, fingerprint=resolved["fingerprint"])
    row = artifact_row("agent", RELEASE_TARGET, resolved["fingerprint"], jar)
    write_meta(directory, row, connector=GAME, version=VERSION, revision=commit)
    bundle = directory / RELEASE_BUNDLE
    with zipfile.ZipFile(bundle, "w") as archive:
        archive.writestr("Takaro/TakaroConnector.jar", jar.read_bytes())
        archive.writestr("Takaro/TakaroConfig.txt", "registrationToken=\nidentityToken=\n")
    bundle_row = artifact_row("bundle", RELEASE_TARGET, resolved["fingerprint"], bundle)
    write_meta(directory, bundle_row, connector=GAME, version=VERSION, revision=commit)
    write_manifest(
        directory,
        connector=GAME,
        version=VERSION,
        revision=commit,
        dirty=False,
        toolchain=resolved["build"]["toolchain"],
        mode="container",
        artifacts=[row, bundle_row],
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
    entry = record["targets"][RELEASE_TARGET]
    assert entry["verification"]["required"] == "contract"
    url = entry["inputs"]["server"]["url"]
    assert url.startswith("steam://app/380870/branch/public/")
    assert re.search(r"manifest/[0-9]+", url)
    assert (out / RELEASE_JAR).is_file()
    assert (out / RELEASE_BUNDLE).is_file()
    assert (out / f"TakaroConnector-{VERSION}.jar").read_bytes() == (out / RELEASE_JAR).read_bytes()
    assert (out / "SHA256SUMS").is_file()


# --------------------------------------------------------------------------- 22-23 the tracked scripts


def test_catalog_deps_match_the_gradle_pins() -> None:
    record = json.loads((REPO_ROOT / "catalog" / GAME / "targets" / f"{TARGET}.json").read_text())
    deps = record["build"]["deps"]
    wrapper = (REPO_ROOT / "games/zomboid/mod/gradle/wrapper/gradle-wrapper.properties").read_text()
    toml = (REPO_ROOT / "games/zomboid/mod/gradle/libs.versions.toml").read_text()

    assert f"distributionSha256Sum={deps['gradle']['sha256']}" in wrapper
    assert deps["gradle"]["coordinate"].rsplit(":", 1)[1] in wrapper
    for name, key in (("byte-buddy", "byte-buddy"), ("java-websocket", "java-websocket"), ("gson", "gson")):
        version = deps[name]["coordinate"].rsplit(":", 1)[1]
        assert re.search(rf'^{re.escape(key)} = "{re.escape(version)}"$', toml, re.MULTILINE), name
        assert deps[name]["resolvedCoordinate"].endswith(".jar")


def test_setup_environment_has_no_implicit_reference_sources() -> None:
    script = (REPO_ROOT / "games/zomboid/scripts/setup-environment.sh").read_text()

    for forbidden in ("docker exec", "dev-servers/_data", "app_update", "steamcmd", "EXPECTED_ZOMBOID_JAR_SHA256"):
        assert forbidden not in script, forbidden
    assert "WARNING" not in script
    assert "steam references" in script
    assert "exit 5" in script


# --------------------------------------------------------------------------- 24-25 the rig


DS_ROOT = REPO_ROOT / "dev-servers"
REGISTRY_ROW = "zomboid|zomboid.yml|-|zomboid|8|16|connector|Project Zomboid B42 + Takaro javaagent|zomboid"
SOURCE_PATHS = (
    "games/zomboid/mod/core games/zomboid/mod/agent games/zomboid/mod/gradle "
    "games/zomboid/mod/build.gradle.kts games/zomboid/mod/settings.gradle.kts games/zomboid/version.txt"
)


def bash(script: str) -> str:
    completed = subprocess.run(["bash", "-c", script], cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


def test_dev_servers_zomboid_dispatch_and_compose() -> None:
    for path in (DS_ROOT / "lib/games/zomboid.sh", REPO_ROOT / "games/zomboid/scripts/lib-target.sh"):
        completed = subprocess.run(["bash", "-n", str(path)], capture_output=True, text=True, check=False)
        assert completed.returncode == 0, f"{path.name}: {completed.stderr}"

    assert bash(". dev-servers/lib/common.sh; ds_target_prefix zomboid").strip() == "ZOMBOID"
    assert bash(". dev-servers/lib/common.sh; ds_target_dest zomboid").strip().endswith("/zomboid/server")
    assert bash(". dev-servers/lib/common.sh; ds_success_pattern_zomboid").strip() == "Identified successfully"

    compose = (DS_ROOT / "compose" / "zomboid.yml").read_text()
    assert ":latest" not in compose
    assert "${ZOMBOID_IMAGE" in compose
    expected = (FIXTURES / "compose.expected-mounts.txt").read_text().split()
    for mount in expected:
        assert f"- {mount}" in compose, mount
    assert "JAVA_TOOL_OPTIONS" in compose
    assert "/home/steam/ZomboidDedicatedServer/Takaro/TakaroConnector.jar" in compose


def test_the_registry_row_and_source_paths_are_unchanged() -> None:
    """The rig's registry row and source fingerprint are exactly what they were."""
    registry = bash(". dev-servers/lib/common.sh; ds_registry").splitlines()
    rows = [line for line in registry if line.startswith("zomboid|")]

    assert rows == [REGISTRY_ROW]
    assert bash(". dev-servers/lib/common.sh; ds_source_paths zomboid").strip() == SOURCE_PATHS


def _java_literal(relative: str, pattern: str) -> str:
    """The one literal `pattern` finds in an agent source file, or a loud failure."""
    source = (REPO_ROOT / "games/zomboid/mod/agent/src/main/java/io/takaro/zomboid/agent" / relative).read_text(
        encoding="utf-8"
    )
    found = re.findall(pattern, source)
    assert len(found) == 1, f"{relative}: {pattern!r} matched {len(found)} times, expected exactly one"
    return str(found[0])
