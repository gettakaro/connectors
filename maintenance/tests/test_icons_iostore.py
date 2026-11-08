"""Unit tests for the .utoc directory-index parser on synthetic blobs."""

from __future__ import annotations

import struct

import pytest

from takaro_maint.exit_codes import BuildFailed
from takaro_maint.icons import iostore

INVALID = iostore.INVALID


def _fstring(text: str) -> bytes:
    raw = text.encode() + b"\0"
    return struct.pack("<i", len(raw)) + raw


def _index(
    directories: list[tuple[int, int, int, int]], files: list[tuple[int, int, int]], strings: list[str]
) -> bytes:
    blob = _fstring("../../../") + struct.pack("<I", len(directories))
    blob += b"".join(struct.pack("<4I", *entry) for entry in directories)
    blob += struct.pack("<I", len(files)) + b"".join(struct.pack("<3I", *entry) for entry in files)
    return blob + struct.pack("<I", len(strings)) + b"".join(_fstring(text) for text in strings)


def test_a_well_formed_index_maps_every_file() -> None:
    data = _index(
        directories=[(INVALID, 1, INVALID, INVALID), (3, INVALID, INVALID, 0)],
        files=[(1, 1, 0), (2, INVALID, 1)],
        strings=["", "x", "y", "a"],
    )
    mount_point, files = iostore._directory_index(data, "x.utoc", 2)
    assert mount_point == "../../../"
    assert files == {"a/x": 0, "a/y": 1}


def test_a_cyclic_file_chain_is_refused() -> None:
    data = _index(directories=[(INVALID, INVALID, INVALID, 0)], files=[(0, 0, 0)], strings=["x"])
    with pytest.raises(BuildFailed, match="loops"):
        iostore._directory_index(data, "x.utoc", 1)


def test_a_cyclic_sibling_chain_is_refused() -> None:
    data = _index(directories=[(INVALID, 1, INVALID, INVALID), (0, INVALID, 1, INVALID)], files=[], strings=["a"])
    with pytest.raises(BuildFailed, match="loops"):
        iostore._directory_index(data, "x.utoc", 1)
