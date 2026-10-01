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
