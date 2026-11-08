"""What the pipeline hands the Minecraft mapper for the data reports of the server jar: one container run, cached."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..exit_codes import BuildFailed
from .decoders.minecraft_datagen import DatagenDecoder

MAX_LISTED_FAILURES = 20
SERVER_JAR = "server.jar"


class DatagenSession:
    def __init__(self, decoder: DatagenDecoder, *, jar_root: Path, jar_sha256: str) -> None:
        self.decoder = decoder
        self._jar_root = jar_root
        self._jar_sha256 = jar_sha256
        self.tool: dict[str, Any] = {}

    def reports(self, names: list[str]) -> dict[str, Path]:
        """The named data reports (``registries.json``, ...), from one generator run over ``server.jar``."""
        ids = {name: "r-" + name.removesuffix(".json").replace("_", "-") for name in names}
        manifest = self.decoder.export(
            self._jar_root,
            [{"id": ids[name], "kind": "datagen-report", "file": SERVER_JAR, "report": name} for name in names],
            inputs_sha256=self._jar_sha256,
        )
        self.tool = manifest.tool
        failed = [f"{result['id']}: {result['error']}" for result in manifest.failures()]
        if failed:
            raise BuildFailed(
                f"the data generator failed on {len(failed)} of {len(names)} reports",
                failures=failed[:MAX_LISTED_FAILURES],
            )
        return {name: manifest.file(ids[name]) for name in names}
