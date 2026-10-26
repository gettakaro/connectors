#!/usr/bin/env python3
"""Makes an ILRepack-merged assembly byte-identical from build to build.

ILRepack 2.0.18 has no deterministic mode: every merge writes a fresh PE timestamp, a
random module MVID, and renames each merged input's <PrivateImplementationDetails> with a
random GUID. This replaces all three with values derived from the rest of the file, so the
same sources still give the same Takaro.dll and the same release zip.

Usage: normalize-assembly.py <assembly.dll>
"""

import hashlib
import re
import struct
import sys
import uuid

GUID_NAME = re.compile(
    rb"<([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})><PrivateImplementationDetails>"
)
ZERO_GUID = b"00000000-0000-0000-0000-000000000000"


def rva_to_offset(data, sections, rva):
    for virtual_address, virtual_size, raw_pointer in sections:
        if virtual_address <= rva < virtual_address + virtual_size:
            return rva - virtual_address + raw_pointer
    raise SystemExit(f"RVA {rva:#x} is in no section")


def locate(data):
    """Offsets of the PE timestamp and of the module MVID."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    timestamp = pe + 8
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (96 if magic == 0x10B else 112)
    cli_rva = struct.unpack_from("<I", data, directories + 14 * 8)[0]
    section_table = optional + optional_size
    sections = []
    for i in range(section_count):
        virtual_size, virtual_address, _raw_size, raw_pointer = struct.unpack_from(
            "<IIII", data, section_table + i * 40 + 8
        )
        sections.append((virtual_address, virtual_size, raw_pointer))
    cli = rva_to_offset(data, sections, cli_rva)
    metadata = rva_to_offset(data, sections, struct.unpack_from("<I", data, cli + 8)[0])
    version_length = struct.unpack_from("<I", data, metadata + 12)[0]
    cursor = metadata + 16 + version_length + 2
    stream_count = struct.unpack_from("<H", data, cursor)[0]
    cursor += 2
    streams = {}
    for _ in range(stream_count):
        offset, _size = struct.unpack_from("<II", data, cursor)
        end = data.index(b"\0", cursor + 8)
        name = data[cursor + 8 : end].decode("ascii")
        cursor = (end + 4) & ~3
        streams[name] = metadata + offset
    tables = streams.get("#~") or streams["#-"]
    heap_sizes = data[tables + 6]
    string_index = 4 if heap_sizes & 1 else 2
    guid_index = 4 if heap_sizes & 2 else 2
    # The Module table is the first table: Generation, Name, Mvid, EncId, EncBaseId.
    valid = struct.unpack_from("<Q", data, tables + 8)[0]
    present = valid.bit_count()
    module_row = tables + 24 + 4 * present
    mvid_at = module_row + 2 + string_index
    mvid = struct.unpack_from("<I" if guid_index == 4 else "<H", data, mvid_at)[0]
    return timestamp, streams["#GUID"] + (mvid - 1) * 16


def main(path):
    with open(path, "rb") as source:
        data = bytearray(source.read())
    timestamp, mvid = locate(data)
    names = [m.start(1) for m in GUID_NAME.finditer(data)]

    data[timestamp : timestamp + 4] = bytes(4)
    data[mvid : mvid + 16] = bytes(16)
    for start in names:
        data[start : start + 36] = ZERO_GUID
    digest = hashlib.sha256(data).digest()

    data[timestamp : timestamp + 4] = struct.pack(
        "<I", struct.unpack_from("<I", digest)[0] & 0x7FFFFFFF
    )
    data[mvid : mvid + 16] = digest[:16]
    for n, start in enumerate(names):
        derived = hashlib.sha256(digest + n.to_bytes(4, "little")).digest()
        data[start : start + 36] = str(uuid.UUID(bytes=derived[:16])).encode("ascii")
    with open(path, "wb") as target:
        target.write(data)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
