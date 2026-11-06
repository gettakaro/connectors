"""Project Zomboid item icons: the item codes and their texture names come from ``media/scripts``.

``parse_items`` is the ScriptManager equivalent: it finds every ``item`` block of every script file and
reads its properties. ``map_items`` then turns each item into a recipe (or an unmatched entry that says
why) from the item's own ``Icon``, ``IconsForTexture`` and ``WorldObjectSprite`` properties, never from
how a name looks.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...icons.decoders.pz_texturepack import PixelSource
from ...icons.mappers import MapResult
from ...paths import safe_relative

SCRIPTS = "media/scripts"
MAPPER_VERSION = 1
CODES_DESCRIPTION = "a ScriptManager-equivalent parse of media/scripts item blocks"
EXCLUSIONS = (
    "obsolete items (OBSOLETE = true), disabled items (enabled = anything but true) "
    "and debug-only items (debugOnly = true)"
)

_ITEM_HEADER = re.compile(r"item\s+([^\s{}]+)")
_MODULE_HEADER = re.compile(r"module\s+([^\s{}]+)")
_UNMATCHED_DETAIL_LIMIT = 256
_COMMENT_MARK = re.compile(r"/\*|\*/")
_BRACE = re.compile(r"[{}]")


@dataclass
class _Block:
    header: str
    text: list[str]
    children: list[_Block]


@dataclass(frozen=True)
class ScriptItem:
    code: str
    properties: dict[str, str]


@dataclass(frozen=True)
class ParsedItems:
    items: list[ScriptItem]
    files: int
    duplicates: int
    excluded: dict[str, int]


def strip_comments(text: str) -> str:
    """Remove ``/* ... */`` comments, which nest."""
    out: list[str] = []
    depth = 0
    position = 0
    for mark in _COMMENT_MARK.finditer(text):
        if mark.group() == "/*":
            if depth == 0:
                out.append(text[position : mark.start()])
            depth += 1
        elif depth:
            depth -= 1
            if depth == 0:
                position = mark.end()
    if depth == 0:
        out.append(text[position:])
    return "".join(out)


def _split_pending(pending: str) -> tuple[str, str]:
    """The text since the last brace, split into what belongs to the enclosing block and the header line."""
    lines = pending.split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    header = lines.pop().strip() if lines else ""
    return "\n".join(lines), header


def _parse_blocks(text: str) -> _Block:
    root = _Block("", [], [])
    stack = [root]
    position = 0
    for brace in _BRACE.finditer(text):
        pending = text[position : brace.start()]
        position = brace.end()
        if brace.group() == "{":
            before, header = _split_pending(pending)
            stack[-1].text.append(before)
            block = _Block(header, [], [])
            stack[-1].children.append(block)
            stack.append(block)
        else:
            stack[-1].text.append(pending)
            if len(stack) > 1:
                stack.pop()
    stack[-1].text.append(text[position:])
    return root


def _properties(block: _Block) -> dict[str, str]:
    """``key = value`` pairs at the block's own depth; keys are case-insensitive and the last one wins."""
    found: dict[str, str] = {}
    for piece in "\n".join(block.text).split(","):
        for line in piece.splitlines():
            key, separator, value = line.partition("=")
            if separator and key.strip():
                found[key.strip().casefold()] = value.strip()
    return found


def script_files(data_root: Path) -> list[Path]:
    root = data_root / SCRIPTS
    return sorted(
        (path for path in root.rglob("*") if path.is_file() and path.name.lower().endswith(".txt")),
        key=lambda path: path.relative_to(root).as_posix(),
    )


def _read(path: Path) -> str:
    raw = path.read_bytes()
    try:
        return raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return raw.decode("cp1252", errors="replace")


def _exclusion(properties: dict[str, str]) -> str | None:
    """Why ScriptManager would not register the item, from the checks ``ScriptBucket`` makes after loading it.

    Read from the game jar (build 25485538): an item is dropped when ``OBSOLETE`` is true, when ``enabled`` is
    given and is not ``true``, and when ``debugOnly`` is true while the game is not in debug mode, which a
    dedicated server is not.
    """
    if properties.get("obsolete", "").casefold() == "true":
        return "obsolete"
    if properties.get("enabled") and properties["enabled"].casefold() != "true":
        return "disabled"
    if properties.get("debugonly", "").casefold() == "true":
        return "debug-only"
    return None


def parse_items(data_root: Path) -> ParsedItems:
    """Every item the game would register, in file order; the first of a repeated full name wins."""
    files = script_files(data_root)
    items: list[ScriptItem] = []
    seen: set[str] = set()
    duplicates = 0
    excluded: dict[str, int] = {}
    for path in files:
        root = _parse_blocks(strip_comments(_read(path)))
        for module in root.children:
            module_header = _MODULE_HEADER.fullmatch(module.header)
            if module_header is None:
                continue
            for block in module.children:
                header = _ITEM_HEADER.fullmatch(block.header)
                if header is None:
                    continue
                code = f"{module_header.group(1)}.{header.group(1)}"
                properties = _properties(block)
                reason = _exclusion(properties)
                if reason:
                    excluded[reason] = excluded.get(reason, 0) + 1
                    continue
                if code in seen:
                    duplicates += 1
                    continue
                seen.add(code)
                items.append(ScriptItem(code, properties))
    return ParsedItems(items, len(files), duplicates, excluded)


def _decide(item: ScriptItem, pixels: PixelSource) -> tuple[str, str, tuple[str, Any] | None, str | None]:
    """``(rule, reason or "", found texture, detail)``; a reason means the item is unmatched."""
    icon = item.properties.get("icon", "")
    if icon.casefold() == "default":
        sprite = item.properties.get("worldobjectsprite", "")
        if not sprite:
            return "moveable", "no-world-sprite", None, None
        found = pixels.lookup(sprite)
        if found is None:
            return "moveable", "missing-sprite", None, sprite
        return "moveable", "", found, None
    if icon:
        name = f"Item_{icon}"
        found = pixels.lookup(name)
        return "icon", ("" if found else "missing-texture"), found, (None if found else name)
    first = next((part.strip() for part in item.properties.get("iconsfortexture", "").split(";") if part.strip()), "")
    if first:
        name = f"Item_{first}"
        found = pixels.lookup(name)
        return "icons-for-texture", ("" if found else "missing-texture"), found, (None if found else name)
    return "none", "no-icon-in-game", None, None


def map_items(data_root: Path, pixels: PixelSource, work: Path, *, codes_from: str) -> MapResult:
    parsed = parse_items(data_root)
    notes = [f"{parsed.files} script files, {len(parsed.items)} items"]
    if parsed.duplicates:
        notes.append(f"{parsed.duplicates} repeated full names ignored (the first definition is kept)")
    for reason, count in sorted(parsed.excluded.items()):
        notes.append(f"{count} {reason} items left out of the code list")

    recipes: list[dict[str, Any]] = []
    unmatched: list[dict[str, Any]] = []
    needed: dict[str, tuple[str, Any]] = {}
    for item in parsed.items:
        rule, reason, found, detail = _decide(item, pixels)
        if found is None:
            entry: dict[str, Any] = {"code": item.code, "kind": "item", "reason": reason, "rule": rule}
            if detail:
                entry["detail"] = detail[:_UNMATCHED_DETAIL_LIMIT]
            unmatched.append(entry)
            continue
        pack_file, texture = found
        path = pixels.relative_path(pack_file, texture)
        needed[path] = (pack_file, texture)
        pixels.packs[pack_file].supplied += 1
        ops = [{"op": "trim"}] if rule == "moveable" else []
        recipes.append(
            {
                "code": item.code,
                "kind": "item",
                "rule": rule,
                "layers": [{"image": path, "ops": ops}],
                "composite": "over",
            }
        )

    for path, (pack_file, texture) in sorted(needed.items(), key=lambda row: (row[1][0], row[1][1].page, row[0])):
        pixels.write_png(pack_file, texture, work / safe_relative(path, field="texture path"))

    document = {"schemaVersion": 1, "game": "zomboid", "codesFrom": codes_from, "recipes": recipes}
    if unmatched:
        document["unmatched"] = unmatched
    usage = {
        relative: {
            "layout": pack.layout,
            "pages": len(pack.pages),
            "entries": pack.rows,
            "codesSupplied": pack.supplied,
            "shadowedNames": pack.shadowed,
        }
        for relative, pack in pixels.packs.items()
    }
    return MapResult(document=document, pack_usage=usage, notes=notes)
