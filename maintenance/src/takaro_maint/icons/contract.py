"""The Icon Pack contract shared with Takaro. Pure: no network, no environment.

Takaro's reader (``iconPackIndex.ts``) rejects a whole index when it is not an object, when its
``game`` differs from the game in the URL, when ``enabled`` is not a boolean, when ``icons`` is not
an object or when a value is not ``<64 hex>.webp``. This module applies those rules and a few
stricter ones, at build time and again at publish time.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, UnidentifiedImageError

from ..exit_codes import UsageError

GAME_PATTERN = re.compile(r"[a-z0-9][a-z0-9-]*")
GAME_MAX_LENGTH = 64
OBJECT_PATTERN = re.compile(r"[0-9a-f]{64}\.webp")
INDEX_NAME = "index.json"
MAX_EDGE = 128
MAX_INDEX_BYTES = 8 * 1024 * 1024
STORE_PREFIX = "_store/"
INDEX_KEYS = ("game", "enabled", "icons")

CONTENT_TYPES = {"webp": "image/webp", "index": "application/json"}
CACHE_CONTROL = {
    "webp": "public, max-age=31536000, immutable",
    "index": "public, max-age=300",
}

_KEY_PATTERN = re.compile(r"[a-z0-9][a-z0-9-]{0,63}/(index\.json|[0-9a-f]{64}\.webp)")


@dataclass(frozen=True)
class Index:
    game: str
    enabled: bool
    icons: dict[str, str]


def is_game_identifier(value: object) -> bool:
    return isinstance(value, str) and len(value) <= GAME_MAX_LENGTH and GAME_PATTERN.fullmatch(value) is not None


def object_name(webp: bytes) -> str:
    return hashlib.sha256(webp).hexdigest() + ".webp"


def serialize_index(game: str, enabled: bool, icons: dict[str, str]) -> bytes:
    """The one place index bytes are made, so build and publish agree for an identical mapping."""
    document = {"game": game, "enabled": enabled, "icons": dict(sorted(icons.items()))}
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def parse_index(raw: bytes, game: str) -> Index | list[str]:
    """The index, or every reason Takaro (or this tool's stricter rules) would reject it."""
    if len(raw) > MAX_INDEX_BYTES:
        return [f"index is {len(raw)} bytes, over the {MAX_INDEX_BYTES} byte cap"]
    try:
        document = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        return [f"index is not valid JSON: {exc}"]
    if not isinstance(document, dict):
        return ["index is not a JSON object"]
    problems: list[str] = []
    if set(document) != set(INDEX_KEYS):
        problems.append(f"index keys must be exactly {list(INDEX_KEYS)}, got {list(document)}")
    if document.get("game") != game:
        problems.append(f"index game {document.get('game')!r} != {game!r}")
    enabled = document.get("enabled")
    if not isinstance(enabled, bool):
        problems.append("index enabled is not a boolean")
    icons = document.get("icons")
    if not isinstance(icons, dict):
        problems.append("index icons is not an object")
    else:
        for code, name in icons.items():
            if not code:
                problems.append("index has an empty code")
            if not isinstance(name, str) or not OBJECT_PATTERN.fullmatch(name):
                problems.append(f"index value for {code!r} is not <sha256>.webp")
    if problems:
        return problems
    return Index(game=game, enabled=bool(enabled), icons=dict(icons or {}))


def check_pack_dir(path: Path) -> list[str]:
    """Every contract problem in a pack directory (named for its game); empty when it is valid."""
    game = path.name
    if not is_game_identifier(game):
        return [f"directory name {game!r} is not a valid game identifier"]
    if not path.is_dir():
        return [f"{path} is not a directory"]
    problems: list[str] = []
    entries = sorted(path.iterdir())
    for entry in entries:
        if entry.is_symlink() or not entry.is_file():
            problems.append(f"{entry.name}: not a regular file")
        elif entry.name != INDEX_NAME and not OBJECT_PATTERN.fullmatch(entry.name):
            problems.append(f"{entry.name}: not index.json or <sha256>.webp")
    index_path = path / INDEX_NAME
    if not index_path.is_file() or index_path.is_symlink():
        return [*problems, f"{INDEX_NAME} is missing"]
    parsed = parse_index(index_path.read_bytes(), game)
    if isinstance(parsed, list):
        return [*problems, *parsed]

    objects = {
        entry.name
        for entry in entries
        if entry.is_file() and not entry.is_symlink() and OBJECT_PATTERN.fullmatch(entry.name)
    }
    referenced = set(parsed.icons.values())
    for name in sorted(referenced - objects):
        problems.append(f"{name}: referenced by the index but missing")
    for name in sorted(objects - referenced):
        problems.append(f"{name}: not referenced by the index")
    for name in sorted(objects):
        problems.extend(check_object(name, (path / name).read_bytes()))
    return problems


def validate_game(game: str) -> None:
    """Refuse a game that is not a plain identifier; ``_store`` is named explicitly because it is Takaro's."""
    if game.startswith("_"):
        raise UsageError(f"refusing game {game!r}: {STORE_PREFIX} belongs to Takaro's Icon Store and is never written")
    if not is_game_identifier(game):
        raise UsageError(f"{game!r} is not a valid game identifier (lowercase letters, digits and '-', at most 64)")


def check_object(name: str, data: bytes) -> list[str]:
    """Contract problems of one object's bytes: its name is its sha256 and it is a WebP of at most 128px."""
    if object_name(data) != name:
        return [f"{name}: sha256 of the bytes does not match the name"]
    try:
        with Image.open(io.BytesIO(data), formats=["WEBP"]) as image:
            width, height = image.size
    except (UnidentifiedImageError, OSError) as exc:
        return [f"{name}: not a readable WebP ({exc})"]
    if width < 1 or height < 1 or max(width, height) > MAX_EDGE:
        return [f"{name}: {width}x{height} exceeds {MAX_EDGE}px"]
    return []


def guard_keys(keys: Iterable[str]) -> None:
    """Refuse any key that is not ``<game>/index.json`` or ``<game>/<sha256>.webp``; `_store/` is Takaro's."""
    for key in keys:
        if key.startswith(STORE_PREFIX):
            raise UsageError(f"refusing {key!r}: {STORE_PREFIX} belongs to Takaro's Icon Store and is never written")
        if not _KEY_PATTERN.fullmatch(key):
            raise UsageError(f"refusing {key!r}: not <game>/index.json or <game>/<sha256>.webp")
