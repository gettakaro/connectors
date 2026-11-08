"""What the pipeline hands a mapper for .NET games: assembly facts, call sites and decompiled C# of selected types.

The assembly is game data and is only read by the decoder container. Decompiled text is returned to the mapper
in memory; it stays in the decode cache and never reaches the repository.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ..exit_codes import BuildFailed
from .decoders import container
from .decoders.clr import ClrDecoder

MAX_LISTED_FAILURES = 20


class ClrSession:
    def __init__(self, decoder: ClrDecoder, *, mirror_root: Path, mirror_tree_sha256: str) -> None:
        self.decoder = decoder
        self._root = mirror_root
        self._tree_sha256 = mirror_tree_sha256
        self.tool: dict[str, Any] = {}

    def _export(self, exports: list[dict[str, Any]]) -> container.Manifest:
        manifest = self.decoder.export(self._root, exports, inputs_sha256=self._tree_sha256)
        self.tool = manifest.tool
        problems = [f"{result['id']}: {result['error']}" for result in manifest.failures()]
        if problems:
            raise BuildFailed(
                f"the decoder could not answer {len(problems)} of {len(exports)} exports",
                failures=problems[:MAX_LISTED_FAILURES],
            )
        return manifest

    def facts(self, file: str, callers_of: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        """``(assembly-info, callers of callers_of)`` of one assembly, from a single decoder run."""
        manifest = self._export(
            [
                {"id": "asm", "kind": "assembly-info", "file": file},
                {"id": "cl00001", "kind": "callers", "file": file, "member": callers_of},
            ]
        )
        info = json.loads(manifest.file("asm").read_text(encoding="utf-8"))
        callers = json.loads(manifest.file("cl00001").read_text(encoding="utf-8"))
        return info, callers

    def decompile(self, file: str, types: list[str]) -> dict[str, str]:
        """The C# source of each type (``Ns.Type`` or ``Ns.Outer+Inner``), from a single decoder run."""
        ordered = sorted(set(types))
        manifest = self._export(
            [
                {"id": f"dc{number:05d}", "kind": "decompile-type", "file": file, "type": name}
                for number, name in enumerate(ordered, start=1)
            ]
        )
        return {
            name: str(json.loads(manifest.file(f"dc{number:05d}").read_text(encoding="utf-8"))["source"])
            for number, name in enumerate(ordered, start=1)
        }
