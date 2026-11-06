"""The catalog-driven build: acquire the inputs, map them to recipes, render the pack.

Everything that can fail on a typo or a missing file fails before the first byte is downloaded: the output
directory, the target, the icons block, the mapper and the client files are all checked first.
"""

from __future__ import annotations

import hashlib
import json
import tempfile
from pathlib import Path
from typing import Any

from .. import output, paths
from ..catalog import load as load_catalog
from ..exit_codes import UsageError
from . import acquire, build, mappers, provenance
from .catalog_route import IconsRoute, route_for

CODES_FROM = (
    "Item codes derived from game data: {description} (server depot app {app}, build {buildid}), "
    "excluding {exclusions}. Not measured against a live code list from a staging server (#811 skipped)."
)


def build_from_catalog(
    game: str, target_id: str | None, platform: str | None, client: Path | None, out: Path
) -> dict[str, Any]:
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise UsageError(f"--out {out} exists and is not an empty directory")
    route = route_for(load_catalog(), game, target_id, platform)
    mapper = mappers.registry()[route.mapper]
    client_dir = acquire.check_client(client, route.pixel_files)

    client_buildid, client_manifest = acquire.read_client_build(
        client_dir, route.block["pixels"]["app"], route.block["pixels"]["depot"]
    )
    status, expected, warning = provenance.client_status(client_buildid, route.target.revision, route.client_builds)
    warnings = [warning] if warning else []
    for message in warnings:
        output.warn(message)

    cache = paths.cache_dir()
    work_root = cache / "icons" / "work"
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work_root, prefix="build-") as scratch:
        work = Path(scratch)
        data = acquire.fetch_data(route, cache=cache, work=work)
        pixel_files = acquire.hash_pixel_files(client_dir, route.pixel_files)
        pixels = mappers.open_pixels(client_dir, route.pixel_files)
        codes_from = CODES_FROM.format(
            description=mapper.codes_description,
            app=route.steam.app,
            buildid=route.steam.buildid,
            exclusions=mapper.exclusions,
        )
        mapped = mapper.map_items(data.root, pixels, work / "textures-root", codes_from=codes_from)
        document = {**mapped.document, "game": route.pack_game}
        build.validate_recipes(document)
        recipes_bytes = (json.dumps(document, indent=2) + "\n").encode("utf-8")
        client_record = {
            "app": route.block["pixels"]["app"],
            "depot": route.block["pixels"]["depot"],
            "buildid": client_buildid,
            "manifest": client_manifest,
            "status": status,
            "expected": expected,
        }
        record = _provenance(route, data, pixel_files, mapped, mapper, client_record)
        record["warnings"] = warnings
        result = build.build_pack(
            document,
            work / "textures-root",
            out,
            recipes_sha256=hashlib.sha256(recipes_bytes).hexdigest(),
            provenance=record,
        )
    (out / "recipes.json").write_bytes(recipes_bytes)

    reports = {**result.reports, "recipes": out / "recipes.json"}
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
        "clientBuild": {"id": client_buildid, "status": status, "expected": expected},
        "inputs": {
            "data": {
                "buildid": route.steam.buildid,
                "depots": route.depots,
                "files": data.files,
                "treeSha256": data.tree_sha256,
            },
            "pixels": {"files": len(route.pixel_files)},
        },
        "warnings": warnings,
    }


def _provenance(
    route: IconsRoute,
    data: acquire.DataTree,
    pixel_files: dict[str, dict[str, Any]],
    mapped: mappers.MapResult,
    mapper: mappers.Mapper,
    client: dict[str, Any],
) -> dict[str, Any]:
    pixels = route.block["pixels"]
    return {
        "source": "catalog",
        "catalogGame": route.catalog_game,
        "target": route.target.id,
        "revision": route.target.revision,
        "fingerprint": route.target.fingerprint,
        "data": {
            "source": "server-depot",
            "app": route.steam.app,
            "branch": route.steam.branch,
            "buildid": route.steam.buildid,
            "os": route.steam.os_,
            "depots": route.depots,
            "selectors": route.selectors,
            "files": data.files,
            "treeSha256": data.tree_sha256,
        },
        "pixels": {
            "source": "client",
            "decoder": pixels["decoder"],
            "files": {name: {**pixel_files[name], **mapped.pack_usage[name]} for name in route.pixel_files},
        },
        "client": client,
        "mapper": {"name": mapper.name, "version": mapper.version, "notes": mapped.notes},
        "tools": provenance.tools_record(),
    }
