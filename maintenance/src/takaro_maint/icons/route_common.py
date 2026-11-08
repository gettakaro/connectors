"""The steps every catalog route shares: map to recipes, render the pack, and open the build report."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import output
from . import acquire, build, mappers, provenance
from .catalog_route import IconsRoute


@dataclass(frozen=True)
class ClientSide:
    """The installed client a client-sourced route reads its pixels from, and how it compares with the target."""

    directory: Path
    buildid: int | None
    manifest: str | None
    status: str
    expected: list[int]
    warnings: list[str]

    def record(self, route: IconsRoute) -> dict[str, Any]:
        return {
            "app": route.block["pixels"]["app"],
            "depot": route.block["pixels"]["depot"],
            "buildid": self.buildid,
            "manifest": self.manifest,
            "status": self.status,
            "expected": self.expected,
        }


def client_build_side(route: IconsRoute, directory: Path) -> ClientSide:
    """How the installed client's build compares with the target's release; warns when they differ."""
    buildid, manifest = acquire.read_client_build(
        directory, route.block["pixels"]["app"], route.block["pixels"]["depot"]
    )
    status, expected, warning = provenance.client_status(buildid, route.target.revision, route.client_builds)
    warnings = [warning] if warning else []
    for message in warnings:
        output.warn(message)
    return ClientSide(directory, buildid, manifest, status, expected, warnings)


def map_recipes(
    route: IconsRoute, mapper: mappers.Mapper, context: mappers.MapContext
) -> tuple[mappers.MapResult, dict[str, Any], bytes]:
    """Run the mapper; returns its result, the validated recipe document of the route's pack, and its bytes."""
    mapped = mapper.run(context)
    document = {**mapped.document, "game": route.pack_game}
    build.validate_recipes(document)
    return mapped, document, (json.dumps(document, indent=2) + "\n").encode("utf-8")


def write_pack(
    document: dict[str, Any], recipes_bytes: bytes, work: Path, out: Path, record: dict[str, Any]
) -> tuple[build.BuildResult, dict[str, Path]]:
    """Render the pack and write ``recipes.json`` next to it; returns the result and every report path."""
    result = build.build_pack(
        document,
        work / "textures-root",
        out,
        recipes_sha256=hashlib.sha256(recipes_bytes).hexdigest(),
        provenance=record,
    )
    (out / "recipes.json").write_bytes(recipes_bytes)
    return result, {**result.reports, "recipes": out / "recipes.json"}


def report_head(route: IconsRoute, result: build.BuildResult, reports: dict[str, Path]) -> dict[str, Any]:
    """The report keys every route opens with, in order."""
    return {
        "game": result.game,
        "catalogGame": route.catalog_game,
        "target": route.target.id,
        "fingerprint": route.target.fingerprint,
        "pack": str(result.pack_dir.resolve()),
        "index": str(result.index_path.resolve()),
        "codes": result.codes,
        "objects": result.objects,
        "coverage": result.coverage,
        "reports": {name: str(path.resolve()) for name, path in reports.items()},
        "encoder": result.encoder,
    }
