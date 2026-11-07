"""Reader for Project Zomboid texture packs (``media/texturepacks/*.pack``), layouts v1 and v2.

All integers are little-endian int32 and a string is an int32 byte length followed by that many
bytes, read as latin-1. A pack is a list of pages; a page is a PNG atlas plus the named rectangles
cut from it::

    v2:  "PZPK"  version(=1)  numPages  page*       page = header  pngLength  PNG
    v1:  numPages  page*                             page = header  PNG  0xDEADBEEF
    header = name  numEntries  mask  (name x y w h ox oy fw fh)*

The packer trimmed each entry from an original ``fw x fh`` frame, and ``(ox, oy)`` is where the
trimmed ``w x h`` rectangle sat inside that frame, so a texture is rebuilt as a transparent frame
with the crop pasted at that offset.

Indexing reads names and rectangles only; a page's pixels are decoded when one of its entries is
asked for, so a 300 MB tile pack is never decoded wholesale.
"""

from __future__ import annotations

import io
import struct
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO

from PIL import Image, UnidentifiedImageError

from ...exit_codes import BuildFailed, UsageError
from ...paths import safe_relative

DECODER_ID = "pz-texturepack"
DECODER_VERSION = 1
DECODER_DESCRIPTION = f"{DECODER_ID}/{DECODER_VERSION} (in-process, no container)"

MAGIC = b"PZPK"
V2_VERSION = 1
V1_TERMINATOR = b"\xef\xbe\xad\xde"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_STRING = 1024
MAX_PAGES = 100_000
MAX_ENTRIES = 5_000_000
PAGE_CACHE = 3


@dataclass(frozen=True)
class Entry:
    name: str
    page: int
    x: int
    y: int
    w: int
    h: int
    ox: int
    oy: int
    fw: int
    fh: int


@dataclass(frozen=True)
class Page:
    name: str
    png_offset: int
    png_length: int
    width: int
    height: int


@dataclass
class Pack:
    """One indexed pack file."""

    path: Path
    layout: str
    pages: list[Page]
    entries: dict[str, Entry]
    rows: int
    duplicates: int
    shadowed: int = 0
    supplied: int = field(default=0)


class _Reader:
    def __init__(self, handle: BinaryIO, path: Path) -> None:
        self.handle = handle
        self.path = path

    def fail(self, message: str) -> BuildFailed:
        return BuildFailed(f"{self.path.name}: {message}", pack=self.path.name)

    def read(self, count: int) -> bytes:
        data = self.handle.read(count)
        if len(data) != count:
            raise self.fail(f"truncated at byte {self.handle.tell()} (wanted {count}, got {len(data)})")
        return data

    def int32(self) -> int:
        return int(struct.unpack("<i", self.read(4))[0])

    def string(self) -> str:
        length = self.int32()
        if not 0 <= length <= MAX_STRING:
            raise self.fail(f"string length {length} at byte {self.handle.tell() - 4} is out of range")
        return self.read(length).decode("latin-1")


def _check_name(reader: _Reader, name: str) -> None:
    if not name or name in {".", ".."} or any(char in name for char in "/\\\x00"):
        raise reader.fail(f"unsafe entry name {name!r}")


def _png_size(reader: _Reader, offset: int) -> tuple[int, int]:
    reader.handle.seek(offset)
    head = reader.read(24)
    if head[:8] != PNG_SIGNATURE or head[12:16] != b"IHDR":
        raise reader.fail(f"no PNG at byte {offset}")
    width, height = struct.unpack(">II", head[16:24])
    return int(width), int(height)


def _skip_v1_png(reader: _Reader, offset: int) -> int:
    """The PNG's length, found by walking its chunks to IEND and checking the 0xDEADBEEF that follows.

    The terminator is not searched for: its bytes can occur inside compressed image data.
    """
    reader.handle.seek(offset)
    if reader.read(8) != PNG_SIGNATURE:
        raise reader.fail(f"no PNG at byte {offset}")
    while True:
        length, kind = struct.unpack(">I4s", reader.read(8))
        reader.handle.seek(length + 4, io.SEEK_CUR)
        if kind == b"IEND":
            break
    end = reader.handle.tell()
    if reader.read(4) != V1_TERMINATOR:
        raise reader.fail(f"missing 0xDEADBEEF terminator after the PNG ending at byte {end}")
    return end - offset


def _page(reader: _Reader, layout: str, index: int, entries: dict[str, Entry]) -> tuple[Page, int, int]:
    """One page; returns it with the number of entry rows it held and how many repeated an earlier name."""
    name = reader.string()
    count = reader.int32()
    if not 0 <= count <= MAX_ENTRIES:
        raise reader.fail(f"page {name!r} declares {count} entries")
    reader.int32()  # mask flag: 1 when the page has an alpha mask, which this reader does not use
    pending: list[Entry] = []
    for _ in range(count):
        entry_name = reader.string()
        _check_name(reader, entry_name)
        x, y, w, h, ox, oy, fw, fh = struct.unpack("<8i", reader.read(32))
        pending.append(Entry(entry_name, index, x, y, w, h, ox, oy, fw, fh))
    if layout == "v2":
        png_length = reader.int32()
        png_offset = reader.handle.tell()
        if png_length < 0:
            raise reader.fail(f"page {name!r} declares a PNG of {png_length} bytes")
        width, height = _png_size(reader, png_offset)
        if png_offset + png_length > _file_size(reader):
            raise reader.fail(f"page {name!r} PNG runs past the end of the file")
        reader.handle.seek(png_offset + png_length)
    else:
        png_offset = reader.handle.tell()
        width, height = _png_size(reader, png_offset)
        png_length = _skip_v1_png(reader, png_offset)
    duplicates = 0
    for entry in pending:
        _check_entry(reader, name, entry, width, height)
        if entry.name in entries:
            duplicates += 1
        else:
            entries[entry.name] = entry
    return Page(name, png_offset, png_length, width, height), count, duplicates


def _file_size(reader: _Reader) -> int:
    here = reader.handle.tell()
    size = reader.handle.seek(0, io.SEEK_END)
    reader.handle.seek(here)
    return size


def _check_entry(reader: _Reader, page: str, entry: Entry, width: int, height: int) -> None:
    values = (entry.x, entry.y, entry.w, entry.h, entry.ox, entry.oy, entry.fw, entry.fh)
    problem = None
    if min(values) < 0:
        problem = "has a negative value"
    elif entry.x + entry.w > width or entry.y + entry.h > height:
        problem = f"rectangle {entry.x},{entry.y} {entry.w}x{entry.h} lies outside the {width}x{height} page"
    elif entry.ox + entry.w > entry.fw or entry.oy + entry.h > entry.fh:
        problem = f"offset {entry.ox},{entry.oy} with {entry.w}x{entry.h} does not fit its {entry.fw}x{entry.fh} frame"
    if problem:
        raise reader.fail(f"entry {entry.name!r} of page {page!r} {problem}")


def load_pack(path: Path) -> Pack:
    """Index one pack file; any malformed structure is a ``BuildFailed`` naming the file."""
    with path.open("rb") as handle:
        reader = _Reader(handle, path)
        if reader.read(4) == MAGIC:
            version = reader.int32()
            if version != V2_VERSION:
                raise reader.fail(f"PZPK pack version {version} is not supported (only {V2_VERSION})")
            layout = "v2"
        else:
            handle.seek(0)
            layout = "v1"
        page_count = reader.int32()
        if not 0 <= page_count <= MAX_PAGES:
            raise reader.fail(f"declares {page_count} pages")
        entries: dict[str, Entry] = {}
        pages: list[Page] = []
        rows = duplicates = 0
        for index in range(page_count):
            page, count, repeated = _page(reader, layout, index, entries)
            pages.append(page)
            rows += count
            duplicates += repeated
    return Pack(path=path, layout=layout, pages=pages, entries=entries, rows=rows, duplicates=duplicates)


class PixelSource:
    """Named textures out of several packs; the first pack listed that holds a name supplies it."""

    def __init__(self, packs: dict[str, Pack]) -> None:
        self.packs = packs
        self._pages: OrderedDict[tuple[str, int], Image.Image] = OrderedDict()
        seen: set[str] = set()
        for pack in packs.values():
            pack.shadowed = pack.duplicates + sum(1 for name in pack.entries if name in seen)
            seen.update(pack.entries)

    @classmethod
    def open(cls, root: Path, files: list[str]) -> PixelSource:
        return cls({relative: load_pack(root / relative) for relative in files})

    def lookup(self, name: str) -> tuple[str, Entry] | None:
        for relative, pack in self.packs.items():
            entry = pack.entries.get(name)
            if entry is not None:
                return relative, entry
        return None

    def write_png(self, pack_file: str, entry: Entry, dest: Path) -> Path:
        """The entry's full ``fw x fh`` frame as a PNG at ``dest``."""
        frame = Image.new("RGBA", (entry.fw, entry.fh))
        crop = self._page_image(pack_file, entry.page).crop((entry.x, entry.y, entry.x + entry.w, entry.y + entry.h))
        frame.paste(crop, (entry.ox, entry.oy))
        dest.parent.mkdir(parents=True, exist_ok=True)
        frame.save(dest, format="PNG", optimize=False)
        return dest

    def relative_path(self, pack_file: str, entry: Entry) -> str:
        """Where a texture sits under a work directory: ``textures/<pack file>/<entry>.png``."""
        pack_name = Path(pack_file).name
        try:
            return str(safe_relative(f"textures/{pack_name}/{entry.name}.png", field="texture path"))
        except UsageError as exc:
            raise BuildFailed(f"{pack_name}: entry name {entry.name!r} cannot be written to a file") from exc

    def _page_image(self, pack_file: str, index: int) -> Image.Image:
        key = (pack_file, index)
        cached = self._pages.get(key)
        if cached is not None:
            self._pages.move_to_end(key)
            return cached
        pack = self.packs[pack_file]
        page = pack.pages[index]
        with pack.path.open("rb") as handle:
            handle.seek(page.png_offset)
            data = handle.read(page.png_length)
        try:
            with Image.open(io.BytesIO(data), formats=["PNG"]) as source:
                source.load()
                image = source.convert("RGBA")
        except (UnidentifiedImageError, OSError, SyntaxError) as exc:
            raise BuildFailed(f"{pack.path.name}: page {page.name!r} is not a readable PNG ({exc})") from exc
        self._pages[key] = image
        if len(self._pages) > PAGE_CACHE:
            self._pages.popitem(last=False)
        return image
