"""7 Days to Die item icons: codes and icon names come from the server depot's ``Data/Config`` XML.

Both the data and the pixels come from the anonymous dedicated-server depot: ``items.xml``,
``item_modifiers.xml`` and ``blocks.xml`` say which entries exist and which sprite each uses, ``Localization.csv``
decides which entries the connector reports, and ``Data/ItemIcons/*.png`` are the sprites.

The rules are the ones the game applies (read from ``Assembly-CSharp.dll``, build 26310339; see
``maintenance/docs/icons.md``): an entry's icon is its own ``CustomIcon``, else the one it inherits through
``Extends`` (unless that hop's ``param1`` lists ``CustomIcon``), else its own name; ``CustomIconTint`` is
inherited the same way and multiplies the sprite. No icon is guessed from how a name looks.
"""

from __future__ import annotations

import csv
import io
import re
import shutil
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ...exit_codes import BuildFailed, UpstreamUnavailable
from ...icons.mappers import MapContext, MapResult
from ...paths import safe_relative

MAPPER_VERSION = 1
CODES_DESCRIPTION = "item, item-modifier and block names from Data/Config items.xml, item_modifiers.xml and blocks.xml"
EXCLUSIONS = "entries without an English localization (missing, empty, or equal to the key)"

CONFIG = "Data/Config"
ICONS = "Data/ItemIcons"
LOCALIZATION = f"{CONFIG}/Localization.csv"
PROP_ICON = "CustomIcon"
PROP_TINT = "CustomIconTint"
DETAIL_LIMIT = 256

_HEX_TINT = re.compile(r"#?([0-9A-Fa-f]{6})(?:[0-9A-Fa-f]{2})?")
_CHANNEL = re.compile(r"[0-9]{1,3}")
_DTD = re.compile(rb"<!\s*(?:DOCTYPE|ENTITY)", re.IGNORECASE)


@dataclass(frozen=True)
class Source:
    """One XML file: its own namespace of entries, which the ``Extends`` of its entries resolve in."""

    key: str
    path: str
    element: str
    kind: str


SOURCES = [
    Source("items", f"{CONFIG}/items.xml", "item", "item"),
    Source("itemModifiers", f"{CONFIG}/item_modifiers.xml", "item_modifier", "item"),
    Source("blocks", f"{CONFIG}/blocks.xml", "block", "block"),
]


@dataclass(frozen=True)
class Entry:
    name: str
    properties: dict[str, str]
    extends: str | None
    excluded: frozenset[str]


@dataclass
class Resolver:
    """Resolves one inherited property across the entries of one file, memoized; counts what it could not follow."""

    entries: dict[str, Entry]
    unresolved_extends: set[str] = field(default_factory=set)
    cycles: set[str] = field(default_factory=set)
    _memo: dict[tuple[str, str], tuple[str | None, bool]] = field(default_factory=dict)
    _active: set[tuple[str, str]] = field(default_factory=set)

    def resolve(self, name: str, prop: str) -> tuple[str | None, bool]:
        """``(value, inherited)``: the entry's own value, else its parent's unless the hop excludes ``prop``."""
        key = (name, prop)
        if key in self._memo:
            return self._memo[key]
        if key in self._active:
            self.cycles.add(name)
            return None, False
        entry = self.entries[name]
        if prop in entry.properties:
            result: tuple[str | None, bool] = (entry.properties[prop], False)
        elif entry.extends is None or prop in entry.excluded:
            result = (None, False)
        elif entry.extends not in self.entries:
            self.unresolved_extends.add(name)
            result = (None, False)
        else:
            self._active.add(key)
            value, _ = self.resolve(entry.extends, prop)
            self._active.discard(key)
            result = (value, value is not None)
        self._memo[key] = result
        return result


def _read_required(data_root: Path, relative: str) -> bytes:
    path = data_root / relative
    if not path.is_file():
        raise UpstreamUnavailable(
            f"the server depot did not serve {relative}, which the 7d2d mapper needs", missing=relative
        )
    return path.read_bytes()


def parse_entries(raw: bytes, source: Source) -> list[Entry]:
    """The entries of one config file in file order; only an entry's direct, class-less properties count."""
    if _DTD.search(raw):
        raise BuildFailed(f"{source.path} declares a DOCTYPE or ENTITY, which game config never does", file=source.path)
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as exc:
        raise BuildFailed(f"{source.path} is not well-formed XML: {exc}", file=source.path) from exc
    entries = []
    for element in root.findall(source.element):
        name = element.get("name")
        if not name:
            raise BuildFailed(f"{source.path} has a <{source.element}> without a name", file=source.path)
        properties: dict[str, str] = {}
        extends: str | None = None
        excluded: frozenset[str] = frozenset()
        for prop in element.findall("property"):
            if prop.get("class") is not None or prop.get("name") is None:
                continue
            properties[prop.get("name", "")] = prop.get("value", "")
            if prop.get("name") == "Extends":
                extends = prop.get("value", "")
                excluded = frozenset(part.strip() for part in (prop.get("param1") or "").split(","))
        entries.append(Entry(name, properties, extends, excluded))
    if not entries:
        raise BuildFailed(f"{source.path} has no <{source.element}> entries", file=source.path)
    return entries


def read_english(raw: bytes) -> dict[str, str]:
    """Key to English text from ``Localization.csv``, found by header name; the first row of a key wins."""
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise BuildFailed(f"{LOCALIZATION} is not UTF-8: {exc}", file=LOCALIZATION) from exc
    rows = csv.reader(io.StringIO(text, newline=""))
    header = next(rows, [])
    columns = {cell.strip().casefold(): index for index, cell in enumerate(header)}
    missing = [column for column in ("key", "english") if column not in columns]
    if missing:
        raise BuildFailed(f"{LOCALIZATION} has no {', '.join(missing)} column in its header", file=LOCALIZATION)
    key_at, english_at = columns["key"], columns["english"]
    english: dict[str, str] = {}
    for row in rows:
        if len(row) > max(key_at, english_at):
            english.setdefault(row[key_at], row[english_at])
    return english


def is_localized(name: str, english: dict[str, str]) -> bool:
    """``Localization.Get`` returns the key when there is no entry; that and blank text are not names."""
    value = english.get(name)
    return value is not None and value.strip() != "" and value != name


def parse_tint(value: str) -> str | None:
    """``#rrggbb`` for a game tint, or ``None`` when the game's formats do not cover it.

    ``StringParsers.ParseHexColor`` takes hex digits with an optional ``#`` or comma-separated 0 to 255
    integers; a fourth component or two more hex digits are alpha, which the sprite colour ignores.
    """
    text = value.strip()
    if "," in text:
        parts = [part.strip() for part in text.split(",")]
        if len(parts) not in (3, 4) or not all(_CHANNEL.fullmatch(part) for part in parts):
            return None
        channels = [int(part) for part in parts]
        if any(channel > 255 for channel in channels):
            return None
        return "#{:02x}{:02x}{:02x}".format(*channels[:3])
    hexed = _HEX_TINT.fullmatch(text)
    return f"#{hexed.group(1).lower()}" if hexed else None


@dataclass(frozen=True)
class IconFiles:
    exact: dict[str, Path]
    folded: dict[str, Path]

    @staticmethod
    def scan(data_root: Path) -> IconFiles:
        root = data_root / ICONS
        files = (
            sorted(
                (path for path in root.iterdir() if path.is_file() and path.suffix.lower() == ".png"),
                key=lambda path: path.name,
            )
            if root.is_dir()
            else []
        )
        exact = {path.stem: path for path in files}
        folded: dict[str, Path] = {}
        for path in files:
            folded.setdefault(path.stem.casefold(), path)
        return IconFiles(exact, folded)

    def lookup(self, icon: str) -> tuple[Path, bool] | None:
        """``(file, case_mismatch)``; the exact stem first, then a casefold-only hit."""
        if icon in self.exact:
            return self.exact[icon], False
        found = self.folded.get(icon.casefold())
        return (found, True) if found else None


def _decide_icon(resolver: Resolver, name: str) -> tuple[str, str]:
    """``(icon name, rule)``; the icon falls back to the entry's own name, never the parent's."""
    icon, inherited = resolver.resolve(name, PROP_ICON)
    if icon:
        return icon, "inherited-icon" if inherited else "own-icon"
    return name, "name"


def map_items(ctx: MapContext) -> MapResult:
    english = read_english(_read_required(ctx.data_root, LOCALIZATION))
    icon_files = IconFiles.scan(ctx.data_root)
    if not icon_files.exact:
        raise UpstreamUnavailable(
            f"the server depot served no icons under {ICONS}, which the 7d2d mapper needs", missing=ICONS
        )
    notes: list[str] = []
    counts: dict[str, int] = {}
    excluded: dict[str, int] = {}
    seen: set[str] = set()
    recipes: list[dict[str, Any]] = []
    unmatched: list[dict[str, Any]] = []
    copied: dict[str, Path] = {}
    repeats = 0
    unresolved = 0
    cycles = 0
    case_mismatches = 0

    for source in SOURCES:
        entries = parse_entries(_read_required(ctx.data_root, source.path), source)
        counts[source.key] = len(entries)
        excluded[source.key] = 0
        first_definitions: dict[str, Entry] = {}
        for entry in entries:
            first_definitions.setdefault(entry.name, entry)
        resolver = Resolver(first_definitions)
        for entry in entries:
            if entry.name in seen:
                repeats += 1
                continue
            seen.add(entry.name)
            if not is_localized(entry.name, english):
                excluded[source.key] += 1
                continue
            icon, rule = _decide_icon(resolver, entry.name)
            tint_value, _ = resolver.resolve(entry.name, PROP_TINT)
            tint = parse_tint(tint_value) if tint_value is not None else None
            tinted = tint is not None and tint != "#ffffff"
            full_rule = f"{rule}-tinted" if tinted else rule
            failure: dict[str, Any] | None = None
            found = None
            if tint_value is not None and tint is None:
                failure = {"reason": "invalid-tint", "rule": rule, "detail": tint_value}
            else:
                found = icon_files.lookup(icon)
                if found is None:
                    failure = {"reason": "missing-texture", "rule": full_rule, "detail": f"{icon}.png"}
            if failure is not None:
                unmatched.append({"code": entry.name, "kind": source.kind, **failure})
                continue
            assert found is not None
            file, mismatch = found
            relative = f"ItemIcons/{file.name}"
            copied[relative] = file
            recipe: dict[str, Any] = {
                "code": entry.name,
                "kind": source.kind,
                "rule": full_rule,
                "layers": [{"image": relative, "ops": [{"op": "tint-multiply", "color": tint}] if tinted else []}],
                "composite": "over",
            }
            if mismatch:
                case_mismatches += 1
                recipe["flags"] = [{"flag": "case-mismatch", "detail": file.name}]
            recipes.append(recipe)
        unresolved += len(resolver.unresolved_extends)
        cycles += len(resolver.cycles)

    for unmatched_entry in unmatched:
        unmatched_entry["detail"] = unmatched_entry["detail"][:DETAIL_LIMIT]

    for relative, file in sorted(copied.items()):
        destination = ctx.work / "textures-root" / safe_relative(relative, field="icon path")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(file, destination)

    notes.append(
        f"{counts['items']} items, {counts['itemModifiers']} item modifiers, {counts['blocks']} blocks in the config"
    )
    for source in SOURCES:
        if excluded[source.key]:
            notes.append(f"{excluded[source.key]} {source.key} entries left out of the code list (no English name)")
    if repeats:
        notes.append(f"{repeats} repeated names ignored (the first definition is kept)")
    if unresolved:
        notes.append(f"{unresolved} entries extend an entry that is not in their file (unresolved-extends)")
    if cycles:
        notes.append(f"{cycles} Extends cycles met (inheritance stopped where each closed)")
    if case_mismatches:
        notes.append(f"{case_mismatches} icons found only by a case-insensitive file name match")

    document: dict[str, Any] = {"schemaVersion": 1, "game": "7d2d", "codesFrom": ctx.codes_from, "recipes": recipes}
    if unmatched:
        document["unmatched"] = unmatched
    return MapResult(
        document=document,
        pack_usage={},
        notes=notes,
        details={
            "icons": {"files": len(icon_files.exact), "used": len(copied)},
            "excluded": excluded,
            "sources": counts,
        },
    )
