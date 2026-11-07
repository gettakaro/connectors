"""What a decoder answered for one export, shared by the Unreal and Unity sessions."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Texture:
    path: Path
    width: int
    height: int
    format: str


@dataclass(frozen=True)
class Missing:
    detail: str


@dataclass(frozen=True)
class Unsupported:
    detail: str


Outcome = Texture | Missing | Unsupported
