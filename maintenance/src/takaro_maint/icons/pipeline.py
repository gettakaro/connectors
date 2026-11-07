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
from . import acquire, build, client_ranges, mappers, provenance
from .catalog_route import IconsRoute, route_for
from .decoders import container
from .decoders import unreal as unreal_decoder
from .unreal_session import UnrealSession

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
    settings: unreal_decoder.UnrealSettings | None = None
    if route.uses_unreal:
        settings = unreal_decoder.UnrealSettings.of(route.block)
        unreal_decoder.require_key_variable(settings)
        container.require_docker()
        client_dir = client_ranges.check(client, route.pixel_files)
    else:
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
    session: UnrealSession | None = None
    image: container.DecoderImage | None = None
    oodle_sha256 = ""
    with tempfile.TemporaryDirectory(dir=work_root, prefix="build-") as scratch:
        work = Path(scratch)
        if settings is not None:
            oodle_path = unreal_decoder.ensure_oodle(cache)
            oodle_sha256 = unreal_decoder.read_oodle_lock().sha256
            image = container.ensure_image(unreal_decoder.DECODER_NAME)
        data = acquire.fetch_data(route, cache=cache, work=work)
        codes_from = CODES_FROM.format(
            description=mapper.codes_description,
            app=route.steam.app,
            buildid=route.steam.buildid,
            exclusions=mapper.exclusions,
        )
        pixels = None
        if settings is not None and image is not None:
            mirror_base = (
                cache
                / "icons"
                / "clients"
                / route.pack_game
                / str(route.block["pixels"]["app"])
                / (str(client_buildid) if client_buildid else "unknown")
            )
            session = UnrealSession(
                unreal_decoder.UnrealDecoder(settings, image, oodle_path, cache),
                data_root=data.root,
                data_tree_sha256=data.tree_sha256,
                client=client_dir,
                utoc_files=route.pixel_files,
                mirror_base=mirror_base,
            )
        else:
            pixels = mappers.open_pixels(client_dir, route.pixel_files)
        context = mappers.MapContext(
            data_root=data.root,
            pixels=pixels,
            work=work,
            codes_from=codes_from,
            codes_from_inputs={"app": route.steam.app, "buildid": route.steam.buildid},
            unreal=session,
            warn=output.warn,
        )
        mapped = mapper.run(context)
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
        record = _provenance(
            route, data, mapped, mapper, client_record, client_dir, session, image, oodle_sha256, settings
        )
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
    if session is not None and session.mirror is not None:
        ranges_path = out / "client-ranges.json"
        ranges_path.write_text(json.dumps(session.mirror.ranges, indent=2) + "\n", encoding="utf-8")
        reports["clientRanges"] = ranges_path
    report: dict[str, Any] = {
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
    if session is not None and session.mirror is not None and image is not None:
        report["decoder"] = {
            "name": unreal_decoder.DECODER_NAME,
            "imageId": image.id,
            "contextSha256": image.context_sha256,
        }
        report["inputs"]["pixels"]["rangeBytes"] = session.mirror.bytes
    return report


def _provenance(
    route: IconsRoute,
    data: acquire.DataTree,
    mapped: mappers.MapResult,
    mapper: mappers.Mapper,
    client: dict[str, Any],
    client_dir: Path,
    session: UnrealSession | None,
    image: container.DecoderImage | None,
    oodle_sha256: str,
    settings: unreal_decoder.UnrealSettings | None,
) -> dict[str, Any]:
    pixels = route.block["pixels"]
    record: dict[str, Any] = {
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
        "client": client,
        "mapper": {"name": mapper.name, "version": mapper.version, "notes": mapped.notes, **mapped.details},
    }
    if session is not None and session.mirror is not None and image is not None and settings is not None:
        mirror = session.mirror
        record["decoder"] = {
            "name": unreal_decoder.DECODER_NAME,
            "imageId": image.id,
            "contextSha256": image.context_sha256,
            "tool": session.tool,
            "oodleSha256": oodle_sha256,
            "engine": settings.engine,
        }
        record["pixels"] = {
            "source": "client",
            "decoder": pixels["decoder"],
            "access": route.pixel_access,
            "ranges": {
                "planSha256": mirror.plan_sha256,
                "containers": len({entry["file"] for entry in mirror.ranges}),
                "ranges": len(mirror.ranges),
                "bytes": mirror.bytes,
            },
            "files": {
                name: {
                    "sha256": info.sha256,
                    "size": info.size,
                    "version": info.version,
                    "entries": info.entries,
                    "partitions": info.partitions,
                }
                for name, info in mirror.containers.items()
            },
        }
        record["tools"] = provenance.tools_record(f"{unreal_decoder.DECODER_NAME} container ({image.tag})")
    else:
        pixel_files = acquire.hash_pixel_files(client_dir, route.pixel_files)
        record["pixels"] = {
            "source": "client",
            "decoder": pixels["decoder"],
            "files": {name: {**pixel_files[name], **mapped.pack_usage[name]} for name in route.pixel_files},
        }
        record["tools"] = provenance.tools_record()
    return record
