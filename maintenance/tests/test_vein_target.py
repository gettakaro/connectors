from __future__ import annotations

import io
import tarfile
from pathlib import Path

import pytest

from takaro_maint.exit_codes import ConflictError
from takaro_maint.games.vein import GAME


def _archive(path: Path, name: str, content: bytes = b"plugin") -> None:
    with tarfile.open(path, "w:gz") as archive:
        info = tarfile.TarInfo(name)
        info.size = len(content)
        archive.addfile(info, io.BytesIO(content))


def test_vein_plugin_deploy_replaces_only_package(tmp_path: Path) -> None:
    artifact = tmp_path / "plugin.tar.gz"
    _archive(artifact, "TakaroVein/libtakaro-vein.so")
    installed = tmp_path / "takaro/TakaroVein"
    installed.mkdir(parents=True)
    (installed / "libtakaro-vein.so").write_bytes(b"old")
    state = tmp_path / "Vein/Saved/world.dat"
    state.parent.mkdir(parents=True)
    state.write_bytes(b"world")

    GAME.after_deploy(tmp_path, {"role": "plugin", "installDir": "takaro"}, artifact)

    assert (installed / "libtakaro-vein.so").read_bytes() == b"plugin"
    assert state.read_bytes() == b"world"


def test_vein_plugin_deploy_rejects_unexpected_archive_entry(tmp_path: Path) -> None:
    artifact = tmp_path / "plugin.tar.gz"
    _archive(artifact, "other-file")

    with pytest.raises(ConflictError):
        GAME.after_deploy(tmp_path, {"role": "plugin", "installDir": "takaro"}, artifact)
    assert not (tmp_path / "takaro/TakaroVein").exists()


def test_vein_build_writes_target_identity_beside_archive(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    import json
    import subprocess

    from takaro_maint.games import vein

    def fake_run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
        _archive(Path(command[-1]) / "takaro-vein-plugin.tar.gz", "TakaroVein/libtakaro-vein.so")
        return subprocess.CompletedProcess(command, 0, "", "")

    monkeypatch.setattr(vein.subprocess, "run", fake_run)
    resolved = {
        "id": "linux-1",
        "fingerprint": "f" * 64,
        "fp16": "f" * 16,
        "platform": "linux",
        "revision": "1",
        "components": [{"role": "plugin", "artifact": "takaro-vein-plugin-linux-1-{version}.tar.gz"}],
    }

    result = GAME.build(resolved, "9.9.9", tmp_path, "container", tmp_path, source_revision="abc")

    artifact = result.artifacts["plugin"]
    assert artifact.name == "takaro-vein-plugin-linux-1-9.9.9.tar.gz"
    meta = json.loads(artifact.with_name(artifact.name + ".meta.json").read_text(encoding="utf-8"))
    assert meta["target"] == "linux-1"
    assert meta["fingerprint"] == "f" * 64
    assert meta["connectorVersion"] == "9.9.9"
