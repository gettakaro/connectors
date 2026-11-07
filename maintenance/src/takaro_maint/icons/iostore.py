"""Reader for Unreal IoStore table-of-contents files (``.utoc``) and a planner for the ``.ucas`` byte ranges
that hold named packages.

A client install is tens of gigabytes of containers, of which an icon build needs a few hundred
megabytes. The planner answers "which bytes of which ``.ucas`` hold these packages", so only those
ranges have to cross the network.

Layout reference: CUE4Parse ``FIoStoreTocResource`` and ``FIoDirectoryIndexResource`` (MIT licence),
which follow Unreal Engine's ``IoStore.h``. Integers are little-endian unless noted::

    header (144 bytes): magic[16] version u8 reserved u8 reserved u16 headerSize u32 entryCount u32
        blockCount u32 blockEntrySize u32 methodCount u32 methodNameLength u32 blockSize u32
        directoryIndexSize u32 partitionCount u32 containerId u64 encryptionKeyGuid[16]
        containerFlags u8 reserved u8 reserved u16 hashSeedsCount u32 partitionSize u64
        chunksWithoutHashCount u32 reserved u32 reserved u64[5]
    chunk ids          entryCount x 12 bytes (u64 id, u16 index, u8 pad, u8 type)
    offset and length  entryCount x 10 bytes, two big-endian 40-bit integers
    perfect hash seeds hashSeedsCount x i32          (version >= 4)
    chunks w/o hash    chunksWithoutHashCount x i32  (version >= 5)
    compression blocks blockCount x 12 bytes: offset u40, compressed size u24, uncompressed size u24, method u8
    method names       methodCount x methodNameLength bytes
    signatures         (Signed flag) u32 hashSize, two hashes, blockCount x 20 bytes
    directory index    directoryIndexSize bytes (Indexed flag)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from ..exit_codes import BuildFailed, UsageError

MAGIC = b"-==--==--==--==-"
HEADER_SIZE = 144
CHUNK_ID_SIZE = 12
OFFSET_LENGTH_SIZE = 10
BLOCK_ENTRY_SIZE = 12
INVALID = 0xFFFFFFFF
MAX_ENTRIES = 50_000_000
MAX_DIRECTORY_INDEX = 512 * 1024 * 1024

VERSION_DIRECTORY_INDEX = 2
VERSION_PARTITION_SIZE = 3
VERSION_PERFECT_HASH = 4
VERSION_PERFECT_HASH_WITH_OVERFLOW = 5
SUPPORTED_VERSIONS = range(VERSION_DIRECTORY_INDEX, 10)

FLAG_COMPRESSED = 1
FLAG_ENCRYPTED = 2
FLAG_SIGNED = 4
FLAG_INDEXED = 8

PACKAGE_EXTENSIONS = (".uasset", ".umap")
BULK_EXTENSIONS = (".ubulk", ".uptnl", ".m.ubulk")
MERGE_GAP = 64 * 1024
AES_BLOCK = 16

ENCRYPTED_MESSAGE = "encrypted IoStore directory index: not supported by the range planner yet (#747)"


@dataclass(frozen=True)
class Header:
    version: int
    entry_count: int
    block_count: int
    method_count: int
    method_name_length: int
    block_size: int
    directory_index_size: int
    partition_count: int
    container_id: int
    flags: int
    seeds_count: int
    partition_size: int
    without_hash_count: int


@dataclass(frozen=True)
class Range:
    file: str
    offset: int
    length: int


@dataclass(frozen=True)
class Toc:
    """One parsed container: enough to say where a named file's bytes are, and nothing about their content."""

    name: str
    header: Header
    files: dict[str, int]
    offsets: list[tuple[int, int]]
    chunk_ids: list[bytes]
    blocks: bytes
    mount_point: str

    @property
    def encrypted(self) -> bool:
        return bool(self.header.flags & FLAG_ENCRYPTED)

    def block(self, index: int) -> tuple[int, int]:
        """``(offset in the whole container, compressed size)`` of one compression block."""
        raw = self.blocks[index * BLOCK_ENTRY_SIZE : (index + 1) * BLOCK_ENTRY_SIZE]
        offset = int.from_bytes(raw[0:5], "little")
        size = int.from_bytes(raw[5:8], "little")
        return offset, size

    def ucas_names(self) -> list[str]:
        """The ``.ucas`` file of each partition, relative to the directory of the ``.utoc``."""
        stem = self.name.removesuffix(".utoc")
        return [
            f"{stem}.ucas" if index == 0 else f"{stem}_s{index}.ucas" for index in range(self.header.partition_count)
        ]


def read_header(path: Path, *, indexed: bool = True) -> Header:
    """The validated header of a ``.utoc``; refuses what the planner cannot read."""
    with path.open("rb") as handle:
        raw = handle.read(HEADER_SIZE)
    if len(raw) < HEADER_SIZE or raw[:16] != MAGIC:
        raise BuildFailed(f"{path.name} is not an IoStore table of contents", file=path.name)
    (
        version,
        _r0,
        _r1,
        header_size,
        entry_count,
        block_count,
        block_entry_size,
        method_count,
        method_name_length,
        block_size,
        directory_index_size,
        partition_count,
        container_id,
    ) = struct.unpack_from("<BBHIIIIIIIIIQ", raw, 16)
    flags = raw[16 + 4 + 4 * 9 + 8 + 16]
    seeds_count, partition_size, without_hash_count = struct.unpack_from("<IQI", raw, 16 + 4 + 4 * 9 + 8 + 16 + 4)
    if version not in SUPPORTED_VERSIONS:
        raise BuildFailed(
            f"{path.name}: IoStore TOC version {version} is not supported (known: "
            f"{SUPPORTED_VERSIONS.start}..{SUPPORTED_VERSIONS.stop - 1})",
            file=path.name,
            version=version,
        )
    if header_size != HEADER_SIZE or block_entry_size != BLOCK_ENTRY_SIZE:
        raise BuildFailed(
            f"{path.name}: unexpected IoStore TOC layout (header {header_size}, block {block_entry_size})"
        )
    if flags & FLAG_ENCRYPTED:
        raise UsageError(f"{path.name}: {ENCRYPTED_MESSAGE}", file=path.name)
    if indexed and not flags & FLAG_INDEXED:
        raise BuildFailed(f"{path.name}: the container has no directory index (Indexed flag unset)", file=path.name)
    if version < VERSION_PARTITION_SIZE:
        partition_count, partition_size = 1, 1 << 63
    if entry_count > MAX_ENTRIES or block_count > MAX_ENTRIES or directory_index_size > MAX_DIRECTORY_INDEX:
        raise BuildFailed(f"{path.name}: implausible IoStore TOC sizes")
    if block_size == 0 or partition_count == 0 or partition_size == 0:
        raise BuildFailed(f"{path.name}: implausible IoStore TOC geometry")
    return Header(
        version=version,
        entry_count=entry_count,
        block_count=block_count,
        method_count=method_count,
        method_name_length=method_name_length,
        block_size=block_size,
        directory_index_size=directory_index_size,
        partition_count=partition_count,
        container_id=container_id,
        flags=flags,
        seeds_count=seeds_count,
        partition_size=partition_size,
        without_hash_count=without_hash_count,
    )


def read_toc(path: Path, name: str | None = None) -> Toc:
    header = read_header(path)
    data = path.read_bytes()
    position = HEADER_SIZE
    count = header.entry_count

    def take(size: int) -> bytes:
        nonlocal position
        chunk = data[position : position + size]
        if len(chunk) != size:
            raise BuildFailed(f"{path.name}: the table of contents ends early")
        position += size
        return chunk

    ids_raw = take(count * CHUNK_ID_SIZE)
    offsets_raw = take(count * OFFSET_LENGTH_SIZE)
    if header.version >= VERSION_PERFECT_HASH:
        take(header.seeds_count * 4)
    if header.version >= VERSION_PERFECT_HASH_WITH_OVERFLOW:
        take(header.without_hash_count * 4)
    blocks = take(header.block_count * BLOCK_ENTRY_SIZE)
    take(header.method_count * header.method_name_length)
    if header.flags & FLAG_SIGNED:
        (hash_size,) = struct.unpack("<I", take(4))
        take(hash_size * 2 + header.block_count * 20)
    index = take(header.directory_index_size)
    mount_point, files = _directory_index(index, path.name, count)
    return Toc(
        name=name or path.name,
        header=header,
        files=files,
        offsets=[
            (
                int.from_bytes(offsets_raw[at : at + 5], "big"),
                int.from_bytes(offsets_raw[at + 5 : at + 10], "big"),
            )
            for at in range(0, len(offsets_raw), OFFSET_LENGTH_SIZE)
        ],
        chunk_ids=[ids_raw[at : at + CHUNK_ID_SIZE] for at in range(0, len(ids_raw), CHUNK_ID_SIZE)],
        blocks=blocks,
        mount_point=mount_point,
    )


def _fstring(data: bytes, position: int) -> tuple[str, int]:
    (length,) = struct.unpack_from("<i", data, position)
    position += 4
    if length == 0:
        return "", position
    if length < 0:
        size = -length * 2
        return data[position : position + size - 2].decode("utf-16-le"), position + size
    return data[position : position + length - 1].decode("latin-1"), position + length


def _directory_index(data: bytes, name: str, entry_count: int) -> tuple[str, dict[str, int]]:
    """``(mount point, lower-cased full path -> chunk index)`` for every file of the container."""
    try:
        mount_point, position = _fstring(data, 0)
        (directory_count,) = struct.unpack_from("<I", data, position)
        position += 4
        directories = struct.unpack_from(f"<{directory_count * 4}I", data, position)
        position += directory_count * 16
        (file_count,) = struct.unpack_from("<I", data, position)
        position += 4
        file_entries = struct.unpack_from(f"<{file_count * 3}I", data, position)
        position += file_count * 12
        (string_count,) = struct.unpack_from("<I", data, position)
        position += 4
        strings = []
        for _ in range(string_count):
            text, position = _fstring(data, position)
            strings.append(text)
    except (struct.error, UnicodeDecodeError) as exc:
        raise BuildFailed(f"{name}: the directory index cannot be read ({exc})") from exc

    try:
        prefix = mount_point
        while prefix.startswith("../"):
            prefix = prefix[3:]
        files: dict[str, int] = {}
        stack: list[tuple[int, str]] = [(0, prefix)] if directory_count else []
        visited = 0
        while stack:
            entry, directory_path = stack.pop()
            visited += 1
            if visited > directory_count + 1:
                raise BuildFailed(f"{name}: the directory index loops")
            _name, first_child, _sibling, first_file = directories[entry * 4 : entry * 4 + 4]
            file_index = first_file
            while file_index != INVALID:
                file_name, next_file, user_data = file_entries[file_index * 3 : file_index * 3 + 3]
                if user_data >= entry_count:
                    raise BuildFailed(f"{name}: a directory index file points past the chunk table")
                files[(directory_path + strings[file_name]).lower()] = user_data
                file_index = next_file
            child = first_child
            while child != INVALID:
                child_name = directories[child * 4]
                stack.append((child, directory_path + strings[child_name] + "/"))
                child = directories[child * 4 + 2]
    except (IndexError, ValueError) as exc:
        raise BuildFailed(f"{name}: the directory index is inconsistent ({exc})") from exc
    return mount_point, files


def _chunk_ranges(toc: Toc, chunk: int) -> list[Range]:
    offset, length = toc.offsets[chunk]
    if length == 0:
        return []
    header = toc.header
    first = offset // header.block_size
    last = (offset + length - 1) // header.block_size
    if last >= header.block_count:
        raise BuildFailed(f"{toc.name}: a chunk points past the compression blocks")
    ucas = toc.ucas_names()
    ranges = []
    for index in range(first, last + 1):
        block_offset, size = toc.block(index)
        if toc.encrypted:
            size = -(-size // AES_BLOCK) * AES_BLOCK
        partition = block_offset // header.partition_size
        if partition >= len(ucas):
            raise BuildFailed(f"{toc.name}: a block lies in partition {partition} of {len(ucas)}")
        ranges.append(Range(ucas[partition], block_offset % header.partition_size, size))
    return ranges


def container_header_chunks(toc: Toc) -> list[int]:
    """The chunk holding the container header: Unreal keys it by the container id itself."""
    wanted = struct.pack("<Q", toc.header.container_id)
    return [index for index, chunk_id in enumerate(toc.chunk_ids) if chunk_id[:8] == wanted]


def plan(tocs: list[Toc], stems: list[str], *, whole: list[str] | None = None) -> tuple[list[Range], list[str]]:
    """``(ranges, missing stems)``: the ``.ucas`` byte ranges that hold the files named ``<stem>.<extension>``.

    ``stems`` are paths below the container mount, such as ``ConanSandbox/Content/UI/Icon``. Every container
    contributes its container header, which Unreal reads when it mounts. Containers named in ``whole`` are
    read in full (the ``global`` container is a few megabytes and every package needs its script objects).
    """
    wanted: list[Range] = []
    missing = []
    for stem in sorted(set(stems)):
        found = False
        for toc in tocs:
            for extension in (*PACKAGE_EXTENSIONS, *BULK_EXTENSIONS):
                chunk = toc.files.get((stem + extension).lower())
                if chunk is not None:
                    found = found or extension in PACKAGE_EXTENSIONS
                    wanted.extend(_chunk_ranges(toc, chunk))
        if not found:
            missing.append(stem)
    for toc in tocs:
        for chunk in container_header_chunks(toc):
            wanted.extend(_chunk_ranges(toc, chunk))
    return merge(wanted), missing


def merge(ranges: list[Range]) -> list[Range]:
    """Sort, de-duplicate and join ranges of one file that lie less than ``MERGE_GAP`` apart."""
    merged: list[Range] = []
    for current in sorted(set(ranges), key=lambda r: (r.file, r.offset, r.length)):
        last = merged[-1] if merged else None
        if last and last.file == current.file and current.offset - (last.offset + last.length) < MERGE_GAP:
            end = max(last.offset + last.length, current.offset + current.length)
            merged[-1] = Range(last.file, last.offset, end - last.offset)
        else:
            merged.append(current)
    return merged


def safe_stem(stem: str) -> str:
    """A package stem as a normalised relative path (no climbing out of the mount)."""
    pure = PurePosixPath(stem)
    if pure.is_absolute() or ".." in pure.parts:
        raise UsageError(f"package path {stem!r} must be relative to the container mount")
    return pure.as_posix()
