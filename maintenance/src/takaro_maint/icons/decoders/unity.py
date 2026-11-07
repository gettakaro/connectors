"""The Unity decoder: the export requests the container understands for asset bundles."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import container

DECODER_NAME = "unity"
GAME_MOUNT = "/game"


@dataclass(frozen=True)
class UnityDecoder:
    image: container.DecoderImage
    cache: Path

    def request(self, exports: list[dict[str, Any]], cab_index: dict[str, str] | None = None) -> dict[str, Any]:
        """``cab_index`` maps a serialized-file name to the bundle (relative to the mount) that holds it."""
        return {
            "schemaVersion": 1,
            "decoder": DECODER_NAME,
            "settings": {"cabIndex": cab_index} if cab_index is not None else {},
            "root": GAME_MOUNT,
            "exports": exports,
        }

    def export(
        self,
        root: Path,
        exports: list[dict[str, Any]],
        *,
        inputs_sha256: str,
        cab_index: dict[str, str] | None = None,
    ) -> container.Manifest:
        """Run ``exports`` against the game files under ``root`` (mounted read-only, no environment variables)."""
        return container.run_cached(
            self.image,
            self.request(exports, cab_index),
            [container.Mount(root, GAME_MOUNT)],
            inputs_sha256=inputs_sha256,
            cache=self.cache,
        )
