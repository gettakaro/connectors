"""The registry of mapping modules: each turns a game's data files into a recipe document."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .decoders.pz_texturepack import PixelSource

if TYPE_CHECKING:
    from .unreal_session import UnrealSession


@dataclass(frozen=True)
class MapResult:
    """What a mapper hands the builder: the recipe document, per-pack usage, notes and extra build-record fields."""

    document: dict[str, Any]
    pack_usage: dict[str, dict[str, Any]]
    notes: list[str]
    details: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class MapContext:
    """Everything a mapper may use: the server data, the pixel source of its decoder family and a scratch directory.

    ``codes_from`` is the provenance sentence the pipeline composes for mappers that take it as is;
    ``codes_from_inputs`` carries the facts a mapper needs to word its own.
    """

    data_root: Path
    pixels: PixelSource | None
    work: Path
    codes_from: str
    codes_from_inputs: dict[str, Any]
    unreal: UnrealSession | None
    warn: Callable[[str], None]


@dataclass(frozen=True)
class Mapper:
    name: str
    version: int
    run: Callable[[MapContext], MapResult]
    codes_description: str = ""
    exclusions: str = ""


def _zomboid_run(ctx: MapContext) -> MapResult:
    from ..games.zomboid import icons as zomboid

    assert ctx.pixels is not None
    return zomboid.map_items(ctx.data_root, ctx.pixels, ctx.work / "textures-root", codes_from=ctx.codes_from)


def registry() -> dict[str, Mapper]:
    from ..games.conan_exiles import icons as conan
    from ..games.zomboid import icons as zomboid

    return {
        "conan-exiles": Mapper("conan-exiles", conan.MAPPER_VERSION, conan.map_items),
        "zomboid": Mapper(
            "zomboid", zomboid.MAPPER_VERSION, _zomboid_run, zomboid.CODES_DESCRIPTION, zomboid.EXCLUSIONS
        ),
    }


def names() -> list[str]:
    return sorted(registry())


def open_pixels(root: Path, files: list[str]) -> PixelSource:
    return PixelSource.open(root, files)
