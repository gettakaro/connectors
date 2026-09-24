"""ARK protocol level needs native evidence for each semantic capability."""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
from types import SimpleNamespace

import pytest

from takaro_maint import redact
from takaro_maint.catalog import schema
from takaro_maint.exit_codes import UpstreamUnavailable
from takaro_maint.games.ark import ArkAdapter
from takaro_maint.games.ark import verify as ark_verify
from takaro_maint.verify import runner as verify_runner
from takaro_maint.verify.checks import CheckResult
from takaro_maint.verify.report import GAME_PROTOCOL_CHECKS, build_report, level_for
from takaro_maint.verify.runner import _command_env_secrets, capture_ark_diagnostics


def _rows(status: str = "pass") -> list[dict[str, str]]:
    return [{"id": check_id, "status": status} for check_id in ("build", "startup", *GAME_PROTOCOL_CHECKS["ark"])]


def test_ark_protocol_level_requires_every_native_semantic_check() -> None:
    rows = _rows()
    assert level_for(rows, "ark") == "protocol"
    assert level_for(rows) == "startup"  # The Minecraft protocol ladder is unchanged.
    for required in GAME_PROTOCOL_CHECKS["ark"]:
        missing = [row for row in rows if row["id"] != required]
        assert level_for(missing, "ark") == "startup"
        skipped = [row | {"status": "skip"} if row["id"] == required else row for row in rows]
        assert level_for(skipped, "ark") == "startup"


def test_ark_verification_uses_init_and_redacts_derived_native_token(tmp_path: Path) -> None:
    assert "--init" in ArkAdapter().container_options({}, tmp_path)
    command = ["docker", "run", "-e", "ARK_NATIVE_TOKEN=ephemeral-native-token", "-e", "LEVEL=TheIsland"]
    secrets = _command_env_secrets(command)
    assert "ephemeral-native-token" in secrets
    logged = redact.redact(" ".join(command), secrets)
    assert "ephemeral-native-token" not in logged
    assert "ARK_NATIVE_TOKEN=<redacted>" in logged


def test_game_container_error_and_docker_log_redact_native_token(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    command = ["docker", "run", "-e", "ARK_NATIVE_TOKEN=ephemeral-native-token", "image"]
    container = verify_runner.Container(
        name="isolated",
        argv=command,
        log_file=tmp_path / "server.log",
        docker_log=tmp_path / "docker.log",
        secrets=_command_env_secrets(command),
    )
    monkeypatch.setattr(
        verify_runner.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(returncode=1, stderr="bad ephemeral-native-token"),
    )
    with pytest.raises(UpstreamUnavailable, match="bad <redacted>"):
        container.start()
    assert "ephemeral-native-token" not in (tmp_path / "docker.log").read_text()


@pytest.mark.parametrize(
    ("game_lines", "valid"),
    [
        (["game 7 /ark/ShooterGame/Binaries/Linux/ShooterGameServer"], True),
        (["game 1 /ark/ShooterGame/Binaries/Linux/ShooterGameServer"], False),
        (["game 7 /ark-base/ShooterGame/Binaries/Linux/ShooterGameServer"], False),
        (
            [
                "game 7 /ark/ShooterGame/Binaries/Linux/ShooterGameServer",
                "game 8 /ark/ShooterGame/Binaries/Linux/ShooterGameServer",
            ],
            False,
        ),
        ([], False),
    ],
)
def test_owned_runtime_path_probe_handles_init_pid_and_rejects_ambiguity(game_lines: list[str], valid: bool) -> None:
    assert "/proc/[0-9]*/exe" in ark_verify._OWNED_PATH_PROBE_SCRIPT
    assert ark_verify._owned_runtime_paths_valid([*game_lines, "saved /ark/ShooterGame/Saved"]) is valid
    assert ark_verify._owned_runtime_paths_valid([*game_lines, "saved /ark-base/ShooterGame/Saved"]) is False


def test_ark_report_keeps_artifact_and_runner_revisions_separate(tmp_path: Path) -> None:
    target = SimpleNamespace(
        game="ark",
        id="pinned-ark",
        fingerprint="a" * 64,
        record={
            "inputs": {},
            "runtime": {"container": {"image": "server", "tag": "pinned", "digest": "sha256:" + "b" * 64}},
        },
    )
    report = build_report(
        target=target,
        game_record={},
        manifest={"version": "1.0", "sourceRevision": "artifact-commit", "dirty": False, "artifacts": []},
        artifacts_dir=tmp_path,
        runtime={"readOnlyBase": {"readOnlyMount": True, "steamBuild": "21241282"}},
        checks=_rows(),
        started_at="2026-09-24T00:00:00Z",
        logs=[],
        repo_root=tmp_path,
    )
    assert report["source"] == {"repo": "unknown", "revision": "artifact-commit", "dirty": False}
    assert report["coverage"]["verificationRunner"] == {"revision": "unknown", "dirty": True}
    assert report["coverage"]["readOnlyBase"] == {"readOnlyMount": True, "steamBuild": "21241282"}


def test_ark_readonly_report_provenance_validates_against_published_schema(tmp_path: Path) -> None:
    target = SimpleNamespace(
        game="ark",
        id="pinned-ark",
        fingerprint="a" * 64,
        record={
            "inputs": {},
            "runtime": {"container": {"image": "server", "tag": "pinned", "digest": "sha256:" + "b" * 64}},
        },
    )
    provenance = {
        "path": str(tmp_path),
        "readOnlyMount": True,
        "steamApp": "376030",
        "steamBuild": "21241282",
        "depotManifests": {"376031": "6366771435093287465"},
        "appManifestSha256": "c" * 64,
    }
    report = build_report(
        target=target,
        game_record={},
        manifest={"version": "1.0", "sourceRevision": "artifact-commit", "dirty": False, "artifacts": []},
        artifacts_dir=tmp_path,
        runtime={"readOnlyBase": provenance},
        checks=[{**row, "durationMs": 0, "detail": {}} for row in _rows()],
        started_at="2026-09-24T00:00:00Z",
        logs=[],
        repo_root=tmp_path,
    )
    assert schema.errors_for("verify-report.schema.json", report) == []
    report["coverage"]["readOnlyBase"]["readOnlyMount"] = False
    assert any("readOnlyMount" in error for error in schema.errors_for("verify-report.schema.json", report))


def test_sidecar_build_uses_packaged_dockerfile_outside_repository_cwd(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    data = tmp_path / "data"
    out = tmp_path / "out"
    out.mkdir()
    source = data / ark_verify.SIDECAR_FOLDER
    (source / "dist").mkdir(parents=True)
    for name in ("Dockerfile", "dist/index.js", "package-lock.json"):
        (source / name).write_text("fixture", encoding="utf-8")
    calls: list[list[str]] = []

    def failed_build(argv: list[str], **kwargs: object) -> SimpleNamespace:
        del kwargs
        calls.append(argv)
        return SimpleNamespace(returncode=1, stdout="", stderr="fixture build refusal")

    monkeypatch.setattr(ark_verify.subprocess, "run", failed_build)
    monkeypatch.setattr(ark_verify, "docker_command", lambda: ["docker"])
    run = SimpleNamespace(data_dir=data, out=out, options=SimpleNamespace(run_id="fixture"))
    with pytest.raises(RuntimeError, match="sidecar image build failed"):
        ark_verify.start_sidecar(run, None)
    assert calls[0] == [
        "docker",
        "build",
        "-f",
        str(source / "Dockerfile"),
        "-t",
        "takaro-ark-sidecar:tm-fixture",
        "--label",
        "tm.run=fixture",
        str(source),
    ]


def test_ark_cleanup_keeps_exit_state_timestamped_log_and_owned_crash_context(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    data = tmp_path / "owned"
    out = tmp_path / "report"
    out.mkdir()
    crash = data / "ShooterGame/Saved/Crashes/CrashContext.runtime-xml"
    crash.parent.mkdir(parents=True)
    crash.write_text("<CrashContext>owned rig</CrashContext>", encoding="utf-8")
    world = data / "ShooterGame/Saved/SavedArks/TheIsland.ark"
    world.parent.mkdir(parents=True)
    world.write_bytes(b"fresh-world")
    calls: list[list[str]] = []

    def docker_result(argv: list[str], **kwargs: object) -> SimpleNamespace:
        del kwargs
        calls.append(argv)
        if "inspect" in argv:
            return SimpleNamespace(returncode=0, stdout='{"Running":false,"ExitCode":139}', stderr="")
        return SimpleNamespace(returncode=0, stdout="2026-09-24T09:00:00Z Signal 11 caught\n", stderr="")

    monkeypatch.setattr("takaro_maint.verify.runner.subprocess.run", docker_result)
    monkeypatch.setattr("takaro_maint.verify.runner.docker_command", lambda: ["docker"])
    container = SimpleNamespace(name="owned-ark", secrets=[])
    files = capture_ark_diagnostics([container], data, out)
    assert out / "ark-cleanup-diagnostics.json" in files
    assert (out / "owned-ark-timestamped.log").read_text() == "2026-09-24T09:00:00Z Signal 11 caught\n"
    assert (out / "owned-saved-diagnostics/Crashes/CrashContext.runtime-xml").read_text() == crash.read_text()
    evidence = json.loads((out / "ark-cleanup-diagnostics.json").read_text())
    assert evidence["containers"][0]["state"]["ExitCode"] == 139
    assert {row["file"] for row in evidence["ownedSavedFiles"]} == {
        "Crashes/CrashContext.runtime-xml",
        "SavedArks/TheIsland.ark",
    }
    assert calls == [
        ["docker", "inspect", "-f", "{{json .State}}", "owned-ark"],
        ["docker", "logs", "--timestamps", "--tail", "10000", "owned-ark"],
    ]


def test_native_health_waits_for_zero_player_protocol_without_client_chat(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    starting = {
        "status": "starting",
        "build": "21241282",
        "bootId": "fresh",
        "capabilities": {
            "chat": "starting",
            "sendMessage": "starting",
            "roster": "starting",
            "items": "unavailable",
            "entities": "unavailable",
        },
    }
    ready = {
        **starting,
        "status": "ok",
        "capabilities": {**starting["capabilities"], "roster": "ok", "items": "ok", "entities": "ok"},
    }
    responses = iter((starting, ready))
    monkeypatch.setattr(ark_verify, "_get_json", lambda *args, **kwargs: next(responses))
    result = asyncio.run(ark_verify._wait_native_protocol_ready("sidecar", "21241282", lambda: True, poll_interval=0))
    assert result == ready
    assert ark_verify._native_protocol_ready(result, "21241282")
    assert not ark_verify._native_protocol_ready(result, "wrong-build")


def test_native_health_timeout_keeps_starting_result(monkeypatch: pytest.MonkeyPatch) -> None:
    starting = {"status": "starting", "build": "21241282", "bootId": "fresh", "capabilities": {}}
    monkeypatch.setattr(ark_verify, "_get_json", lambda *args, **kwargs: starting)
    result = asyncio.run(ark_verify._wait_native_protocol_ready("sidecar", "21241282", lambda: True, timeout=0))
    assert result == starting
    assert not ark_verify._native_protocol_ready(result, "21241282")


def test_native_health_wrong_build_fails_without_waiting(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = 0

    def wrong_build(*args: object, **kwargs: object) -> dict[str, object]:
        nonlocal calls
        del args, kwargs
        calls += 1
        return {"status": "ok", "build": "wrong-build", "bootId": "fresh", "capabilities": {}}

    monkeypatch.setattr(ark_verify, "_get_json", wrong_build)
    result = asyncio.run(ark_verify._wait_native_protocol_ready("sidecar", "21241282", lambda: True))
    assert result["build"] == "wrong-build"
    assert calls == 1
    assert not ark_verify._native_protocol_ready(result, "21241282")


def test_ark_native_health_passes_with_client_dependent_chat_starting(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))

    async def ready(*args: object) -> dict[str, object]:
        del args
        return {
            "status": "ok",
            "build": "21241282",
            "bootId": "fresh",
            "capabilities": {
                "chat": "starting",
                "sendMessage": "starting",
                "roster": "ok",
                "items": "ok",
                "entities": "ok",
            },
        }

    monkeypatch.setattr(ark_verify, "_wait_native_protocol_ready", ready)

    class FakeRun:
        options = SimpleNamespace(ark_readonly_base=None)
        target = SimpleNamespace(record={"revision": "21241282"})

        def __init__(self) -> None:
            self.results: list[CheckResult] = []

        def wanted(self, name: str) -> bool:
            return name == "native-health"

        def record(self, result: CheckResult) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, object(), lambda: True))
    assert run.results[0].status == "pass"
    assert run.results[0].detail["clientDependentCapabilities"] == ["chat", "sendMessage"]


def test_ark_heartbeat_detail_problems_do_not_crash_report(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))

    async def heartbeat(fake: object) -> CheckResult:
        del fake
        return CheckResult("heartbeat", "fail", 0, {"pingRoundTripMs": [], "problems": ["no pong"]})

    monkeypatch.setattr(ark_verify.checks, "check_heartbeat", heartbeat)

    class FakeRun:
        def __init__(self) -> None:
            self.results: list[CheckResult] = []

        def wanted(self, name: str) -> bool:
            return name == "ark-heartbeat"

        def record(self, result: CheckResult) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, object(), lambda: True))
    assert run.results[0].status == "fail"
    assert run.results[0].detail == {"pingRoundTripMs": [], "problems": ["no pong"]}


@pytest.mark.parametrize(
    ("ack", "exit_code", "expected"),
    [({}, 134, "pass"), ({"success": True}, 134, "fail"), ({}, 0, "fail"), ({}, 139, "fail")],
)
def test_ark_native_shutdown_requires_sidecar_ack_and_clean_native_exit(
    tmp_path: Path, ack: dict[str, object], exit_code: int, expected: str
) -> None:
    log = tmp_path / "server.log"
    log.write_text("", encoding="utf-8")

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> dict[str, object]:
            assert (name, args) == ("shutdown", {})
            with log.open("a", encoding="utf-8") as stream:
                stream.write(
                    "ARK_NATIVE_DIAG native-shutdown-synchronous-save-completed-before-ack\n"
                    "ARK_NATIVE_SHUTDOWN native-exit-requested\n"
                )
            return ack

    class FakeRun:
        container = SimpleNamespace(wait_for_exit=lambda timeout: exit_code)
        server_log = log

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "native-shutdown"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_shutdown(run, FakeTakaro(), "", []))
    assert len(run.results) == 1
    assert run.results[0].id == "native-shutdown"
    assert run.results[0].status == expected, run.results[0].detail


def test_ark_native_shutdown_rejects_unacknowledged_request(tmp_path: Path) -> None:
    log = tmp_path / "server.log"
    log.write_text("", encoding="utf-8")

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> None:
            del name, args
            raise TimeoutError("native response was not acknowledged")

    class FakeRun:
        container = SimpleNamespace(wait_for_exit=lambda timeout: 0)
        server_log = log

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "native-shutdown"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_shutdown(run, FakeTakaro(), "", []))
    assert run.results[0].status == "fail"
    assert "not acknowledged" in run.results[0].detail["problems"][0]


def test_ark_shutdown_rejects_unattributed_abort(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    log = tmp_path / "server.log"
    log.write_text(
        "ARK_NATIVE_DIAG native-shutdown-synchronous-save-completed-before-ack\n"
        "ARK_NATIVE_SHUTDOWN native-exit-requested\n"
    )
    monkeypatch.setattr(ark_verify, "SHUTDOWN_MARKER_TIMEOUT", 0)

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> dict[str, object]:
            del name, args
            return {}

    class FakeRun:
        container = SimpleNamespace(wait_for_exit=lambda timeout: 134)
        server_log = log

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "native-shutdown"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_shutdown(run, FakeTakaro(), "", []))
    assert run.results[0].status == "fail"
    assert "native shutdown save completion marker is missing" in run.results[0].detail["problems"]
    assert "native shutdown request completion marker is missing" in run.results[0].detail["problems"]


@pytest.mark.parametrize("old_marker", ("engine-exit-handled", "forced-exit-requested"))
def test_ark_shutdown_rejects_old_exit_marker(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, old_marker: str) -> None:
    log = tmp_path / "server.log"
    log.write_text("", encoding="utf-8")
    monkeypatch.setattr(ark_verify, "SHUTDOWN_MARKER_TIMEOUT", 0)

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> dict[str, object]:
            assert (name, args) == ("shutdown", {})
            with log.open("a", encoding="utf-8") as stream:
                stream.write(
                    "ARK_NATIVE_DIAG native-shutdown-synchronous-save-completed-before-ack\n"
                    f"ARK_NATIVE_SHUTDOWN {old_marker}\n"
                )
            return {}

    class FakeRun:
        container = SimpleNamespace(wait_for_exit=lambda timeout: 134)
        server_log = log

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "native-shutdown"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_shutdown(run, FakeTakaro(), "", []))
    assert run.results[0].status == "fail"
    assert "native shutdown request completion marker is missing" in run.results[0].detail["problems"]


def test_ark_shutdown_log_rotation_fails_closed(tmp_path: Path) -> None:
    log = tmp_path / "server.log"
    log.write_text("before shutdown\n")
    before = log.stat()
    log.rename(tmp_path / "server.log.1")
    log.write_text(
        "ARK_NATIVE_DIAG native-shutdown-synchronous-save-completed-before-ack\n"
        "ARK_NATIVE_SHUTDOWN native-exit-requested\n"
    )
    with pytest.raises(RuntimeError, match="rotated or truncated"):
        ark_verify._fresh_shutdown_markers(log, before.st_dev, before.st_ino, before.st_size)


@pytest.mark.parametrize(("write_save", "expected"), [(True, "pass"), (False, "fail")])
@pytest.mark.parametrize("read_only", (True, False))
def test_ark_shutdown_requires_fresh_owned_save(
    tmp_path: Path, write_save: bool, expected: str, read_only: bool
) -> None:
    log = tmp_path / "server.log"
    log.write_text("")
    save = tmp_path / "ShooterGame/Saved/SavedArks/TheIsland.ark"

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> dict[str, object]:
            assert (name, args) == ("shutdown", {})
            with log.open("a") as stream:
                stream.write(
                    "ARK_NATIVE_DIAG native-shutdown-synchronous-save-completed-before-ack\n"
                    "ARK_NATIVE_SHUTDOWN native-exit-requested\n"
                )
            if write_save:
                save.parent.mkdir(parents=True)
                save.write_bytes(b"fresh-world")
            return {}

    class FakeRun:
        container = SimpleNamespace(wait_for_exit=lambda timeout: 134)
        server_log = log
        data_dir = tmp_path
        options = SimpleNamespace(ark_readonly_base=tmp_path / "base" if read_only else None)

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "native-shutdown"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_shutdown(run, FakeTakaro(), "", []))
    assert run.results[0].status == expected, run.results[0].detail


def test_owned_save_watch_requires_an_actual_read(tmp_path: Path) -> None:
    save = tmp_path / "TheIsland.ark"
    save.write_bytes(b"saved world")
    watch = ark_verify._OwnedSaveReadWatch(save)
    try:
        assert watch.read_evidence()["openedReadClosed"] is False
        assert save.read_bytes() == b"saved world"
        assert watch.read_evidence()["openedReadClosed"] is True
    finally:
        watch.close()


def test_owned_save_watch_rejects_replaced_inode(tmp_path: Path) -> None:
    save = tmp_path / "TheIsland.ark"
    save.write_bytes(b"first world")
    watch = ark_verify._OwnedSaveReadWatch(save)
    try:
        save.unlink()
        save.write_bytes(b"replacement")
        assert save.read_bytes() == b"replacement"
        evidence = watch.read_evidence()
        assert evidence["invalidated"] is True
        assert evidence["openedReadClosed"] is False
    finally:
        watch.close()


@pytest.mark.parametrize(
    ("read_save", "second_boot_id", "second_exit", "read_only", "expected"),
    [
        (True, "boot-two", 134, True, "pass"),
        (True, "boot-two", 134, False, "pass"),
        (False, "boot-two", 134, True, "fail"),
        (True, "boot-one", 134, True, "fail"),
        (True, "boot-two", 139, True, "fail"),
    ],
)
def test_owned_save_reload_requires_same_world_read_new_boot_and_second_native_exit(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    read_save: bool,
    second_boot_id: str,
    second_exit: int,
    read_only: bool,
    expected: str,
) -> None:
    save = tmp_path / "ShooterGame/Saved/SavedArks/TheIsland.ark"
    save.parent.mkdir(parents=True)
    save.write_bytes(b"saved world from first boot")
    reload_log = tmp_path / "server-reload.log"
    old_sidecar = SimpleNamespace(name="first-sidecar", removed=False)

    def remove_old_sidecar() -> None:
        old_sidecar.removed = True

    old_sidecar.remove = remove_old_sidecar
    second = SimpleNamespace(name="second", alive=lambda: True, wait_for_exit=lambda timeout: second_exit)

    class FakeRun:
        out = tmp_path
        data_dir = tmp_path
        target = SimpleNamespace(record={"revision": "21241282"})
        startup_timeout = 5

        def __init__(self) -> None:
            self.options = SimpleNamespace(ark_readonly_base=tmp_path / "read-only-base" if read_only else None)
            self.container = SimpleNamespace(name="first")
            self.containers = [old_sidecar]
            self.extra_logs: list[Path] = []
            self.results = [
                CheckResult("native-shutdown", "pass", 0, {}),
                CheckResult("native-health", "pass", 0, {"health": {"bootId": "boot-one"}}),
            ]

        def boot(self, ws_url: str, *, suffix: str, log_name: str) -> SimpleNamespace:
            assert ws_url == "ws://isolated/"
            assert suffix == "-reload" and log_name == "server-reload.log"
            assert old_sidecar.removed
            if read_save:
                assert save.read_bytes() == b"saved world from first boot"
            reload_log.write_text("ARK_NATIVE_DIAG main-loop-tick count=1\n")
            self.container = second
            return second

        def ready_line(self) -> object:
            return ark_verify.READY_LINE

        def record(self, result: CheckResult) -> None:
            self.results.append(result)

    class FakeTakaro:
        identify_count = 1

        async def wait_for_identify(self, timeout: float, *, minimum: int) -> None:
            assert timeout == 120 and minimum == 2
            self.identify_count = 2

        async def request(self, name: str, args: dict[str, object]) -> dict[str, object]:
            assert (name, args) == ("shutdown", {})
            with reload_log.open("a") as stream:
                stream.write(
                    "ARK_NATIVE_DIAG native-shutdown-synchronous-save-completed-before-ack\n"
                    "ARK_NATIVE_SHUTDOWN native-exit-requested\n"
                )
            return {}

    async def reload_health(container: str, build: str, alive: object) -> dict[str, object]:
        assert (container, build, alive()) == ("second-sidecar", "21241282", True)
        return {
            "status": "ok",
            "build": build,
            "bootId": second_boot_id,
            "capabilities": {"roster": "ok", "items": "ok", "entities": "ok"},
        }

    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake, **kwargs: SimpleNamespace(name="second-sidecar"))
    monkeypatch.setattr(ark_verify, "_wait_native_protocol_ready", reload_health)
    monkeypatch.setattr(
        ark_verify.checks,
        "check_startup",
        lambda *args: CheckResult("startup", "pass", 0, {"readyLine": "main-loop-tick count=1"}),
    )
    run = FakeRun()
    asyncio.run(ark_verify._reload_owned_save(run, FakeTakaro(), "ws://isolated/", []))
    result = run.results[-1]
    assert result.id == "owned-save-reload"
    assert result.status == expected, result.detail
    if expected == "pass":
        assert result.detail["readEvidence"]["openedReadClosed"] is True
        assert result.detail["firstBootId"] == "boot-one"
        assert result.detail["secondBootId"] == "boot-two"
        assert result.detail["reloadExitCode"] == 134
        assert run.extra_logs == [reload_log]


@pytest.mark.parametrize(
    ("entities", "expected"),
    [([{"code": "/Game/Dino_C", "name": "Dino"}], "pass"), ([], "fail")],
)
def test_ark_entity_check_queries_real_generic_catalog(
    monkeypatch: pytest.MonkeyPatch, entities: list[dict[str, str]], expected: str
) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> list[dict[str, str]]:
            assert (name, args) == ("listEntities", {})
            return entities

    class FakeRun:
        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "entities"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, FakeTakaro(), lambda: True))
    assert len(run.results) == 1
    assert run.results[0].id == "entities"
    assert run.results[0].status == expected


@pytest.mark.parametrize(
    ("result", "rejection_status", "expected"),
    [
        ({"success": True, "rawResult": "", "errorMessage": None}, 400, "pass"),
        ({"success": False, "rawResult": "", "errorMessage": "unhandled"}, 400, "fail"),
        ({"success": True, "rawResult": None, "errorMessage": None}, 400, "fail"),
        ({"success": True, "rawResult": "", "errorMessage": None}, 200, "fail"),
    ],
)
def test_ark_console_requires_native_handled_result(
    monkeypatch: pytest.MonkeyPatch, result: dict[str, object], rejection_status: int, expected: str
) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))
    monkeypatch.setattr(ark_verify, "_get_json", lambda *args, **kwargs: {"bootId": "same-boot"})

    def reject(container: str, command: str) -> dict[str, object]:
        assert container == "sidecar"
        expected_error = dict(ark_verify.CONSOLE_SAFETY_PROBES)[command]
        return {
            "status": rejection_status,
            "body": {"success": False, "rawResult": "", "errorMessage": expected_error},
        }

    monkeypatch.setattr(ark_verify, "_native_console_probe", reject)

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> dict[str, object]:
            assert (name, args) == (
                "executeConsoleCommand",
                {"command": "GetAll ShooterPlayerState PlayerName"},
            )
            return result

    class FakeRun:
        container = SimpleNamespace(alive=lambda: True)

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "ark-console"

        def record(self, check: object) -> None:
            self.results.append(check)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, FakeTakaro(), lambda: True))
    assert len(run.results) == 1
    assert run.results[0].id == "ark-console"
    assert run.results[0].status == expected
    assert len(run.results[0].detail["safetyProbes"]) == len(ark_verify.CONSOLE_SAFETY_PROBES)


def test_native_console_safety_probe_reads_structured_http_400_without_token_in_argv(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[list[str]] = []

    def native_response(argv: list[str], **kwargs: object) -> SimpleNamespace:
        del kwargs
        calls.append(argv)
        return SimpleNamespace(
            returncode=0,
            stdout=(
                '{"status":400,"body":{"success":false,"rawResult":"",'
                '"errorMessage":"Use the dedicated shutdown action"}}'
            ),
            stderr="",
        )

    monkeypatch.setattr(ark_verify.subprocess, "run", native_response)
    monkeypatch.setattr(ark_verify, "docker_command", lambda: ["docker"])
    result = ark_verify._native_console_probe("owned-sidecar", "DoExit")
    assert result["status"] == 400
    assert result["body"]["errorMessage"] == "Use the dedicated shutdown action"
    assert calls[0][:4] == ["docker", "exec", "owned-sidecar", "node"]
    assert calls[0][-1] == "DoExit"
    assert "ARK_NATIVE_TOKEN" in calls[0][-2]
    assert all("Bearer fixture-secret" not in part for part in calls[0])


def test_native_message_probe_reads_real_status_without_token_in_argv(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[list[str]] = []

    def native_response(argv: list[str], **kwargs: object) -> SimpleNamespace:
        del kwargs
        calls.append(argv)
        return SimpleNamespace(returncode=0, stdout='{"status":503,"body":{"error":"offline"}}', stderr="")

    monkeypatch.setattr(ark_verify.subprocess, "run", native_response)
    monkeypatch.setattr(ark_verify, "docker_command", lambda: ["docker"])
    result = ark_verify._native_message_probe("owned-sidecar", "/players/76561198009999999/message", "fixture")
    assert result == {"status": 503, "body": {"error": "offline"}}
    assert calls[0][:4] == ["docker", "exec", "owned-sidecar", "node"]
    assert calls[0][-2:] == ["/players/76561198009999999/message", "fixture"]
    assert "process.env.ARK_NATIVE_TOKEN" in calls[0][-3]
    assert all("fixture-secret" not in part for part in calls[0])


@pytest.mark.parametrize("native_status", [200, 503])
def test_empty_broadcast_requires_native_and_generic_ack(monkeypatch: pytest.MonkeyPatch, native_status: int) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))
    monkeypatch.setattr(ark_verify, "_get_json", lambda *args, **kwargs: [])
    monkeypatch.setattr(
        ark_verify,
        "_native_message_probe",
        lambda container, path, message: {"status": native_status, "body": {"success": native_status == 200}},
    )

    class FakeTakaro:
        async def request(self, action: str, args: dict[str, object]) -> object:
            if action == "getPlayers":
                assert args == {}
                return []
            assert action == "sendMessage" and "empty broadcast" in str(args["message"])
            return {}

    class FakeRun:
        options = SimpleNamespace(run_id="owned-empty")

        def __init__(self) -> None:
            self.results: list[CheckResult] = []

        def wanted(self, name: str) -> bool:
            return name == "ark-empty-broadcast"

        def record(self, result: CheckResult) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, FakeTakaro(), lambda: True))
    assert len(run.results) == 1
    assert run.results[0].status == ("pass" if native_status == 200 else "fail")


@pytest.mark.parametrize("native_status", [503, 200])
def test_offline_targeted_message_must_still_fail(monkeypatch: pytest.MonkeyPatch, native_status: int) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))
    monkeypatch.setattr(ark_verify, "_get_json", lambda *args, **kwargs: [])
    monkeypatch.setattr(
        ark_verify,
        "_native_message_probe",
        lambda container, path, message: {
            "status": native_status,
            "body": {"error": "no native recipient or queue full"} if native_status == 503 else {"success": True},
        },
    )

    class FakeRun:
        def __init__(self) -> None:
            self.results: list[CheckResult] = []

        def wanted(self, name: str) -> bool:
            return name == "ark-targeted-offline"

        def record(self, result: CheckResult) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, object(), lambda: True))
    assert len(run.results) == 1
    assert run.results[0].status == ("pass" if native_status == 503 else "fail")


def test_saveworld_requires_fresh_ordered_native_log_events() -> None:
    fake = SimpleNamespace(
        events=[
            {"type": "log", "data": {"msg": "Saving world..."}},
            {"type": "log", "data": {"msg": "World Save Complete. Took 0.1"}},
        ]
    )
    assert asyncio.run(ark_verify._fresh_saveworld_events(fake, 2, timeout=0)) == {
        "start": None,
        "complete": None,
    }
    fake.events += [
        {"type": "log", "data": {"msg": "World Save Complete. Took 0.2"}},
        {"type": "log", "data": {"msg": "Saving world..."}},
        {"type": "log", "data": {"msg": "World Save Complete. Took 0.3"}},
    ]
    markers = asyncio.run(ark_verify._fresh_saveworld_events(fake, 2, timeout=0))
    assert markers["start"] == "Saving world..."
    assert markers["complete"] == "World Save Complete. Took 0.3"


@pytest.mark.parametrize("handled", [True, False])
def test_saveworld_check_needs_handled_command_and_owned_file_update(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, handled: bool
) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))
    monkeypatch.setattr(ark_verify, "_get_json", lambda *args, **kwargs: {"bootId": "same-boot"})
    original_events = ark_verify._fresh_saveworld_events

    async def fresh_events(fake: object, baseline: int) -> dict[str, str | None]:
        return await original_events(fake, baseline, timeout=0)

    monkeypatch.setattr(ark_verify, "_fresh_saveworld_events", fresh_events)
    saved = tmp_path / "ShooterGame/Saved/SavedArks/TheIsland.ark"
    saved.parent.mkdir(parents=True)
    saved.write_bytes(b"old")
    os.utime(saved, ns=(1, 1))
    log = tmp_path / "server.log"
    log.write_text("booted\n")

    class FakeTakaro:
        def __init__(self) -> None:
            self.events: list[dict[str, object]] = [
                {"type": "log", "data": {"msg": "Saving world..."}},
                {"type": "log", "data": {"msg": "World Save Complete. Took 0.1"}},
            ]

        async def request(self, action: str, args: dict[str, object]) -> dict[str, object]:
            assert (action, args) == ("executeConsoleCommand", {"command": "SaveWorld"})
            if handled:
                saved.write_bytes(b"new-world")
                self.events.extend(
                    [
                        {"type": "log", "data": {"msg": "Saving world..."}},
                        {"type": "log", "data": {"msg": "World Save Complete. Took 0.3"}},
                    ]
                )
            return {"success": handled, "rawResult": "", "errorMessage": None if handled else "unhandled"}

    class FakeRun:
        container = SimpleNamespace(alive=lambda: True)
        data_dir = tmp_path
        server_log = log

        def __init__(self) -> None:
            self.results: list[CheckResult] = []

        def wanted(self, name: str) -> bool:
            return name == "ark-saveworld"

        def record(self, result: CheckResult) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, FakeTakaro(), lambda: True))
    assert len(run.results) == 1
    assert run.results[0].status == ("pass" if handled else "fail"), run.results[0].detail


def test_ark_console_safety_probe_fails_if_native_boot_changes(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))
    health_calls = 0

    def health(*args: object, **kwargs: object) -> dict[str, str]:
        nonlocal health_calls
        del args, kwargs
        health_calls += 1
        return {"bootId": "before" if health_calls == 1 else "different"}

    monkeypatch.setattr(ark_verify, "_get_json", health)
    monkeypatch.setattr(
        ark_verify,
        "_native_console_probe",
        lambda container, command: {
            "status": 400,
            "body": {
                "success": False,
                "rawResult": "",
                "errorMessage": dict(ark_verify.CONSOLE_SAFETY_PROBES)[command],
            },
        },
    )

    class FakeTakaro:
        async def request(self, name: str, args: dict[str, object]) -> dict[str, object]:
            del name, args
            return {"success": True, "rawResult": "", "errorMessage": None}

    class FakeRun:
        container = SimpleNamespace(alive=lambda: True)

        def __init__(self) -> None:
            self.results: list[CheckResult] = []

        def wanted(self, name: str) -> bool:
            return name == "ark-console"

        def record(self, result: CheckResult) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, FakeTakaro(), lambda: True))
    assert run.results[0].status == "fail"
    assert any("native boot changed" in problem for problem in run.results[0].detail["problems"])


def test_ark_shutdown_only_selection_starts_sidecar(monkeypatch: pytest.MonkeyPatch) -> None:
    started: list[bool] = []
    monkeypatch.setattr(
        ark_verify,
        "start_sidecar",
        lambda run, fake: started.append(True) or SimpleNamespace(name="sidecar"),
    )

    class FakeRun:
        def wanted(self, name: str) -> bool:
            return name == "native-shutdown"

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    asyncio.run(ark_verify.after_protocol(FakeRun(), object(), lambda: True))
    assert started == [True]
