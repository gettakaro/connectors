"""The low-space ARK rig attests its base and writes only to its own tree."""

from __future__ import annotations

import asyncio
from pathlib import Path
from types import SimpleNamespace

import pytest

from takaro_maint import net
from takaro_maint.exit_codes import IntegrityError, UsageError
from takaro_maint.games.ark import verify as ark_verify
from takaro_maint.games.ark.readonly_base import _project_ban_list, project_base, validate_base
from takaro_maint.verify.hooks import GameHooks
from takaro_maint.verify.runner import RunOptions, TargetRun


def _base(tmp_path: Path) -> tuple[Path, SimpleNamespace]:
    base = tmp_path / "existing-base"
    exe = base / "ShooterGame/Binaries/Linux/ShooterGameServer"
    steamclient = base / "linux64/steamclient.so"
    exe.parent.mkdir(parents=True)
    steamclient.parent.mkdir(parents=True)
    exe.write_bytes(b"exact-ark-executable")
    steamclient.write_bytes(b"exact-steamworks-library")
    (base / "ShooterGame/Saved").mkdir()
    (base / "ShooterGame/Saved/live-world").write_text("must remain untouched")
    manifest = base / "steamapps/appmanifest_376030.acf"
    manifest.parent.mkdir()
    manifest.write_text(
        '"AppState" { "appid" "376030" "buildid" "21241282" '
        '"TargetBuildID" "21241282" "InstalledDepots" { '
        '"1006" { "manifest" "111" "size" "10" } '
        '"376031" { "manifest" "222" "size" "20" } } }'
    )
    target = SimpleNamespace(
        record={
            "inputs": {
                "server": {
                    "app": 376030,
                    "buildid": 21241282,
                    "depots": {"1006": {"manifest": "111", "size": 10}, "376031": {"manifest": "222", "size": 20}},
                    "files": {
                        "ShooterGame/Binaries/Linux/ShooterGameServer": net.hash_file(exe),
                        "linux64/steamclient.so": net.hash_file(steamclient),
                    },
                }
            }
        }
    )
    return base, target


def test_readonly_base_attests_build_depots_and_pinned_files(tmp_path: Path) -> None:
    base, target = _base(tmp_path)
    inputs, provenance = validate_base(base, target)
    assert {row["path"] for row in inputs} == {
        "ShooterGame/Binaries/Linux/ShooterGameServer",
        "linux64/steamclient.so",
    }
    assert provenance["readOnlyMount"] is True
    assert provenance["depotManifests"] == {"1006": "111", "376031": "222"}


def test_readonly_base_rejects_wrong_executable_hash(tmp_path: Path) -> None:
    base, target = _base(tmp_path)
    (base / "ShooterGame/Binaries/Linux/ShooterGameServer").write_bytes(b"wrong executable")
    with pytest.raises(IntegrityError, match="wrong hash or size"):
        validate_base(base, target)


@pytest.mark.parametrize(
    ("old", "new"),
    [('buildid" "21241282', 'buildid" "999'), ('manifest" "222', 'manifest" "999')],
)
def test_readonly_base_rejects_wrong_steam_manifest(tmp_path: Path, old: str, new: str) -> None:
    base, target = _base(tmp_path)
    manifest = base / "steamapps/appmanifest_376030.acf"
    manifest.write_text(manifest.read_text().replace(old, new))
    with pytest.raises(IntegrityError, match="build ID|manifest"):
        validate_base(base, target)


def test_readonly_projection_keeps_saved_and_connector_owned(tmp_path: Path) -> None:
    base, _ = _base(tmp_path)
    owned = tmp_path / "owned"
    owned.mkdir()
    project_base(base, owned)
    assert (owned / "ShooterGame/Saved").is_dir()
    assert not (owned / "ShooterGame/Saved").is_symlink()
    assert not (owned / "ShooterGame/Saved/live-world").exists()
    assert (base / "ShooterGame/Saved/live-world").read_text() == "must remain untouched"
    assert (owned / "TakaroArk").is_dir() and not (owned / "TakaroArk").is_symlink()
    assert (owned / ".takaro/home").is_dir()
    assert (owned / "ShooterGame/Binaries/Linux").is_dir()
    assert not (owned / "ShooterGame/Binaries/Linux").is_symlink()
    owned_exe = owned / "ShooterGame/Binaries/Linux/ShooterGameServer"
    assert owned_exe.is_file() and not owned_exe.is_symlink()
    assert owned_exe.read_bytes() == (base / "ShooterGame/Binaries/Linux/ShooterGameServer").read_bytes()
    owned_steamclient = owned / "linux64/steamclient.so"
    assert owned_steamclient.is_file() and not owned_steamclient.is_symlink()
    assert owned_steamclient.read_bytes() == (base / "linux64/steamclient.so").read_bytes()
    with pytest.raises(UsageError, match="must be separate"):
        project_base(base, base / "nested")


def test_readonly_projection_keeps_banlist_writable_and_preserves_it_on_restart(tmp_path: Path) -> None:
    base, _ = _base(tmp_path)
    source = base / "ShooterGame/Binaries/Linux/BanList.txt"
    source.write_text("76561198000000001\n")
    owned = tmp_path / "owned"
    owned.mkdir()
    project_base(base, owned)
    target = owned / "ShooterGame/Binaries/Linux/BanList.txt"
    assert target.is_file() and not target.is_symlink()
    assert target.read_text() == "76561198000000001\n"
    assert target.stat().st_mode & 0o200

    target.write_text("76561198009999999\n")
    source.write_text("76561198000000002\n")
    _project_ban_list(source, target)  # A second boot keeps the same owned file.
    assert target.read_text() == "76561198009999999\n"
    assert source.read_text() == "76561198000000002\n"


def test_readonly_projection_creates_owned_banlist_when_base_lacks_one(tmp_path: Path) -> None:
    base, _ = _base(tmp_path)
    owned = tmp_path / "owned"
    owned.mkdir()
    project_base(base, owned)
    target = owned / "ShooterGame/Binaries/Linux/BanList.txt"
    assert target.is_file() and not target.is_symlink() and target.read_bytes() == b""


def test_readonly_container_mounts_never_expose_existing_base_writable(tmp_path: Path) -> None:
    base, _ = _base(tmp_path)
    owned = tmp_path / "owned"
    options = RunOptions(artifacts=tmp_path, out=tmp_path, run_id="isolated", ark_readonly_base=base)
    run = object.__new__(TargetRun)
    run.options = options
    run.data_dir = owned
    assert run.container_mounts() == [f"{base}:/ark-base:ro", f"{owned}:/ark:rw"]

    run.target = SimpleNamespace(game="ark", id="linux-21241282")
    run.resolved = {"containerRef": "node@sha256:pinned", "runtime": {"container": {"env": {}}}}
    run.registration_token = "isolated-test-token"
    run.hooks = GameHooks()
    run.adapter = SimpleNamespace(
        runtime_env=lambda resolved, takaro: {},
        container_options=lambda resolved, data_dir: ["--memory", "24g"],
        container_command=lambda resolved, data_dir: ["/ark/TakaroArk/TakaroArkNative/launch.sh"],
    )
    argv = run.container_argv("ws://host.docker.internal:1234/")
    limits = [argv[index + 1] for index, token in enumerate(argv[:-1]) if token == "--memory"]
    assert limits == ["3g", "24g", "12g"]
    assert "-p" not in argv and "--publish" not in argv
    assert f"{base}:/ark-base:ro" in argv and f"{owned}:/ark:rw" in argv


@pytest.mark.parametrize(
    ("actual_exe", "expected"),
    [
        ("/ark/ShooterGame/Binaries/Linux/ShooterGameServer", "pass"),
        ("/ark-base/ShooterGame/Binaries/Linux/ShooterGameServer", "fail"),
    ],
)
def test_readonly_health_rejects_executable_resolving_to_preserved_base(
    monkeypatch: pytest.MonkeyPatch, actual_exe: str, expected: str
) -> None:
    monkeypatch.setattr(ark_verify, "start_sidecar", lambda run, fake: SimpleNamespace(name="sidecar"))

    async def healthy(*args: object) -> dict[str, object]:
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

    monkeypatch.setattr(
        ark_verify,
        "_wait_native_protocol_ready",
        healthy,
    )
    monkeypatch.setattr(
        ark_verify.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(
            returncode=0, stdout=f"game 7 {actual_exe}\nsaved /ark/ShooterGame/Saved\n", stderr=""
        ),
    )

    class FakeRun:
        target = SimpleNamespace(record={"revision": "21241282"})
        options = SimpleNamespace(ark_readonly_base=Path("/preserved"))
        container = SimpleNamespace(name="owned-ark")

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "native-health"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    asyncio.run(ark_verify.after_protocol(run, object(), lambda: True))
    assert run.results[0].status == expected


def test_readonly_stop_rejects_tampered_owned_executable(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    base, target = _base(tmp_path)
    owned = tmp_path / "owned"
    owned.mkdir()
    project_base(base, owned)
    (owned / "ShooterGame/Binaries/Linux/ShooterGameServer").write_bytes(b"tampered")
    monkeypatch.setattr(
        ark_verify.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(returncode=0, stderr=""),
    )

    class FakeRun:
        options = SimpleNamespace(ark_readonly_base=base)
        data_dir = owned
        container = SimpleNamespace(name="owned-ark")

        def __init__(self) -> None:
            self.results: list[object] = []

        def wanted(self, name: str) -> bool:
            return name == "stop"

        def record(self, result: object) -> None:
            self.results.append(result)

        def skip(self, name: str, reason: str) -> None:
            del name, reason

    run = FakeRun()
    inputs, _ = validate_base(base, target)
    asyncio.run(ark_verify.after_shutdown(run, object(), "", inputs))
    assert run.results[0].id == "stop"
    assert run.results[0].status == "fail"
    assert any("owned pinned input changed" in problem for problem in run.results[0].detail["problems"])
