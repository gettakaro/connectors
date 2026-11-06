"""Reading, validating and writing a game's ``index.json``, shared by every command that touches it."""

from __future__ import annotations

import hashlib

from ..exit_codes import ConflictError, IntegrityError
from . import contract
from .bucket import Bucket


def index_key(game: str) -> str:
    return f"{game}/{contract.INDEX_NAME}"


def parse_raw(raw: bytes, game: str, what: str) -> contract.Index:
    parsed = contract.parse_index(raw, game)
    if isinstance(parsed, list):
        raise ConflictError(f"{what} violates the pack contract", problems=parsed)
    return parsed


def read_live(bucket: Bucket, game: str, what: str) -> tuple[bytes, contract.Index] | None:
    raw = bucket.get(index_key(game))
    return None if raw is None else (raw, parse_raw(raw, game, what))


def write_if_changed(bucket: Bucket, game: str, raw: bytes, live_raw: bytes | None) -> bool:
    """Upload the index unless its bytes already are the live bytes, then read it back. True when written."""
    if raw == live_raw:
        return False
    key = index_key(game)
    bucket.put(key, raw, contract.CONTENT_TYPES["index"], contract.CACHE_CONTROL["index"])
    read_back = bucket.get(key)
    if read_back is None or hashlib.sha256(read_back).digest() != hashlib.sha256(raw).digest():
        raise IntegrityError(f"{key} read back after upload differs from what was sent")
    return True
