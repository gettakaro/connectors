#!/usr/bin/env python3
"""Generate the synthetic parts of the Conan Exiles fixtures: a tiny IoStore client, the pictures the decoder
stand-in returns, the stand-in's index and a server depot.

    make_fixture.py [DEST]            (default: this directory)

What it reads is ``selection.json`` and ``decoder/json/*.json``, the real decoder output trimmed by
``record.py``. What it writes (``client/``, ``depots/``, ``decoder/png/``, ``decoder/index.json``) is
deterministic, and a test regenerates it and compares the bytes with the committed files.

The client holds no game art. Its containers are made of block-aligned patterns of known bytes, laid out
the way Unreal's IoStore does (a table of contents with a directory index, blocks of ``BLOCK_SIZE`` bytes,
one container split over two ``.ucas`` partitions), and the decoder stand-in checks that the byte ranges
the planner copied are the ones holding each package.
"""

from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
BLOCK_SIZE = 4096
PARTITION_SIZE = 2 * BLOCK_SIZE
UNBOUNDED_PARTITION = 0xFFFFFFFFFFFFFFFF
PAKS = "ConanSandbox/Content/Paks"
MANIFEST = "7440000000000000043"
SERVER_DEPOT = "443032"
CLIENT_BUILD = 25791535
GAME_PREFIX = "ConanSandbox/Content/"
PLUGIN_STEM = "ConanSandbox/Plugins/DLC_Fixture/Content/UI/icon_plugin_only"
FILLER_STEM = "ConanSandbox/Content/Fixture/filler"
FILLER_BLOCKS = 17
MAGIC = b"-==--==--==--==-"
INVALID = 0xFFFFFFFF
TYPE_EXPORT_BUNDLE, TYPE_BULK_DATA, TYPE_CONTAINER_HEADER = 2, 3, 6

SOLID = "solid"
ROLE_SHAPES = {"battlepass": (500, 500), "small": (127, 128)}


def pattern(label: str, length: int) -> bytes:
    """``length`` bytes that are a function of ``label`` alone, so every package has recognisable contents."""
    out = bytearray()
    counter = 0
    while len(out) < length:
        out += hashlib.sha256(f"{label}|{counter}".encode()).digest()
        counter += 1
    return bytes(out[:length])


def color_for(package: str) -> tuple[int, int, int, int]:
    digest = hashlib.sha256(package.encode()).digest()
    return (40 + digest[0] % 180, 40 + digest[1] % 180, 40 + digest[2] % 180, 255)


# ----------------------------------------------------------------------------------- the IoStore writer


class Container:
    def __init__(self, name: str, mount: str, partitions: int) -> None:
        self.name = name
        self.mount = mount
        self.partitions = partitions
        self.files: list[tuple[str, bytes, int]] = []

    def add(self, path: str, data: bytes, chunk_type: int) -> None:
        self.files.append((path, data, chunk_type))

    @property
    def container_id(self) -> int:
        return int.from_bytes(hashlib.sha256(self.name.encode()).digest()[:8], "little")

    def layout(self) -> dict[str, Any]:
        header_chunk = pattern(f"{self.name}|container-header", 512)
        chunks: list[tuple[bytes, bytes, str | None]] = [
            (struct.pack("<QHBB", self.container_id, 0, 0, TYPE_CONTAINER_HEADER), header_chunk, None)
        ]
        for path, data, chunk_type in self.files:
            ident = hashlib.sha256(path.encode()).digest()[:8]
            chunks.append((ident + struct.pack("<HBB", 0, 0, chunk_type), data, path))
        stream = bytearray()
        offsets = []
        for _, data, _ in chunks:
            stream += b"\0" * (-len(stream) % BLOCK_SIZE)
            offsets.append((len(stream), len(data)))
            stream += data
        stream += b"\0" * (-len(stream) % BLOCK_SIZE)
        return {"chunks": chunks, "offsets": offsets, "stream": bytes(stream)}

    def write(self, directory: Path) -> dict[str, list[dict[str, Any]]]:
        """Write ``.utoc`` and the ``.ucas`` partitions; return, per file path, the byte ranges holding it."""
        layout = self.layout()
        stream: bytes = layout["stream"]
        blocks = len(stream) // BLOCK_SIZE
        partition_size = PARTITION_SIZE if self.partitions > 1 else UNBOUNDED_PARTITION
        if self.partitions > 1:
            assert blocks * BLOCK_SIZE == self.partitions * PARTITION_SIZE, (self.name, blocks)
        stem = self.name
        parts = [bytearray() for _ in range(self.partitions)]
        for block in range(blocks):
            at = block * BLOCK_SIZE
            parts[at // partition_size if self.partitions > 1 else 0] += stream[at : at + BLOCK_SIZE]
        for index, content in enumerate(parts):
            (directory / (f"{stem}.ucas" if index == 0 else f"{stem}_s{index}.ucas")).write_bytes(bytes(content))
        (directory / f"{stem}.utoc").write_bytes(self._toc(layout, blocks, partition_size))
        ranges: dict[str, list[dict[str, Any]]] = {}
        for (_, _data, path), (offset, length) in zip(layout["chunks"], layout["offsets"], strict=True):
            if path is None:
                continue
            for block in range(offset // BLOCK_SIZE, (offset + length - 1) // BLOCK_SIZE + 1):
                at = block * BLOCK_SIZE
                partition = at // PARTITION_SIZE if self.partitions > 1 else 0
                ranges.setdefault(path, []).append(
                    {
                        "file": f"{PAKS}/{stem}.ucas" if partition == 0 else f"{PAKS}/{stem}_s{partition}.ucas",
                        "offset": at % PARTITION_SIZE if self.partitions > 1 else at,
                        "length": BLOCK_SIZE,
                        "sha256": hashlib.sha256(stream[at : at + BLOCK_SIZE]).hexdigest(),
                    }
                )
        return ranges

    def _toc(self, layout: dict[str, Any], blocks: int, partition_size: int) -> bytes:
        chunks = layout["chunks"]
        directory = self._directory_index()
        header = (
            MAGIC
            + struct.pack("<BBH", 8, 0, 0)
            + struct.pack(
                "<IIIIIIIIIQ",
                144,
                len(chunks),
                blocks,
                12,
                1,
                32,
                BLOCK_SIZE,
                len(directory),
                self.partitions,
                self.container_id,
            )
            + b"\0" * 16
            + bytes([1 | 8, 0, 0, 0])
            + struct.pack("<IQI", 0, partition_size, 0)
            + b"\0" * 44
        )
        assert len(header) == 144, len(header)
        body = b"".join(chunk_id for chunk_id, _, _ in chunks)
        body += b"".join(offset.to_bytes(5, "big") + length.to_bytes(5, "big") for offset, length in layout["offsets"])
        for block in range(blocks):
            body += (block * BLOCK_SIZE).to_bytes(5, "little") + BLOCK_SIZE.to_bytes(3, "little")
            body += BLOCK_SIZE.to_bytes(3, "little") + bytes([0])
        body += b"None".ljust(32, b"\0")
        return header + body + directory

    def _directory_index(self) -> bytes:
        strings: list[str] = []

        def string(text: str) -> int:
            if text not in strings:
                strings.append(text)
            return strings.index(text)

        tree: dict[str, Any] = {}
        for chunk_index, (path, _, _) in enumerate(self.files, start=1):
            relative = path.removeprefix(self.mount.removeprefix("../../../"))
            node = tree
            *directories, file_name = relative.split("/")
            for directory in directories:
                node = node.setdefault(directory, {})
            node.setdefault("\0files", []).append((file_name, chunk_index))
        entries: list[list[int]] = []
        files: list[list[int]] = []

        def build(node: dict[str, Any], name: int) -> int:
            index = len(entries)
            entries.append([name, INVALID, INVALID, INVALID])
            previous = INVALID
            for file_name, chunk_index in node.get("\0files", []):
                position = len(files)
                files.append([string(file_name), INVALID, chunk_index])
                if previous == INVALID:
                    entries[index][3] = position
                else:
                    files[previous][1] = position
                previous = position
            previous_child = INVALID
            for child_name in sorted(key for key in node if key != "\0files"):
                child = build(node[child_name], string(child_name))
                if previous_child == INVALID:
                    entries[index][1] = child
                else:
                    entries[previous_child][2] = child
                previous_child = child
            return index

        build(tree, INVALID)
        out = bytearray()
        out += struct.pack("<i", len(self.mount) + 1) + self.mount.encode("ascii") + b"\0"
        out += struct.pack("<I", len(entries)) + b"".join(struct.pack("<IIII", *entry) for entry in entries)
        out += struct.pack("<I", len(files)) + b"".join(struct.pack("<III", *entry) for entry in files)
        out += struct.pack("<I", len(strings))
        for text in strings:
            out += struct.pack("<i", len(text) + 1) + text.encode("ascii") + b"\0"
        return bytes(out)


# ----------------------------------------------------------------------------------------- the pictures


def solid(size: tuple[int, int], color: tuple[int, int, int, int]) -> Image.Image:
    return Image.new("RGBA", size, color)


def base_art(color: tuple[int, int, int, int]) -> Image.Image:
    image = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    ImageDraw.Draw(image).rectangle((8, 8, 55, 55), fill=color)
    return image


BORDER_COLOR = (255, 215, 0, 255)


def border_art() -> Image.Image:
    image = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, 63, 63), outline=BORDER_COLOR, width=4)
    return image


def mask_art() -> Image.Image:
    image = Image.new("L", (64, 64))
    image.putdata([(x * 4) % 256 for _y in range(64) for x in range(64)])
    return image


# ---------------------------------------------------------------------------------------- the generator


def soft_packages(row: dict[str, Any]) -> list[str]:
    paths = [row["Icon"]["AssetPathName"]] if row["Icon"]["AssetPathName"] else []
    paths += [layer["AssetPathName"] for layer in row["IconLayers"]]
    return [path.split(".")[0] for path in paths]


def read_selection() -> dict[str, Any]:
    return json.loads((HERE / "selection.json").read_text())  # type: ignore[no-any-return]


def item_rows() -> dict[str, dict[str, Any]]:
    """Template id (or row name) to row, over the recorded item tables: base first."""
    rows: dict[str, dict[str, Any]] = {}
    for name in ("item-table", "dlc-item-table", "composite-item-table"):
        table = json.loads((HERE / "decoder" / "json" / f"{name}.json").read_text())[0]
        for row_name, row in table["Rows"].items():
            rows.setdefault(row_name, row)
    return rows


def role_packages() -> dict[str, list[str]]:
    selection = read_selection()["roles"]
    rows = item_rows()
    return {
        role: [pkg for name in names for pkg in soft_packages(rows[name])]
        for role, names in selection.items()
        if role != "composite"
    }


def all_packages() -> list[str]:
    packages: dict[str, None] = {}
    for table in ("item-table", "dlc-item-table", "composite-item-table"):
        document = json.loads((HERE / "decoder" / "json" / f"{table}.json").read_text())[0]
        for row in document["Rows"].values():
            for package in soft_packages(row):
                packages.setdefault(package)
    return sorted(packages)


def stem_of(package: str) -> str:
    return GAME_PREFIX + package.removeprefix("/Game/")


def generate(dest: Path) -> None:
    roles = role_packages()
    packages = all_packages()
    missing = set(roles["missing"])
    unsupported = set(roles["unsupported"])
    mask = set(roles["mask"])
    border = {pkg for pkg in packages if pkg.endswith("epic_icon_border")}
    present = [pkg for pkg in packages if pkg not in missing]

    png = dest / "decoder" / "png"
    png.mkdir(parents=True, exist_ok=True)
    index: dict[str, Any] = {
        "asset-registry": {"file": "registry.json", "meta": {}},
    }
    containers = {
        "pakchunk1000-Windows": Container("pakchunk1000-Windows", "../../../ConanSandbox/Content/", 1),
        "pakchunk1010-Windows": Container("pakchunk1010-Windows", "../../../", 2),
    }
    first, second = containers["pakchunk1000-Windows"], containers["pakchunk1010-Windows"]
    for package in present:
        target = second if package in mask else first
        asset = stem_of(package) + ".uasset"
        target.add(asset, pattern(f"uasset|{package}", 1500), TYPE_EXPORT_BUNDLE)
        if package in mask:
            target.add(asset.replace(".uasset", ".ubulk"), pattern(f"ubulk|{package}", 3000), TYPE_BULK_DATA)
    first.add(FILLER_STEM + ".uasset", pattern("filler", FILLER_BLOCKS * BLOCK_SIZE - 100), TYPE_EXPORT_BUNDLE)
    second.add(PLUGIN_STEM + ".uasset", pattern("plugin", 800), TYPE_EXPORT_BUNDLE)

    paks = dest / "client" / PAKS
    paks.mkdir(parents=True, exist_ok=True)
    (paks / "global.utoc").write_bytes(_global_toc())
    (paks / "global.ucas").write_bytes(pattern("global", 64))
    ranges: dict[str, list[dict[str, Any]]] = {}
    for container in containers.values():
        written = container.write(paks)
        ranges.update(written)
    (dest / "client" / "appmanifest_440900.acf").write_text(_appmanifest())

    for position, package in enumerate(packages):
        slug = f"{position:03d}-{package.rsplit('/', 1)[-1]}"
        if package in missing:
            continue
        if package in unsupported:
            index[f"texture:{package}"] = {"error": "PF_ASTC_4x4", "errorKind": "unsupported-format"}
            continue
        if package in border:
            image: Image.Image = border_art()
        elif package in mask:
            image = mask_art()
        elif package in roles["battlepass"]:
            image = solid(ROLE_SHAPES["battlepass"], color_for(package))
        elif package in roles["small"]:
            image = solid(ROLE_SHAPES["small"], color_for(package))
        elif package in roles["layers"] or any(
            package in soft_packages(item_rows()[name]) for name in read_selection()["roles"]["layers"]
        ):
            image = base_art(color_for(package))
        else:
            image = solid((64, 64), color_for(package))
        image.save(png / f"{slug}.png", format="PNG")
        needed = ranges[stem_of(package) + ".uasset"] + ranges.get(stem_of(package) + ".ubulk", [])
        index[f"texture:{package}"] = {
            "file": f"png/{slug}.png",
            "meta": {
                "width": image.size[0],
                "height": image.size[1],
                "format": "PF_BC4" if package in mask else "PF_DXT5",
                "mip": 0,
                "bulk": "bulk" if package in mask else "inline",
            },
            "requires": needed,
        }
    for table, file in (
        ("/Game/Items/ItemTable", "item-table.json"),
        ("/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToTemplateID", "name-table.json"),
        ("/Game/DLC/DLC_Pict/DLC_Pict_ItemTable", "dlc-item-table.json"),
        ("/Game/DLC/DLC_Pict/DLC_Pict_ItemNameToTemplateID", "dlc-name-table.json"),
        ("/Game/DLC/Special/Special_ItemTable_Composite", "composite-item-table.json"),
    ):
        index[f"json:{table}"] = {"file": f"json/{file}", "meta": {}}
    (dest / "decoder" / "index.json").write_text(json.dumps(index, indent=2, sort_keys=True) + "\n")
    _depots(dest)


def _global_toc() -> bytes:
    header = (
        MAGIC
        + struct.pack("<BBH", 8, 0, 0)
        + struct.pack("<IIIIIIIIIQ", 144, 0, 0, 12, 1, 32, BLOCK_SIZE, 0, 1, 1)
        + b"\0" * 16
        + bytes([0, 0, 0, 0])
        + struct.pack("<IQI", 0, UNBOUNDED_PARTITION, 0)
        + b"\0" * 44
    )
    return header + b"None".ljust(32, b"\0")


def _appmanifest() -> str:
    return (
        '"AppState"\n{\n\t"appid"\t\t"440900"\n\t"name"\t\t"Conan Exiles Enhanced"\n'
        f'\t"buildid"\t\t"{CLIENT_BUILD}"\n\t"InstalledDepots"\n\t{{\n\t\t"440901"\n\t\t{{\n'
        '\t\t\t"manifest"\t\t"2054081959462269381"\n\t\t\t"size"\t\t"61800000000"\n\t\t}\n\t}\n}\n'
    )


def server_files() -> dict[str, bytes]:
    """The depot's files: those the selectors name, and two they must leave out."""
    return {
        f"{PAKS}/global.utoc": b"synthetic server global.utoc\n",
        f"{PAKS}/global.ucas": b"synthetic server global.ucas\n",
        f"{PAKS}/pakchunk0-LinuxServer.pak": b"synthetic pakchunk0 pak (holds AssetRegistry.bin)\n",
        f"{PAKS}/pakchunk0-LinuxServer.utoc": b"synthetic pakchunk0 utoc\n",
        f"{PAKS}/pakchunk0-LinuxServer.ucas": b"synthetic pakchunk0 ucas\n",
        f"{PAKS}/pakchunk5150-LinuxServer.utoc": b"synthetic pakchunk5150 utoc\n",
        f"{PAKS}/pakchunk5150-LinuxServer.ucas": b"synthetic pakchunk5150 ucas\n",
        f"{PAKS}/pakchunk1030-LinuxServer.ucas": b"left out: a container the selectors do not name\n",
        "ConanSandbox/Binaries/Linux/ConanSandboxServer": b"left out: not a container\n",
    }


def _depots(dest: Path) -> None:
    depots = dest / "depots"
    (depots / SERVER_DEPOT / MANIFEST / "tree").mkdir(parents=True, exist_ok=True)
    for name, data in server_files().items():
        target = depots / SERVER_DEPOT / MANIFEST / "tree" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (depots / "head.json").write_text(json.dumps({SERVER_DEPOT: MANIFEST}, indent=2) + "\n")


def main(argv: list[str]) -> int:
    generate(Path(argv[0]) if argv else HERE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
