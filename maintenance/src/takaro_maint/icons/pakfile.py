"""A range planner for legacy Unreal ``.pak`` containers (version 10 and 11, with a full directory index).

Like ``iostore`` it answers one question without reading any content: which byte ranges of a container hold
the files of the packages a build needs. A pak is read from its footer: the footer names the primary index,
which names the full directory index (file name to encoded entry) and the path-hash index. The planner reads
those three and decodes the entries; a package is then the byte span of its ``.uasset``, ``.uexp`` and bulk
entries, each including the serialized entry header that precedes the data.

Encrypted containers are refused outright: the index flag, a non-zero key GUID and an encrypted entry that a
build needs all end in a usage error, because nothing here reads, derives or stores an AES key.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO

from ..exit_codes import BuildFailed, UsageError
from .iostore import BULK_EXTENSIONS, PACKAGE_EXTENSIONS, Range, merge

MAGIC = 0x5A6F12E1
SUPPORTED_VERSIONS = (10, 11)
FOOTER_SIZE = 221
GUID_SIZE = 16
MAX_INDEX = 512 * 1024 * 1024
MAX_ENTRIES = 50_000_000
PAK_EXTENSIONS = (*PACKAGE_EXTENSIONS, ".uexp", *BULK_EXTENSIONS)
DESCRIPTOR_EXTENSIONS = (".uplugin", ".upluginmanifest", ".uproject")
ENTRY_HEADER_FIXED = 53
BLOCK_RECORD = 16
ENCRYPTED_MESSAGE = "encrypted pak: this tool reads unencrypted paks only; recovering a key is a maintainer decision"


@dataclass(frozen=True)
class Entry:
    offset: int
    size: int
    uncompressed_size: int
    method: int
    blocks: int
    encrypted: bool

    @property
    def span(self) -> int:
        """Bytes from ``offset``: the serialized entry header, then the (compressed) data."""
        header = ENTRY_HEADER_FIXED + (4 + BLOCK_RECORD * self.blocks if self.method else 0)
        return header + self.size


@dataclass(frozen=True)
class PakIndex:
    """One parsed pak: where each named file's bytes are, and the metadata ranges every mount reads."""

    name: str
    size: int
    version: int
    mount_point: str
    files: dict[str, Entry]
    metadata: list[Range]
    identity_sha256: str

    @property
    def encrypted_entries(self) -> int:
        return sum(1 for entry in self.files.values() if entry.encrypted)


class _Reader:
    def __init__(self, data: bytes, name: str) -> None:
        self.data = data
        self.name = name
        self.at = 0

    def take(self, count: int) -> bytes:
        if count < 0 or self.at + count > len(self.data):
            raise BuildFailed(f"{self.name}: the pak index ends early")
        chunk = self.data[self.at : self.at + count]
        self.at += count
        return chunk

    def unpack(self, fmt: str) -> tuple[int, ...]:
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))

    def string(self) -> str:
        (length,) = self.unpack("<i")
        if length == 0:
            return ""
        if length > 0:
            return self.take(length)[:-1].decode("latin-1")
        return self.take(-length * 2)[:-2].decode("utf-16-le")


def _read_at(handle: BinaryIO, offset: int, length: int, name: str) -> bytes:
    handle.seek(offset)
    data = handle.read(length)
    if len(data) != length:
        raise BuildFailed(f"{name}: the pak ends before offset {offset + length}")
    return data


def _decode_entry(reader: _Reader) -> Entry:
    (bits,) = reader.unpack("<I")
    block_bits = bits & 0x3F
    method = (bits >> 23) & 0x3F
    encrypted = bool(bits & (1 << 22))
    blocks = (bits >> 6) & 0xFFFF
    if block_bits == 0x3F:
        reader.unpack("<I")
    offset = reader.unpack("<I")[0] if bits & (1 << 31) else reader.unpack("<Q")[0]
    uncompressed = reader.unpack("<I")[0] if bits & (1 << 30) else reader.unpack("<Q")[0]
    size = uncompressed
    if method:
        size = reader.unpack("<I")[0] if bits & (1 << 29) else reader.unpack("<Q")[0]
    if blocks and not (blocks == 1 and not encrypted):
        reader.take(4 * blocks)
    return Entry(offset, size, uncompressed, method, blocks, encrypted)


def _relative(mount_point: str, directory: str, name: str) -> str:
    parts = [part for part in PurePosixPath(mount_point + directory + name).parts if part not in ("/", "..", ".")]
    return "/".join(parts).lower()


def read_index(path: Path, name: str) -> PakIndex:
    """Parse the footer and the indexes of the pak at ``path``; refuse what this planner cannot plan."""
    size = path.stat().st_size
    if size < FOOTER_SIZE:
        raise BuildFailed(f"{name}: too small to be a pak", file=name)
    with path.open("rb") as handle:
        footer = _read_at(handle, size - FOOTER_SIZE, FOOTER_SIZE, name)
        guid, encrypted, magic, version, index_offset, index_size = struct.unpack_from("<16sBIIQQ", footer)
        if magic != MAGIC:
            raise BuildFailed(f"{name}: no pak footer (magic {magic:#x})", file=name)
        if encrypted or any(guid):
            raise UsageError(f"{name}: {ENCRYPTED_MESSAGE} (encrypted index or key GUID set)", file=name)
        if version not in SUPPORTED_VERSIONS:
            raise UsageError(f"{name}: pak version {version} is not supported (known: {SUPPORTED_VERSIONS})", file=name)
        if index_size > MAX_INDEX or index_offset + index_size > size - FOOTER_SIZE:
            raise BuildFailed(f"{name}: implausible primary index location", file=name)
        primary = _read_at(handle, index_offset, index_size, name)
        reader = _Reader(primary, name)
        mount_point = reader.string()
        (count,) = reader.unpack("<i")
        reader.unpack("<Q")
        metadata = [Range(str(name), size - FOOTER_SIZE, FOOTER_SIZE), Range(str(name), index_offset, index_size)]
        regions: dict[str, tuple[int, int]] = {}
        for label in ("pathHash", "fullDirectory"):
            (present,) = reader.unpack("<i")
            if present:
                offset, length = reader.unpack("<qq")
                reader.take(20)
                regions[label] = (offset, length)
                metadata.append(Range(str(name), offset, length))
        (encoded_size,) = reader.unpack("<i")
        encoded = reader.take(encoded_size)
        (non_encoded,) = reader.unpack("<i")
        if non_encoded:
            raise BuildFailed(f"{name}: {non_encoded} entries are stored unencoded; not supported", file=name)
        if "fullDirectory" not in regions:
            raise BuildFailed(f"{name}: the pak has no full directory index", file=name)
        offset, length = regions["fullDirectory"]
        if length > MAX_INDEX or offset + length > size - FOOTER_SIZE:
            raise BuildFailed(f"{name}: implausible full directory index location", file=name)
        directory = _read_at(handle, offset, length, name)
    if not 0 <= count <= MAX_ENTRIES:
        raise BuildFailed(f"{name}: implausible entry count {count}", file=name)
    files = _directory_entries(directory, encoded, mount_point, name)
    identity = hashlib.sha256(footer + primary).hexdigest()
    return PakIndex(name, size, version, mount_point, files, merge(metadata), identity)


def _directory_entries(directory: bytes, encoded: bytes, mount_point: str, name: str) -> dict[str, Entry]:
    reader = _Reader(directory, name)
    files: dict[str, Entry] = {}
    (directories,) = reader.unpack("<i")
    for _ in range(directories):
        folder = reader.string()
        (entries,) = reader.unpack("<i")
        for _ in range(entries):
            file_name = reader.string()
            (offset,) = reader.unpack("<i")
            if not 0 <= offset < len(encoded):
                raise BuildFailed(f"{name}: a directory index entry points outside the encoded entries", file=name)
            files[_relative(mount_point, folder, file_name)] = _decode_entry(_Reader(encoded[offset:], name))
    return files


def plan(indexes: list[PakIndex], stems: list[str]) -> tuple[list[Range], list[str]]:
    """``(ranges, missing stems)``: the bytes of every ``<stem>.<extension>`` in any pak, plus each pak's metadata.

    A package that more than one pak holds is planned in all of them, so a mount resolves it exactly as the
    installed client would. The plugin and project descriptors of every pak are planned too.
    """
    wanted: list[Range] = []
    missing = []
    for stem in sorted(set(stems)):
        found = False
        for index in indexes:
            for extension in PAK_EXTENSIONS:
                entry = index.files.get((stem + extension).lower())
                if entry is None:
                    continue
                if entry.encrypted:
                    raise UsageError(f"{index.name}: {stem}{extension} is encrypted; {ENCRYPTED_MESSAGE}")
                found = found or extension in PACKAGE_EXTENSIONS
                wanted.append(Range(index.name, entry.offset, entry.span))
        if not found:
            missing.append(stem)
    for index in indexes:
        wanted.extend(index.metadata)
        # A mount reads the plugin and project descriptors to map virtual paths, so they ride along.
        wanted.extend(
            Range(index.name, entry.offset, entry.span)
            for path, entry in index.files.items()
            if path.endswith(DESCRIPTOR_EXTENSIONS) and not entry.encrypted
        )
    return merge(wanted), missing
