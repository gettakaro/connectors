"""Cache entries: a directory that is complete only once its ``.takaro/complete.json`` marker is in place.

An entry is staged beside its final name and swapped in whole, so a reader sees either no entry or a finished one.
A marker that cannot be read counts as no marker, whatever the reason, and the entry is then built again.
"""

from __future__ import annotations

import json
import os
import shutil
import uuid
from collections.abc import Callable
from pathlib import Path
from typing import Any

from ..steam.install import COMPLETE_MARKER

MARKER = COMPLETE_MARKER


def write_new(path: Path, text: str) -> None:
    """Create ``path`` exclusively and without following a link planted at it."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
        handle.write(text)


def read_marker(directory: Path, key: str) -> dict[str, Any] | None:
    """The marker of ``directory``, or ``None`` when it is missing, unreadable, not an object or lacks ``key``."""
    try:
        document = json.loads((directory / MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return document if isinstance(document, dict) and key in document else None


def store(directory: Path, fill: Callable[[Path], dict[str, Any]]) -> dict[str, Any]:
    """Replace ``directory`` with the entry ``fill`` builds in a sibling staging directory; returns its marker."""
    if directory.exists():
        shutil.rmtree(directory)
    directory.parent.mkdir(parents=True, exist_ok=True)
    staging = directory.parent / f"tmp-{uuid.uuid4().hex}"
    try:
        marker = fill(staging)
        (staging / MARKER).parent.mkdir(parents=True, exist_ok=True)
        write_new(staging / MARKER, json.dumps(marker, indent=2) + "\n")
        os.replace(staging, directory)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return marker
