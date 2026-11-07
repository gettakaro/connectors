"""The registry of mapping modules: each turns a game's data files into a recipe document."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .decoders.pz_texturepack import PixelSource


@dataclass(frozen=True)
class MapResult:
    """What a mapper hands the builder: the recipe document, per-pack usage and notes for the build record."""

    document: dict[str, Any]
    pack_usage: dict[str, dict[str, Any]]
    notes: list[str]


@dataclass(frozen=True)
class Mapper:
    name: str
    version: int
    map_items: Callable[..., MapResult]
    codes_description: str
    exclusions: str


def registry() -> dict[str, Mapper]:
    from ..games.zomboid import icons as zomboid

    return {
        "zomboid": Mapper(
            "zomboid", zomboid.MAPPER_VERSION, zomboid.map_items, zomboid.CODES_DESCRIPTION, zomboid.EXCLUSIONS
        )
    }


def names() -> list[str]:
    return sorted(registry())


def open_pixels(root: Path, files: list[str]) -> PixelSource:
    return PixelSource.open(root, files)
