"""The CLR decoder: the export requests the container understands for .NET assemblies."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import container

DECODER_NAME = "clr"
GAME_MOUNT = "/game"


@dataclass(frozen=True)
class ClrDecoder:
    image: container.DecoderImage
    cache: Path

    def request(self, exports: list[dict[str, Any]]) -> dict[str, Any]:
        return {"schemaVersion": 1, "decoder": DECODER_NAME, "settings": {}, "root": GAME_MOUNT, "exports": exports}

    def export(self, root: Path, exports: list[dict[str, Any]], *, inputs_sha256: str) -> container.Manifest:
        """Run ``exports`` against the assemblies under ``root`` (mounted read-only, no environment variables)."""
        return container.run_cached(
            self.image,
            self.request(exports),
            [container.Mount(root, GAME_MOUNT)],
            inputs_sha256=inputs_sha256,
            cache=self.cache,
        )
