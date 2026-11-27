#!/usr/bin/env python3
"""Tests for pack_pbo.py: the header DayZ needs, and byte-identical output.

    python3 -m unittest discover -s games/dayz/mod/tools -p 'test_*.py'
"""
import hashlib
import struct
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pack_pbo  # noqa: E402

SOURCE = Path(__file__).resolve().parents[1] / "takaro_integration"


def read_cstr(data: bytes, offset: int) -> tuple[str, int]:
    end = data.index(b"\x00", offset)
    return data[offset:end].decode("ascii"), end + 1


def parse(data: bytes) -> tuple[dict[str, str], list[tuple[str, int, int]], int]:
    """(properties, [(name, timestamp, size)], offset of the data block)."""
    name, offset = read_cstr(data, 0)
    assert name == ""
    mime, *_ = struct.unpack_from("<IIIII", data, offset)
    assert mime == 0x56657273, hex(mime)
    offset += 20
    properties: dict[str, str] = {}
    while True:
        key, offset = read_cstr(data, offset)
        if not key:
            break
        value, offset = read_cstr(data, offset)
        properties[key] = value
    entries = []
    while True:
        name, offset = read_cstr(data, offset)
        fields = struct.unpack_from("<IIIII", data, offset)
        offset += 20
        if not name:
            assert fields == (0, 0, 0, 0, 0)
            break
        mime, original, _reserved, timestamp, size = fields
        assert (mime, original) == (0, 0), (name, mime, original)
        entries.append((name, timestamp, size))
    return properties, entries, offset


class PackPboTest(unittest.TestCase):
    def pack(self, out: Path, timestamp: int) -> bytes:
        pack_pbo.pack(SOURCE, out, "TakaroIntegration", timestamp)
        return out.read_bytes()

    def test_header_carries_the_properties_dayz_requires(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            data = self.pack(Path(tmp) / "a.pbo", 1700000000)
        properties, entries, _ = parse(data)
        self.assertEqual(
            properties,
            {"prefix": "TakaroIntegration", "product": "dayz ugc", "version": "1", "PboType": "Arma Addon"},
        )
        names = [name for name, _, _ in entries]
        self.assertIn("config.cpp", names)
        self.assertTrue(any(name.startswith("scripts\\4_World\\") for name in names), names)
        self.assertEqual(names, sorted(names, key=str.lower))
        self.assertEqual({timestamp for _, timestamp, _ in entries}, {1700000000})

    def test_data_block_and_sha1_trailer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            data = self.pack(Path(tmp) / "a.pbo", 1700000000)
        _, entries, offset = parse(data)
        body, trailer = data[:-21], data[-21:]
        self.assertEqual(trailer[0], 0)
        self.assertEqual(trailer[1:], hashlib.sha1(body).digest())
        self.assertEqual(len(body) - offset, sum(size for _, _, size in entries))
        first = entries[0]
        on_disk = (SOURCE / first[0].replace("\\", "/")).read_bytes()
        self.assertEqual(data[offset : offset + first[2]], on_disk)

    def test_same_source_and_timestamp_give_identical_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            first = self.pack(Path(tmp) / "a.pbo", 1700000000)
            second = self.pack(Path(tmp) / "b.pbo", 1700000000)
            other = self.pack(Path(tmp) / "c.pbo", 1700000001)
        self.assertEqual(first, second)
        self.assertNotEqual(first, other)

    def test_source_date_epoch_is_the_default_timestamp(self) -> None:
        with unittest.mock.patch.dict("os.environ", {"SOURCE_DATE_EPOCH": "1234567890"}):
            self.assertEqual(pack_pbo.entry_timestamp(), 1234567890)
        self.assertEqual(pack_pbo.entry_timestamp(42), 42)


if __name__ == "__main__":
    unittest.main()
