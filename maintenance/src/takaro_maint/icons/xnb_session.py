"""What the pipeline hands a mapper for XNA games: the textures of selected content files, decoded to PNG."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from ..exit_codes import BuildFailed
from .decoders.outcomes import Missing, Outcome, Texture, Unsupported
from .decoders.xnb import XnbDecoder

__all__ = ["Missing", "Outcome", "Texture", "Unsupported", "XnbSession"]

MAX_LISTED_FAILURES = 20
_ITEM_FILE = re.compile(r"^Content/Images/Item_([0-9]+)\.xnb$")


class XnbSession:
    def __init__(self, decoder: XnbDecoder, *, mirror_root: Path, mirror_tree_sha256: str) -> None:
        self.decoder = decoder
        self._root = mirror_root
        self._tree_sha256 = mirror_tree_sha256
        self.tool: dict[str, Any] = {}

    @staticmethod
    def _number(file: str) -> int:
        match = _ITEM_FILE.fullmatch(file)
        if match is None:
            raise BuildFailed(f"{file!r} is not a Content/Images/Item_<number>.xnb file")
        return int(match.group(1))

    def textures(self, files: list[str]) -> dict[str, Outcome]:
        """One decoder run for every file, in item order; the answer holds an outcome per file."""
        ordered = sorted(files, key=self._number)
        ids = {file: f"it{self._number(file)}" for file in ordered}
        manifest = self.decoder.export(
            self._root,
            [{"id": ids[file], "kind": "texture", "file": file} for file in ordered],
            inputs_sha256=self._tree_sha256,
        )
        self.tool = manifest.tool
        failed = [
            f"{result['id']}: {result['error']}" for result in manifest.failures() if result["errorKind"] == "failed"
        ]
        if failed:
            raise BuildFailed(
                f"the decoder failed on {len(failed)} of {len(ordered)} textures", failures=failed[:MAX_LISTED_FAILURES]
            )
        outcomes: dict[str, Outcome] = {}
        for file in ordered:
            result = manifest.results[ids[file]]
            if result["ok"]:
                meta = result["meta"]
                outcomes[file] = Texture(
                    manifest.file(ids[file]), int(meta["width"]), int(meta["height"]), str(meta["format"])
                )
            elif result["errorKind"] == "unsupported-format":
                outcomes[file] = Unsupported(str(result["error"]))
            else:
                outcomes[file] = Missing(str(result["error"]))
        return outcomes
