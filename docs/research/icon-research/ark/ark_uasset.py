"""Stdlib-only reader for ARK: Survival Evolved's loose UE4.5-era packages (spike #813).

It proves the item-code to icon-texture chain from data, never from names:

    PrimalGameData_BP CDO -> MasterItemList (class refs) -> item class -> CDO ItemIcon
    (inherited through the parent blueprint) -> Texture2D import -> texture package path

Every property is skipped by its serialised size except the few that are read, which is what
makes the reader tolerant of ARK's custom serialization. It reads the package summary only as far
as the name/import/export table offsets, which sit right after the package flags.

Run it inside the network-less container (run.sh does), as `python -I ark_uasset.py ...`.
Subcommands:
  info PACKAGE...                       summary, names, imports, exports and CDO properties
  codes --root DIR GAMEDATA...          every MasterItemList code per game-data package
  map   --root DIR GAMEDATA...          code -> icon texture package (or the reason it has none)
  textures --root DIR --out DIR PKG...  mip 0 of each Texture2D package as DDS, plus a report
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

PACKAGE_TAG = 0x9E2A83C1
CONTENT_PREFIX = "ShooterGame/Content/"
READ_PROPS = {
    "MasterItemList",
    "ItemIcon",
    "ItemIconMaterialParent",
    "BrokenIcon",
    "bUseItemColors",
    "DescriptiveNameBase",
}
MAX_SUPER_DEPTH = 32


class ParseError(Exception):
    pass


class Reader:
    def __init__(self, data: bytes, pos: int = 0) -> None:
        self.data = data
        self.pos = pos

    def need(self, count: int) -> None:
        if count < 0 or self.pos + count > len(self.data):
            raise ParseError(
                f"read of {count} bytes at {self.pos} runs past {len(self.data)}"
            )

    def unpack(self, fmt: str):
        size = struct.calcsize(fmt)
        self.need(size)
        value = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return value[0] if len(value) == 1 else value

    def i32(self) -> int:
        return self.unpack("<i")

    def u32(self) -> int:
        return self.unpack("<I")

    def i64(self) -> int:
        return self.unpack("<q")

    def u8(self) -> int:
        return self.unpack("<B")

    def skip(self, count: int) -> None:
        self.need(count)
        self.pos += count

    def fstring(self) -> str:
        length = self.i32()
        if length == 0:
            return ""
        if length < 0:
            raw_len = -length * 2
            self.need(raw_len)
            text = self.data[self.pos : self.pos + raw_len - 2].decode("utf-16-le")
            self.pos += raw_len
            return text
        self.need(length)
        text = self.data[self.pos : self.pos + length - 1].decode("latin-1")
        self.pos += length
        return text


@dataclass
class Export:
    index: int
    class_index: int
    super_index: int
    outer_index: int
    name: str
    serial_size: int
    serial_offset: int


@dataclass
class Import:
    class_package: str
    class_name: str
    outer_index: int
    name: str


@dataclass
class Tag:
    name: str
    type: str
    size: int
    array_index: int
    offset: int
    extra: dict = field(default_factory=dict)


class Package:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        reader = Reader(self.data)
        if reader.u32() != PACKAGE_TAG:
            raise ParseError(f"{path}: not a UE4 package")
        legacy = reader.i32()
        if legacy >= 0:
            raise ParseError(
                f"{path}: legacy file version {legacy} is not the UE4 layout"
            )
        reader.i32()  # legacy UE3 version
        self.file_version = reader.i32()
        self.licensee_version = reader.i32()
        for _ in range(reader.i32()):  # custom versions: guid + version
            reader.skip(20)
        self.header_size = reader.i32()
        reader.fstring()  # folder name
        self.package_flags = reader.u32()
        name_count, name_offset = reader.i32(), reader.i32()
        export_count, export_offset = reader.i32(), reader.i32()
        import_count, import_offset = reader.i32(), reader.i32()
        depends_offset = reader.i32()
        self.names = self._read_names(name_count, name_offset)
        self.imports = self._read_imports(import_count, import_offset)
        self.exports = self._read_exports(export_count, export_offset, depends_offset)

    def _read_names(self, count: int, offset: int) -> list[str]:
        reader = Reader(self.data, offset)
        names = []
        for _ in range(count):
            names.append(reader.fstring())
        return names

    def fname(self, reader: Reader) -> str:
        index, number = reader.i32(), reader.i32()
        if not 0 <= index < len(self.names):
            raise ParseError(
                f"{self.path}: name index {index} outside {len(self.names)} names at {reader.pos - 8}"
            )
        name = self.names[index]
        return f"{name}_{number - 1}" if number else name

    def _read_imports(self, count: int, offset: int) -> list[Import]:
        reader = Reader(self.data, offset)
        imports = []
        for _ in range(count):
            class_package = self.fname(reader)
            class_name = self.fname(reader)
            outer = reader.i32()
            imports.append(Import(class_package, class_name, outer, self.fname(reader)))
        return imports

    def _read_exports(self, count: int, offset: int, end: int) -> list[Export]:
        if count == 0:
            return []
        stride = (end - offset) // count
        if stride < 64:
            raise ParseError(f"{self.path}: export stride {stride} too small")
        exports = []
        for index in range(count):
            reader = Reader(self.data, offset + index * stride)
            class_index, super_index, outer_index = (
                reader.i32(),
                reader.i32(),
                reader.i32(),
            )
            name = self.fname(reader)
            reader.u32()  # object flags
            size, serial_offset = reader.i64(), reader.i64()
            exports.append(
                Export(
                    index,
                    class_index,
                    super_index,
                    outer_index,
                    name,
                    size,
                    serial_offset,
                )
            )
        return exports

    def import_path(self, ref: int) -> tuple[str, str]:
        """Return (package path, object name) of a negative package index."""
        entry = self.imports[-ref - 1]
        outer = entry.outer_index
        guard = 0
        while outer < 0 and self.imports[-outer - 1].class_name != "Package":
            outer = self.imports[-outer - 1].outer_index
            guard += 1
            if guard > MAX_SUPER_DEPTH:
                raise ParseError(f"{self.path}: import outer chain too deep")
        package = self.imports[-outer - 1].name if outer < 0 else ""
        return package, entry.name

    def read_tag(self, reader: Reader) -> Tag | None:
        name = self.fname(reader)
        if name == "None":
            return None
        start = reader.pos
        type_name = self.fname(reader)
        size, array_index = reader.i32(), reader.i32()
        extra: dict = {}
        if type_name == "StructProperty":
            extra["struct"] = self.fname(reader)
        elif type_name == "BoolProperty":
            extra["bool"] = reader.u8()
            if extra["bool"] not in (0, 1):
                raise ParseError(
                    f"Invalid bool value {extra['bool']} for {name} at offset {reader.pos - 1}"
                )
        elif type_name in ("ByteProperty", "EnumProperty"):
            extra["enum"] = self.fname(reader)
        elif type_name == "ArrayProperty":
            extra["inner"] = self.fname(reader)
        # Unlike later UE4 builds, this layout has no HasPropertyGuid byte after the type extras.
        if size < 0:
            raise ParseError(f"negative size {size} for {name} at offset {start}")
        return Tag(name, type_name, size, array_index, reader.pos, extra)

    def properties(self, export: Export) -> tuple[dict[str, dict], list[str]]:
        """Read the wanted properties of one export, skipping every other property by its size."""
        reader = Reader(self.data, export.serial_offset)
        end = export.serial_offset + export.serial_size
        wanted: dict[str, dict] = {}
        seen: list[str] = []
        while reader.pos < end:
            tag = self.read_tag(reader)
            if tag is None:
                break
            seen.append(tag.name)
            value_end = tag.offset + tag.size
            if tag.name in READ_PROPS:
                wanted[tag.name] = self._value(tag, Reader(self.data, tag.offset))
            reader.pos = value_end
        return wanted, seen

    def _value(self, tag: Tag, reader: Reader) -> dict:
        if tag.type == "ObjectProperty":
            return {"ref": reader.i32()}
        if tag.type == "BoolProperty":
            return {"value": bool(tag.extra["bool"])}
        if tag.type == "ArrayProperty":
            count = reader.i32()
            return {"refs": [reader.i32() for _ in range(count)]}
        if tag.type == "TextProperty":
            return {"text": self._text(reader)}
        return {"raw_size": tag.size}

    @staticmethod
    def _text(reader: Reader) -> str | None:
        reader.u32()  # flags
        reader.u8()  # history type (Base)
        if reader.pos + 4 > len(reader.data):
            return None
        reader.fstring()  # namespace
        reader.fstring()  # key
        return reader.fstring()  # source string

    def export_named(self, name: str) -> Export | None:
        return next((export for export in self.exports if export.name == name), None)

    def ref_target(self, ref: int) -> tuple[str, str] | None:
        """Resolve an object reference to (package path, object name); exports stay in this package."""
        if ref < 0:
            return self.import_path(ref)
        if ref > 0:
            return self.path_to_game(), self.exports[ref - 1].name
        return None

    def path_to_game(self) -> str:
        text = self.path.as_posix()
        marker = CONTENT_PREFIX
        return "/Game/" + text[text.index(marker) + len(marker) :].removesuffix(
            ".uasset"
        )


def game_path_to_file(root: Path, package_path: str) -> Path:
    if not package_path.startswith("/Game/"):
        raise ParseError(f"not a /Game/ package path: {package_path}")
    return root / CONTENT_PREFIX / (package_path.removeprefix("/Game/") + ".uasset")


def item_code(package_path: str, class_name: str) -> str:
    return f"{package_path}.{class_name}"


class Catalog:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.cache: dict[Path, Package] = {}

    def package(self, path: Path) -> Package:
        if path not in self.cache:
            self.cache[path] = Package(path)
        return self.cache[path]

    def master_item_codes(self, gamedata: Path) -> list[str]:
        """Collect every MasterItemList entry of one game-data package, following its parents."""
        package = self.package(gamedata)
        codes: list[str] = []
        for export in package.exports:
            if not export.name.startswith("Default__"):
                continue
            wanted, _ = package.properties(export)
            for ref in wanted.get("MasterItemList", {}).get("refs", []):
                target = package.ref_target(ref)
                if target:
                    codes.append(item_code(*target))
        return codes

    def icon_for(self, code: str) -> dict:
        package_path, class_name = code.rsplit(".", 1)
        chain: list[str] = []
        for _ in range(MAX_SUPER_DEPTH):
            file = game_path_to_file(self.root, package_path)
            if not file.exists():
                return {
                    "icon": None,
                    "reason": "item-package-missing",
                    "package": package_path,
                    "chain": chain,
                }
            package = self.package(file)
            chain.append(item_code(package_path, class_name))
            cdo = package.export_named(f"Default__{class_name}")
            klass = package.export_named(class_name)
            if cdo is None or klass is None:
                return {
                    "icon": None,
                    "reason": "no-class-or-cdo-export",
                    "chain": chain,
                }
            wanted, _ = package.properties(cdo)
            icon = wanted.get("ItemIcon", {}).get("ref")
            if icon:
                target = package.ref_target(icon)
                result = {
                    "icon": target[0] if target else None,
                    "object": target[1] if target else None,
                    "chain": chain,
                }
                result.update(_icon_extras(package, wanted))
                return result
            if "ItemIconMaterialParent" in wanted:
                extras = _icon_extras(package, wanted)
                return {
                    "icon": None,
                    "reason": "material-icon-only",
                    "chain": chain,
                    **extras,
                }
            parent = package.ref_target(klass.super_index)
            if parent is None or parent[0].startswith("/Script/"):
                return {"icon": None, "reason": "no-icon-in-game", "chain": chain}
            package_path, class_name = parent
        return {"icon": None, "reason": "super-chain-too-deep", "chain": chain}


def _icon_extras(package: Package, wanted: dict) -> dict:
    extras: dict = {}
    for key in ("ItemIconMaterialParent", "BrokenIcon"):
        ref = wanted.get(key, {}).get("ref")
        if ref:
            target = package.ref_target(ref)
            extras[key] = target[0] if target else None
    if "bUseItemColors" in wanted:
        extras["bUseItemColors"] = wanted["bUseItemColors"]["value"]
    return extras


def cmd_info(args: argparse.Namespace) -> None:
    for name in args.packages:
        package = Package(Path(name))
        out = {
            "file": name,
            "bytes": len(package.data),
            "fileVersionUE4": package.file_version,
            "licenseeVersion": package.licensee_version,
            "headerSize": package.header_size,
            "names": len(package.names),
            "imports": [f"{i.class_name} {i.name}" for i in package.imports],
            "exports": [],
        }
        for export in package.exports:
            entry = {
                "name": export.name,
                "super": export.super_index,
                "size": export.serial_size,
            }
            if export.name.startswith("Default__"):
                wanted, seen = package.properties(export)
                entry["wanted"] = wanted
                entry["properties"] = seen
            out["exports"].append(entry)
        json.dump(out, sys.stdout, indent=1)
        print()


def write_json(result: object, out: str | None) -> None:
    if out:
        Path(out).write_text(json.dumps(result, indent=0))
    else:
        json.dump(result, sys.stdout)


def cmd_codes(args: argparse.Namespace) -> None:
    catalog = Catalog(Path(args.root))
    result = {
        gamedata: catalog.master_item_codes(Path(args.root) / gamedata)
        for gamedata in args.gamedata
    }
    write_json(result, args.out)


CODE_PATTERN = re.compile(r"^/Game/[A-Za-z0-9/_.]+_C$")


def coverage_report(mapping: dict[str, dict], listing: dict[str, dict]) -> dict:
    """Count codes by outcome against a depot listing (paths only), and check the connector's code rules."""
    depot_packages = {
        "/Game/" + path.removeprefix(CONTENT_PREFIX).removesuffix(".uasset")
        for path in listing
        if path.startswith(CONTENT_PREFIX) and path.endswith(".uasset")
    }
    outcomes: dict[str, int] = {}
    for entry in mapping.values():
        if entry.get("icon"):
            key = (
                "resolved"
                if entry["icon"] in depot_packages
                else "texture-missing-from-depot"
            )
        else:
            key = entry["reason"]
        outcomes[key] = outcomes.get(key, 0) + 1
    return {
        "codes": len(mapping),
        "outcomes": outcomes,
        "inheritedIcon": sum(
            1 for e in mapping.values() if e.get("icon") and len(e["chain"]) > 1
        ),
        "iconAndMaterialParent": sum(
            1
            for e in mapping.values()
            if e.get("icon") and e.get("ItemIconMaterialParent")
        ),
        "usesItemColors": sum(1 for e in mapping.values() if e.get("bUseItemColors")),
        "validatorRejected": sorted(
            code for code in mapping if not CODE_PATTERN.match(code)
        ),
        "longestCode": max(len(code) for code in mapping),
        "byTopDirectory": {
            top: sum(1 for code in mapping if code.split("/")[2] == top)
            for top in sorted({c.split("/")[2] for c in mapping})
        },
    }


def cmd_map(args: argparse.Namespace) -> None:
    catalog = Catalog(Path(args.root))
    result: dict[str, dict] = {}
    for gamedata in args.gamedata:
        for code in catalog.master_item_codes(Path(args.root) / gamedata):
            result.setdefault(code, {"gamedata": []})["gamedata"].append(gamedata)
    for code, entry in result.items():
        entry.update(catalog.icon_for(code))
    write_json(result, args.out)
    if args.listing:
        coverage = coverage_report(result, json.loads(Path(args.listing).read_text()))
        write_json(coverage, str(Path(args.out).with_name("coverage.json")))


# --- Texture2D (the "Go C" minimal reader) ---------------------------------------------------------
# Layout observed in this build's client packages, after the tagged properties of a Texture2D export:
#   i32 guid flag; 4 strip-flag bytes (UTexture, UTexture2D); i32 bCooked; FName pixel format
#   (None when the data was stripped); i64 skip offset; i32 sizeX, sizeY, slots; FString format;
#   i32 firstMip; i32 numMips; per mip: i32 cooked, i32 flags, i32 element count, i32 size on disk,
#   i64 offset, inline payload when the payload is not at the end of the file, i32 sizeX, i32 sizeY.
# The offset of an at-end-of-file payload is relative to the end of the last export.
BULK_PAYLOAD_AT_END_OF_FILE = 0x1
FOURCC = {"PF_DXT1": b"DXT1", "PF_DXT3": b"DXT3", "PF_DXT5": b"DXT5"}
BLOCK_BYTES = {"PF_DXT1": 8, "PF_DXT3": 16, "PF_DXT5": 16}


@dataclass
class Mip:
    flags: int
    size: int
    offset: int
    width: int
    height: int
    data: bytes


def read_texture_payload(package: Package) -> tuple[dict, list[Mip]]:
    export = next(
        (
            e
            for e in package.exports
            if e.class_index < 0
            and package.imports[-e.class_index - 1].name == "Texture2D"
        ),
        None,
    )
    if export is None:
        return {"status": "no-texture2d-export"}, []
    reader = Reader(package.data, export.serial_offset)
    imported = None
    while True:
        tag = package.read_tag(reader)
        if tag is None:
            break
        if tag.name == "ImportedSize":
            imported = struct.unpack_from("<ii", package.data, tag.offset)
        reader.pos = tag.offset + tag.size
    meta: dict = {"importedSize": imported, "packageBytes": len(package.data)}
    reader.i32()  # guid flag
    strip = [reader.u8() for _ in range(4)]
    meta["stripFlags"] = strip
    meta["cooked"] = reader.i32()
    meta["pixelFormatName"] = package.fname(reader)
    if meta["pixelFormatName"] == "None":
        meta["status"] = "no-pixel-data"
        return meta, []
    reader.i64()  # skip offset
    width, height, _slots = reader.i32(), reader.i32(), reader.i32()
    meta.update(width=width, height=height, pixelFormat=reader.fstring())
    reader.i32()  # first mip
    count = reader.i32()
    if not 0 < count <= 16:
        raise ParseError(
            f"{package.path}: implausible mip count {count} at {reader.pos - 4}"
        )
    bulk_base = max(e.serial_offset + e.serial_size for e in package.exports)
    mips = []
    for _ in range(count):
        reader.i32()  # cooked
        flags, elements, size = reader.i32(), reader.i32(), reader.i32()
        offset = reader.i64()
        if flags & BULK_PAYLOAD_AT_END_OF_FILE:
            start = bulk_base + offset
            data = package.data[start : start + size]
        else:
            data = package.data[offset : offset + size]
            reader.skip(size)
        mip_width, mip_height = reader.i32(), reader.i32()
        if len(data) != size or elements < 0:
            raise ParseError(
                f"{package.path}: mip payload of {size} bytes does not fit the file"
            )
        mips.append(Mip(flags, size, offset, mip_width, mip_height, data))
    meta["mips"] = [
        {"width": m.width, "height": m.height, "bytes": m.size, "flags": m.flags}
        for m in mips
    ]
    meta["status"] = "ok"
    return meta, mips


def dds_header(width: int, height: int, pixel_format: str, size: int) -> bytes:
    if pixel_format in FOURCC:
        pixel = struct.pack("<II4sIIIII", 32, 0x4, FOURCC[pixel_format], 0, 0, 0, 0, 0)
        flags, pitch = 0x1007 | 0x80000, size
    elif pixel_format == "PF_B8G8R8A8":
        pixel = struct.pack(
            "<IIIIIIII", 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000
        )
        flags, pitch = 0x1007 | 0x8, width * 4
    else:
        raise ParseError(f"no DDS mapping for {pixel_format}")
    header = (
        struct.pack("<7I", 124, flags, height, width, pitch, 0, 1) + bytes(44) + pixel
    )
    return b"DDS " + header + struct.pack("<5I", 0x1000, 0, 0, 0, 0)


def cmd_textures(args: argparse.Namespace) -> None:
    """Write mip 0 of each texture package as a DDS file and report what each package holds."""
    root, out = Path(args.root), Path(args.out)
    report = {}
    for package_path in args.packages:
        file = game_path_to_file(root, package_path)
        entry: dict = {
            "file": str(file.relative_to(root)),
            "sha256": hashlib.sha256(file.read_bytes()).hexdigest(),
        }
        meta, mips = read_texture_payload(Package(file))
        entry.update(meta)
        if mips:
            top = max(mips, key=lambda mip: mip.size)
            target = out / "dds" / (package_path.removeprefix("/Game/") + ".dds")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(
                dds_header(top.width, top.height, meta["pixelFormat"], top.size)
                + top.data
            )
        report[package_path] = entry
    write_json(report, str(out / "textures.json"))


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(required=True)
    info = sub.add_parser("info")
    info.add_argument("packages", nargs="+")
    info.set_defaults(func=cmd_info)
    for name, func in (("codes", cmd_codes), ("map", cmd_map)):
        cmd = sub.add_parser(name)
        cmd.add_argument(
            "--root", required=True, help="directory that contains ShooterGame/Content"
        )
        cmd.add_argument("--out")
        if name == "map":
            cmd.add_argument(
                "--listing",
                help="depot listing JSON ({path: {size}}); writes coverage.json next to --out",
            )
        cmd.add_argument(
            "gamedata", nargs="+", help="game-data package path relative to --root"
        )
        cmd.set_defaults(func=func)
    textures = sub.add_parser("textures")
    textures.add_argument(
        "--root", required=True, help="directory that contains ShooterGame/Content"
    )
    textures.add_argument(
        "--out", required=True, help="directory that receives dds/ and textures.json"
    )
    textures.add_argument(
        "packages", nargs="+", help="texture package paths, /Game/..."
    )
    textures.set_defaults(func=cmd_textures)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
