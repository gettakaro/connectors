#!/usr/bin/env python3
"""Generate the synthetic VEIN fixtures: client paks, a server depot and the Unreal decoder stand-in's outputs.

Nothing here is game data. The Blueprints, classes and item names are invented, the pak entries hold a few
synthetic bytes the decoder stand-in never parses, and the pictures are flat colours. What is real is the
container layout: the client paks are legacy pak files of version 11 with an unencrypted primary, full directory
and path-hash index, written the way the engine writes them, so the range planner reads them like VEIN's.
``write_client`` and ``write_server`` build the trees at test time (not committed, because a committed ``.pak``
invites mistaking it for game data); ``python make_fixture.py`` rewrites the committed decoder outputs and
``test_cli_icons_vein.py`` regenerates them into a temporary directory and requires byte equality.
"""

from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any

from PIL import Image

HERE = Path(__file__).parent
BUILD_ID = 25970224
MANIFEST = "7470000000000000047"
MOUNT = "../../../"
PAKS = "Vein/Content/Paks"
ITEMS = "Vein/Content/Vein/Items"
MAGIC = 0x5A6F12E1
FOOTER_SIZE = 221

# name -> (native root, parent Blueprint or None, own thumbnail kind, flags); kinds: png, none, absent, bad-format.
BLUEPRINTS: dict[str, dict[str, Any]] = {
    "BP_Synth_Plain": {"root": "SynthItem", "type": True, "thumb": "png", "color": (200, 40, 40, 255)},
    "BP_Synth_Child": {"root": "SynthItem", "parent": "BP_Synth_Plain"},
    "BP_Synth_Soup": {"root": "SynthFood", "type": True, "thumb": "png", "color": (40, 160, 60, 255)},
    "BP_Synth_Food": {"root": "SynthFood", "thumb": "png", "color": (60, 60, 220, 255)},
    "BP_Synth_NoIcon": {"root": "SynthItem"},
    "BP_Synth_MissingTex": {"root": "SynthItem", "thumb": "absent"},
    "BP_Synth_BadFormat": {"root": "SynthItem", "thumb": "bad-format"},
    "BP_Synth_Abstract": {"root": "SynthItem", "thumb": "png", "color": (1, 2, 3, 255), "abstract": True},
    "SKEL_BP_Synth_Plain": {"root": "SynthItem", "thumb": "png", "color": (4, 5, 6, 255)},
    "BP_Synth_Chair": {"root": "SynthActor", "thumb": "png", "color": (7, 8, 9, 255)},
    "BP_Synth_ClientOnly": {"root": "SynthItem", "thumb": "png", "color": (9, 9, 9, 255), "client-only": True},
}
SERVER_PAKS = {"pakchunk0-LinuxServer.pak": b"synthetic server pak zero\n", "pakchunk1-LinuxServer.pak": b"one\n"}
SERVER_DECOY = {"pakchunk2-LinuxServer.pak": b"not selected\n"}
# client pak -> thumbnails (blueprint names) it holds, in write order; the plain thumbnail is duplicated.
CLIENT_PAKS: dict[str, list[str]] = {
    "pakchunk0-Windows.pak": ["BP_Synth_Food", "BP_Synth_Unused"],
    "pakchunk1-Windows.pak": ["BP_Synth_Plain", "BP_Synth_Soup", "BP_Synth_BadFormat", "BP_Synth_Unused"],
    "pakchunk104-Windows.pak": ["BP_Synth_Plain"],
}
COMPRESSED = {"BP_Synth_Soup"}
TEXTURE_FORMAT = "PF_DXT5"


def thumb_package(name: str) -> str:
    return f"/Game/Vein/Items/T_Thumb_{name}_0"


def thumb_stem(name: str) -> str:
    return f"{ITEMS}/T_Thumb_{name}_0"


def blueprint_package(name: str) -> str:
    return f"/Game/Vein/Items/{name}"


def class_of(name: str) -> str:
    return f"{name}_C"


def _json(value: Any) -> bytes:
    return (json.dumps(value, indent=2) + "\n").encode("utf-8")


def _png(color: tuple[int, int, int, int]) -> bytes:
    import io

    buffer = io.BytesIO()
    Image.new("RGBA", (16, 16), color).save(buffer, "PNG")
    return buffer.getvalue()


# ------------------------------------------------------------------------------------------ pak writer


def fstring(text: str) -> bytes:
    raw = text.encode("latin-1") + b"\0"
    return struct.pack("<i", len(raw)) + raw


def entry_payload(name: str, suffix: str) -> bytes:
    return f"synthetic {suffix} of {name}\n".encode() * (20000 if "Unused" in name else 3)


def _entry_header(size: int, uncompressed: int, method: int, blocks: int) -> bytes:
    header = struct.pack("<QQQI", 0, size, uncompressed, method) + bytes(20)
    if method:
        header += struct.pack("<I", blocks) + bytes(16 * blocks)
    return header + struct.pack("<BI", 0, 0)


def _encode(offset: int, size: int, uncompressed: int, method: int) -> bytes:
    bits = (1 << 31) | (1 << 30)
    if method:
        bits |= (1 << 29) | (method << 23) | (1 << 6) | 32
    encoded = struct.pack("<III", bits, offset, uncompressed)
    return encoded + (struct.pack("<I", size) if method else b"")


def build_pak(
    files: dict[str, bytes],
    *,
    compressed: frozenset[str] = frozenset(),
    version: int = 11,
    encrypted_index: bool = False,
    key_guid: bytes = bytes(16),
    encrypted_entries: frozenset[str] = frozenset(),
) -> bytes:
    """A version 11 pak holding ``files`` (relative path -> bytes); the entries of ``compressed`` carry method 1."""
    body = bytearray()
    encoded = bytearray()
    directory: dict[str, dict[str, int]] = {}
    for path in sorted(files):
        data = files[path]
        method = 1 if path in compressed else 0
        offset = len(body)
        body += _entry_header(len(data), len(data), method, 1 if method else 0) + data
        at = len(encoded)
        record = _encode(offset, len(data), len(data), method)
        if path in encrypted_entries:
            record = struct.pack("<I", struct.unpack("<I", record[:4])[0] | (1 << 22)) + record[4:]
        encoded += record
        folder, _, name = path.rpartition("/")
        directory.setdefault(f"{folder}/" if folder else "/", {})[name] = at
    full = struct.pack("<i", len(directory))
    for folder, names in directory.items():
        full += fstring(folder) + struct.pack("<i", len(names))
        for name, at in names.items():
            full += fstring(name) + struct.pack("<i", at)
    path_hash = b"synthetic path hash index\n"
    path_hash_offset = len(body)
    body += path_hash
    full_offset = len(body)
    body += full
    primary_offset = len(body)
    primary = (
        fstring(MOUNT)
        + struct.pack("<iQ", len(files), 0)
        + struct.pack("<iqq", 1, path_hash_offset, len(path_hash))
        + bytes(20)
        + struct.pack("<iqq", 1, full_offset, len(full))
        + bytes(20)
        + struct.pack("<i", len(encoded))
        + bytes(encoded)
        + struct.pack("<i", 0)
    )
    body += primary
    names = b"Zlib".ljust(32, b"\0") + bytes(128)
    footer = (
        struct.pack("<16sBIIQQ", key_guid, int(encrypted_index), MAGIC, version, primary_offset, len(primary))
        + hashlib.sha1(primary).digest()
        + names
    )
    assert len(footer) == FOOTER_SIZE
    return bytes(body) + footer


def pak_files(holds: list[str]) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for name in holds:
        for suffix, extension in (("package", "uasset"), ("exports", "uexp")):
            files[f"{thumb_stem(name)}.{extension}"] = entry_payload(name, suffix)
    files[f"{ITEMS}/BP_Synth_Elsewhere.uasset"] = b"synthetic unrelated package\n"
    return files


def client_paks() -> dict[str, bytes]:
    paks = {}
    for pak, holds in CLIENT_PAKS.items():
        files = pak_files(holds)
        compressed = frozenset(f"{thumb_stem(n)}.uexp" for n in holds if n in COMPRESSED)
        paks[f"{PAKS}/{pak}"] = build_pak(files, compressed=compressed)
    return paks


def appmanifest_text() -> str:
    return (
        '"AppState"\n{\n\t"appid"\t\t"1857950"\n'
        f'\t"buildid"\t\t"{BUILD_ID}"\n\t"InstalledDepots"\n\t{{\n'
        '\t\t"1857952"\n\t\t{\n\t\t\t"manifest"\t\t"4926741467720672671"\n\t\t}\n\t}\n}\n'
    )


def write_client(root: Path) -> None:
    """The synthetic client install under ``root`` (``common/Vein`` layout is not needed; paks sit at ``root``)."""
    for name, payload in client_paks().items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
    (root / "appmanifest_1857950.acf").write_text(appmanifest_text(), encoding="utf-8")


def write_server(root: Path) -> None:
    """The fake DepotDownloader root: ``head.json`` and the depot tree at ``MANIFEST``."""
    tree = root / "2131402" / MANIFEST / "tree" / PAKS
    tree.mkdir(parents=True, exist_ok=True)
    for name, payload in {**SERVER_PAKS, **SERVER_DECOY}.items():
        (tree / name).write_bytes(payload)
    (root / "head.json").write_text(json.dumps({"2131402": MANIFEST}), encoding="utf-8")


# ------------------------------------------------------------------------------------ decoder outputs


def _parent_tag(spec: dict[str, Any]) -> str:
    if "parent" in spec:
        return f"Class'{blueprint_package(spec['parent'])}.{class_of(spec['parent'])}'"
    return f"Class'/Script/Vein.{spec['root']}'"


def registry_document() -> dict[str, Any]:
    assets = [
        {
            "packageName": blueprint_package(name),
            "assetName": name,
            "assetClass": "Blueprint",
            "tags": {"ParentClass": _parent_tag(spec)},
        }
        for name, spec in sorted(BLUEPRINTS.items())
    ]
    assets.append(
        {
            "packageName": "/Game/Vein/Items/DT_Synth_Table",
            "assetName": "DT_Synth_Table",
            "assetClass": "Blueprint",
            "tags": {"ParentClass": "Class'/Script/Engine.Actor'"},
        }
    )
    return {"registries": ["Vein/AssetRegistry.bin"], "assets": sorted(assets, key=lambda a: a["packageName"])}


def default_object(name: str, spec: dict[str, Any]) -> list[dict[str, Any]]:
    flags = "CLASS_CompiledFromBlueprint" + (" | CLASS_Abstract" if spec.get("abstract") else "")
    properties: dict[str, Any] = {"Weight": 0.5}
    if spec.get("type"):
        properties["Type"] = {"ObjectName": "ItemType'IT_Synth'", "ObjectPath": "/Game/Vein/ItemTypes/IT_Synth"}
    if spec.get("thumb"):
        package = thumb_package(name)
        properties["Thumbnail"] = {"AssetPathName": f"{package}.{package.rsplit('/', 1)[1]}", "SubPathString": ""}
    return [
        {"Type": "BlueprintGeneratedClass", "Name": class_of(name), "ClassFlags": flags},
        {"Type": class_of(name), "Name": f"Default__{class_of(name)}", "Properties": properties},
    ]


def decoder_files() -> dict[str, bytes]:
    index: dict[str, Any] = {"asset-registry": {"file": "json/registry.json", "meta": {"assets": 0}}}
    files: dict[str, bytes] = {"json/registry.json": _json(registry_document())}
    for name, spec in sorted(BLUEPRINTS.items()):
        if spec.get("client-only"):
            continue
        key = f"json:{blueprint_package(name)}"
        index[key] = {"file": f"json/{name}.json", "meta": {"exports": 2}}
        files[f"json/{name}.json"] = _json(default_object(name, spec))
    for holds in CLIENT_PAKS.values():
        for name in holds:
            spec = BLUEPRINTS.get(name)
            key = f"texture:{thumb_package(name)}"
            if spec is None or key in index:
                continue
            if spec["thumb"] == "bad-format":
                index[key] = {"error": "PF_FloatRGBA", "errorKind": "unsupported-format"}
                continue
            index[key] = {
                "file": f"png/{name}.png",
                "meta": {"width": 16, "height": 16, "format": TEXTURE_FORMAT},
                "requires": requirements(name),
            }
            files[f"png/{name}.png"] = _png(tuple(spec["color"]))
    files["index.json"] = _json(index)
    return files


def requirements(name: str) -> list[dict[str, Any]]:
    """The byte ranges of every pak that holds ``name``'s thumbnail, as the planner must have mirrored them."""
    needed = []
    paks = client_paks()
    for pak, holds in CLIENT_PAKS.items():
        if name not in holds:
            continue
        data = paks[f"{PAKS}/{pak}"]
        for suffix in ("package", "exports"):
            payload = entry_payload(name, suffix)
            at = data.index(payload)
            needed.append(
                {
                    "file": f"{PAKS}/{pak}",
                    "offset": at,
                    "length": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
            )
    return needed


def all_files() -> dict[str, bytes]:
    return {f"decoder/{name}": payload for name, payload in decoder_files().items()}


def write(root: Path) -> None:
    for name, payload in all_files().items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)


if __name__ == "__main__":
    write(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE)
