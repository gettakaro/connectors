#!/usr/bin/env python3
"""Check live location binding defaults against the exact installed ELF bytes."""
from __future__ import annotations

import hashlib
import re
import struct
import sys
from pathlib import Path

EXPECTED_SHA256 = "7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520"
ENTRIES = {
    "player_start_class": (0x2E42C80, "55 48 89 e5 41 56 53 48 81 ec a0 02 00 00"),
    "get_all_actors": (0x263B340, "55 48 89 e5 41 57 41 56 41 55 41 54 53"),
    "get_actor_bounds": (0x26E9360, "55 48 89 e5 41 56 53 48 83 ec 20"),
    "get_object_path": (0x1293230, "55 48 89 e5 53 50 48 89 fb"),
    "name_to_string": (0x1C0B900, "55 48 89 e5 41 56 53 48 89 fb"),
    "game_free": (0x1AF8E30, "55 48 89 e5 53 50 48 89 fb"),
}


def verify(executable: Path, header: Path) -> None:
    binary = executable.read_bytes()
    if hashlib.sha256(binary).hexdigest() != EXPECTED_SHA256:
        raise ValueError("unknown executable SHA-256; no binding bytes trusted")
    if binary[:4] != b"\x7fELF" or binary[4] != 2 or binary[5] != 1:
        raise ValueError("expected a little-endian ELF64 executable")
    phoff = struct.unpack_from("<Q", binary, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", binary, 0x36)
    if phentsize < 56 or phnum < 1 or phoff + phentsize * phnum > len(binary):
        raise ValueError("invalid ELF program header table")
    segments = []
    for i in range(phnum):
        ptype, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", binary, phoff + i * phentsize
        )
        if ptype == 1 and flags & 1 and offset + filesz <= len(binary):
            segments.append((vaddr, vaddr + filesz, offset))
    source = header.read_text(encoding="utf-8")
    for name, (address, expected_hex) in ENTRIES.items():
        declaration = re.search(rf"\b{name}\b[^;]*;", source, re.DOTALL)
        if declaration is None or f"0x{address:X}" not in declaration.group():
            raise ValueError(f"{name} default does not use verified 0x{address:X}")
        expected = bytes.fromhex(expected_hex)
        matches = [binary[offset + address - start:offset + address - start + len(expected)]
                   for start, stop, offset in segments if start <= address and address + len(expected) <= stop]
        if len(matches) != 1 or matches[0] != expected:
            raise ValueError(f"{name} prologue mismatch at 0x{address:X}")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/ShooterGameServer", file=sys.stderr)
        return 2
    try:
        verify(Path(sys.argv[1]), Path(__file__).resolve().parents[1] / "mod/src/location_bindings.hpp")
    except (OSError, ValueError, struct.error) as error:
        print(f"location binding verification failed: {error}", file=sys.stderr)
        return 1
    print(f"verified {len(ENTRIES)} exact location entry prologues on executable SHA-256 {EXPECTED_SHA256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
