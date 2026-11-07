"""What the pipeline hands a mapper for Unreal games: decoded data tables, and decoded textures from the client.

``export_data`` runs the decoder over the filtered server-depot files. ``export_textures`` plans which byte
ranges of the client's containers hold the packages, mirrors only those (``client_ranges``) and decodes the
textures from the mirror, in one container run.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..exit_codes import BuildFailed
from . import client_ranges
from .decoders import container
from .decoders.unreal import UnrealDecoder

MAX_LISTED_FAILURES = 20


@dataclass(frozen=True)
class TextureRef:
    """A texture asset: its package path (``/Game/...``), object name and the path of its files below the mount."""

    package: str
    object: str
    stem: str


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


class UnrealSession:
    def __init__(
        self,
        decoder: UnrealDecoder,
        *,
        data_root: Path,
        data_tree_sha256: str,
        client: Path,
        utoc_files: list[str],
        mirror_base: Path,
    ) -> None:
        self.decoder = decoder
        self._data_root = data_root
        self._data_tree_sha256 = data_tree_sha256
        self._client = client
        self._utoc_files = utoc_files
        self._mirror_base = mirror_base
        self.mirror: client_ranges.Mirror | None = None
        self.tool: dict[str, Any] = {}

    def export_data(self, exports: list[dict[str, Any]]) -> container.Manifest:
        manifest = self.decoder.export(self._data_root, exports, inputs_sha256=self._data_tree_sha256)
        self.tool = manifest.tool
        return manifest

    def export_textures(self, textures: list[TextureRef]) -> dict[str, Outcome]:
        """Every texture's outcome keyed by package path; ``Missing`` for a package no container holds."""
        mirror = client_ranges.mirror(
            self._client,
            self._utoc_files,
            [texture.stem for texture in textures],
            mirror_base=self._mirror_base,
        )
        self.mirror = mirror
        outcomes: dict[str, Outcome] = {}
        absent = set(mirror.missing)
        requested = []
        for texture in sorted(textures, key=lambda ref: ref.package):
            if texture.stem in absent:
                outcomes[texture.package] = Missing(texture.package)
            else:
                requested.append(texture)
        if not requested:
            return outcomes
        ids = {texture.package: f"tx{index:05d}" for index, texture in enumerate(requested)}
        manifest = self.decoder.export(
            mirror.root,
            [
                {"id": ids[texture.package], "kind": "texture", "package": texture.package, "object": texture.object}
                for texture in requested
            ],
            inputs_sha256=mirror.inputs_sha256,
        )
        self.tool = manifest.tool
        failed = []
        for texture in requested:
            result = manifest.results[ids[texture.package]]
            if result["ok"]:
                meta = result["meta"]
                outcomes[texture.package] = Texture(
                    manifest.file(ids[texture.package]), int(meta["width"]), int(meta["height"]), str(meta["format"])
                )
            elif result["errorKind"] == "unsupported-format":
                outcomes[texture.package] = Unsupported(str(result["error"]))
            elif result["errorKind"] == "not-found":
                outcomes[texture.package] = Missing(texture.package)
            else:
                failed.append(f"{ids[texture.package]} {texture.package}: {result['error']}")
        if failed:
            raise BuildFailed(
                f"the decoder failed on {len(failed)} textures",
                failures=failed[:MAX_LISTED_FAILURES],
            )
        return outcomes
