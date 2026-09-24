#!/usr/bin/env python3
"""Check reflected ARK chat registration thunks in the pinned Linux executable.

This is an offline analysis tool. A registration is evidence of a named UE
function and its exec thunk, not proof that invoking or hooking it is safe.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


EXPECTED_SHA256 = "7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520"
TEXT_START = 0x648400
TEXT_END = 0x3F6170F
LOAD_BIAS = 0x400000


def registrations(data: bytes, name: str) -> list[dict[str, str]]:
    string = name.encode() + b"\0"
    positions: list[int] = []
    cursor = 0
    while (cursor := data.find(string, cursor)) >= 0:
        if 0x3B61740 <= cursor < 0x43F2C84:
            positions.append(cursor)
        cursor += 1
    if len(positions) != 1:
        raise ValueError(f"{name}: expected one exact rodata name, found {len(positions)}")

    address = positions[0] + LOAD_BIAS
    immediate = b"\xbe" + struct.pack("<I", address)  # mov esi, name address
    matches: list[dict[str, str]] = []
    cursor = TEXT_START - LOAD_BIAS
    while (cursor := data.find(immediate, cursor, TEXT_END - LOAD_BIAS)) >= 0:
        # Registration call shape: mov esi,name; mov edx,exec; xor ecx,ecx;
        # call rel32. No wildcard/closest-match fallback is allowed.
        if data[cursor + 5] == 0xBA and data[cursor + 10 : cursor + 12] == b"\x31\xc9" and data[cursor + 12] == 0xE8:
            thunk = struct.unpack_from("<I", data, cursor + 6)[0]
            call_rel = struct.unpack_from("<i", data, cursor + 13)[0]
            callee = cursor + LOAD_BIAS + 17 + call_rel
            if not (TEXT_START <= thunk < TEXT_END and TEXT_START <= callee < TEXT_END):
                raise ValueError(f"{name}: registration points outside .text")
            matches.append(
                {
                    "registration_va": hex(cursor + LOAD_BIAS),
                    "name_va": hex(address),
                    "exec_thunk_va": hex(thunk),
                    "registration_callee_va": hex(callee),
                    "thunk_first_16_bytes": data[thunk - LOAD_BIAS : thunk - LOAD_BIAS + 16].hex(),
                }
            )
        cursor += 1
    if len(matches) != 1:
        raise ValueError(f"{name}: expected one registration, found {len(matches)}")
    return matches


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    data = args.executable.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"unknown executable SHA-256 {digest}; refusing to derive bindings")
    names = (
        "ClientChatMessage",
        "ClientServerChatMessage",
        "ServerSendChatMessage",
        "SendServerChatMessage",
        "ServerChat",
        "ServerChatTo",
        "ServerChatToPlayer",
        "OnLogout",
    )
    result = {name: registrations(data, name)[0] for name in names}
    print(json.dumps({"executable_sha256": digest, "bindings": result}, indent=2))


if __name__ == "__main__":
    main()
