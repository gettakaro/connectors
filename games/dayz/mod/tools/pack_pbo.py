#!/usr/bin/env python3
"""
Pack a directory of Enforce Script source into a DayZ-compatible PBO file.

Format reference: https://community.bistudio.com/wiki/PBO_File_Format

This is a minimal uncompressed packer. It's enough for script-only addons
(Enforce Script source + config.cpp). For asset-heavy addons with .p3d / .paa
files you should use Mikero's tools or DayZ Tools' AddonBuilder.

Usage:
    python3 pack_pbo.py <source_dir> <output.pbo> [--prefix PREFIX]

Example:
    python3 pack_pbo.py src/takaro_integration @TakaroIntegration/Addons/TakaroIntegration.pbo \\
        --prefix TakaroIntegration
"""
import argparse
import hashlib
import os
import struct
import sys
import time
from pathlib import Path


def write_cstr(buf: bytearray, s: str) -> None:
    buf.extend(s.encode("ascii"))
    buf.append(0)


def write_header_entry(buf: bytearray, filename: str, mime: int, original_size: int,
                       reserved: int, timestamp: int, data_size: int) -> None:
    write_cstr(buf, filename)
    buf.extend(struct.pack("<I", mime))
    buf.extend(struct.pack("<I", original_size))
    buf.extend(struct.pack("<I", reserved))
    buf.extend(struct.pack("<I", timestamp))
    buf.extend(struct.pack("<I", data_size))


def write_properties(buf: bytearray, properties: dict[str, str]) -> None:
    """First header entry — empty filename, mime=0x56657273 'Vers', then key/value strings terminated by empty string."""
    write_cstr(buf, "")  # empty filename
    # mime 'Vers' in little-endian: 0x56657273
    buf.extend(struct.pack("<I", 0x56657273))
    # remaining four uint32s are 0 for the properties entry
    buf.extend(struct.pack("<IIII", 0, 0, 0, 0))
    # key/value pairs
    for key, value in properties.items():
        write_cstr(buf, key)
        write_cstr(buf, value)
    # terminating empty string
    write_cstr(buf, "")


def collect_files(source_dir: Path) -> list[tuple[str, Path]]:
    """Walk source_dir and return list of (relative_pbo_path, absolute_disk_path)."""
    out: list[tuple[str, Path]] = []
    for root, dirs, files in os.walk(source_dir):
        # skip common junk
        dirs[:] = [d for d in dirs if d not in (".git", "__pycache__", ".vscode")]
        for fn in files:
            if fn.endswith((".pbo", ".bisign", ".tmp", ".bak")):
                continue
            disk = Path(root) / fn
            rel = disk.relative_to(source_dir)
            # PBO uses backslashes between path components
            pbo_path = str(rel).replace("/", "\\")
            out.append((pbo_path, disk))
    out.sort(key=lambda p: p[0].lower())
    return out


def pack(source_dir: Path, output_path: Path, prefix: str) -> None:
    if not source_dir.is_dir():
        raise SystemExit(f"Source directory not found: {source_dir}")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    files = collect_files(source_dir)
    if not files:
        raise SystemExit(f"No files found in {source_dir}")

    print(f"Packing {len(files)} files from {source_dir}")
    for pbo_path, disk in files:
        size = disk.stat().st_size
        print(f"  {pbo_path}  ({size} bytes)")

    # Build header section
    # Properties match what DayZ community mods (e.g. CommunityFramework)
    # use — product='dayz ugc' and PboType='Arma Addon' are required for
    # the engine to mount the PBO as a community mod, not just informational.
    header = bytearray()
    write_properties(header, {
        "prefix": prefix,
        "product": "dayz ugc",
        "version": "1",
        "PboType": "Arma Addon",
    })

    timestamp = int(time.time())
    file_data: list[bytes] = []
    for pbo_path, disk in files:
        data = disk.read_bytes()
        file_data.append(data)
        # For uncompressed files, original_size MUST be 0 (per Bohemia spec
        # and observed in vanilla DayZ PBOs). data_size carries the real length.
        write_header_entry(header, pbo_path, 0, 0, 0, timestamp, len(data))

    # Final null entry to end the header table (filename "" + 5 uint32 zeros)
    write_cstr(header, "")
    header.extend(struct.pack("<IIIII", 0, 0, 0, 0, 0))

    # Concatenate header + file data
    body = bytes(header) + b"".join(file_data)

    # SHA1 trailer
    sha1 = hashlib.sha1(body).digest()
    trailer = b"\x00" + sha1

    output_path.write_bytes(body + trailer)
    print(f"\nWrote {output_path} ({output_path.stat().st_size} bytes)")
    print(f"  prefix: {prefix}")
    print(f"  sha1:   {sha1.hex()}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source_dir", type=Path)
    ap.add_argument("output_pbo", type=Path)
    ap.add_argument("--prefix", required=True,
                    help="Virtual path prefix the PBO is mounted at, e.g. TakaroIntegration")
    args = ap.parse_args()
    pack(args.source_dir, args.output_pbo, args.prefix)


if __name__ == "__main__":
    main()
