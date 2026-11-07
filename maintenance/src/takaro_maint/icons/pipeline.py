"""The catalog-driven build: acquire the inputs, map them to recipes, render the pack.

Everything that can fail on a typo or a missing file fails before the first byte is downloaded: the output
directory, the target, the icons block, the mapper and the client files are all checked first.
"""

from __future__ import annotations

import hashlib
import json
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import output, paths
from ..catalog import load as load_catalog
from ..exit_codes import UsageError
from . import acquire, build, client_ranges, mappers, provenance
from .catalog_route import IconsRoute, route_for
from .decoders import container
from .decoders import unity as unity_decoder
from .decoders import unreal as unreal_decoder
from .unity_session import UnitySession
from .unreal_session import UnrealSession

CODES_FROM = (
    "Item codes derived from game data: {description} (server depot app {app}, build {buildid}), "
    "excluding {exclusions}. Not measured against a live code list from a staging server (#811 skipped)."
)


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


def _client_side(route: IconsRoute, client: Path | None) -> ClientSide | None:
    """Check the client before anything is downloaded; ``None`` for a route whose pixels ride the server depot."""
    if route.pixel_source == "server-depot":
        if client is not None:
            raise UsageError(f"game {route.catalog_game} takes its pixels from the server depot; --client is not used")
        return None
    if route.uses_unreal:
        directory = client_ranges.check(client, route.pixel_files)
    else:
        directory = acquire.check_client(client, route.pixel_files)
    return client_build_side(route, directory)


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


def build_from_catalog(
    game: str, target_id: str | None, platform: str | None, client: Path | None, out: Path
) -> dict[str, Any]:
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise UsageError(f"--out {out} exists and is not an empty directory")
    route = route_for(load_catalog(), game, target_id, platform)
    mapper = mappers.registry()[route.mapper]
    if route.data_source == "connector":
        from .connector_pipeline import build_from_connector

        return build_from_connector(route, mapper, client, out)
    if route.data_source == "public-download":
        from .public_pipeline import build_from_public

        return build_from_public(route, mapper, client, out)
    settings: unreal_decoder.UnrealSettings | None = None
    if route.uses_unreal:
        settings = unreal_decoder.UnrealSettings.of(route.block)
        unreal_decoder.require_key_variable(settings)
        container.require_docker()
    client_side = _client_side(route, client)
    warnings = client_side.warnings if client_side else []
    if route.uses_unity:
        container.require_docker()

    cache = paths.cache_dir()
    work_root = cache / "icons" / "work"
    work_root.mkdir(parents=True, exist_ok=True)
    session: UnrealSession | None = None
    unity_session: UnitySession | None = None
    image: container.DecoderImage | None = None
    oodle_sha256 = ""
    with tempfile.TemporaryDirectory(dir=work_root, prefix="build-") as scratch:
        work = Path(scratch)
        if settings is not None:
            oodle_path = unreal_decoder.ensure_oodle(cache)
            oodle_sha256 = unreal_decoder.read_oodle_lock().sha256
            image = container.ensure_image(unreal_decoder.DECODER_NAME)
        if route.uses_unity:
            image = container.ensure_image(unity_decoder.DECODER_NAME)
        data = acquire.fetch_data(route, cache=cache, work=work)
        codes_from = CODES_FROM.format(
            description=mapper.codes_description,
            app=route.steam.app,
            buildid=route.steam.buildid,
            exclusions=mapper.exclusions,
        )
        pixels = None
        if settings is not None and image is not None:
            assert client_side is not None
            mirror_base = (
                cache
                / "icons"
                / "clients"
                / route.pack_game
                / str(route.block["pixels"]["app"])
                / (str(client_side.buildid) if client_side.buildid else "unknown")
            )
            session = UnrealSession(
                unreal_decoder.UnrealDecoder(settings, image, oodle_path, cache),
                data_root=data.root,
                data_tree_sha256=data.tree_sha256,
                client=client_side.directory,
                utoc_files=route.pixel_files,
                mirror_base=mirror_base,
            )
        elif image is not None and route.uses_unity:
            unity_session = UnitySession(
                unity_decoder.UnityDecoder(image, cache), data_root=data.root, data_tree_sha256=data.tree_sha256
            )
        elif client_side is not None:
            pixels = mappers.open_pixels(client_side.directory, route.pixel_files)
        context = mappers.MapContext(
            data_root=data.root,
            pixels=pixels,
            work=work,
            codes_from=codes_from,
            codes_from_inputs={"app": route.steam.app, "buildid": route.steam.buildid},
            unreal=session,
            warn=output.warn,
            unity=unity_session,
        )
        mapped = mapper.run(context)
        document = {**mapped.document, "game": route.pack_game}
        build.validate_recipes(document)
        recipes_bytes = (json.dumps(document, indent=2) + "\n").encode("utf-8")
        record = _provenance(route, data, mapped, mapper, client_side, session, image, oodle_sha256, settings)
        if unity_session is not None and image is not None:
            _add_unity_pixels(record, route, unity_session, image, mapped)
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
        "inputs": {
            "data": {
                "buildid": route.steam.buildid,
                "depots": route.depots,
                "files": data.files,
                "treeSha256": data.tree_sha256,
            },
            "pixels": _pixels_report(route, mapped),
        },
        "warnings": warnings,
    }
    if client_side is not None:
        report["clientBuild"] = {
            "id": client_side.buildid,
            "status": client_side.status,
            "expected": client_side.expected,
        }
    if unity_session is not None and image is not None:
        report["decoder"] = {
            "name": unity_decoder.DECODER_NAME,
            "imageId": image.id,
            "contextSha256": image.context_sha256,
        }
    if session is not None and session.mirror is not None and image is not None:
        report["decoder"] = {
            "name": unreal_decoder.DECODER_NAME,
            "imageId": image.id,
            "contextSha256": image.context_sha256,
        }
        report["inputs"]["pixels"]["rangeBytes"] = session.mirror.bytes
    return report


def _pixels_report(route: IconsRoute, mapped: mappers.MapResult) -> dict[str, Any]:
    if route.uses_unity:
        return {
            "source": "server-depot",
            "decoder": unity_decoder.DECODER_NAME,
            "atlases": len(mapped.details["atlasFiles"]),
        }
    if route.pixel_source == "server-depot":
        return {"source": "server-depot", "files": mapped.details["icons"]["files"]}
    return {"files": len(route.pixel_files)}


def _provenance(
    route: IconsRoute,
    data: acquire.DataTree,
    mapped: mappers.MapResult,
    mapper: mappers.Mapper,
    client_side: ClientSide | None,
    session: UnrealSession | None,
    image: container.DecoderImage | None,
    oodle_sha256: str,
    settings: unreal_decoder.UnrealSettings | None,
) -> dict[str, Any]:
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
        "mapper": {
            "name": mapper.name,
            "version": mapper.version,
            "notes": mapped.notes,
            **{key: value for key, value in mapped.details.items() if key != "atlasFiles"},
        },
    }
    if route.uses_unity:
        return record
    if client_side is None:
        record["pixels"] = _server_pixels_record(route, mapped)
        record["tools"] = provenance.tools_record("none (PNG and XML read in-process)")
        return record
    record["client"] = client_side.record(route)
    if session is not None and session.mirror is not None and image is not None and settings is not None:
        _add_unreal_pixels(record, route, session, image, oodle_sha256, settings)
    else:
        pixel_files = acquire.hash_pixel_files(client_side.directory, route.pixel_files)
        record["pixels"] = {
            "source": "client",
            "decoder": route.block["pixels"]["decoder"],
            "files": {name: {**pixel_files[name], **mapped.pack_usage[name]} for name in route.pixel_files},
        }
        record["tools"] = provenance.tools_record()
    return record


def _add_unity_pixels(
    record: dict[str, Any],
    route: IconsRoute,
    session: UnitySession,
    image: container.DecoderImage,
    mapped: mappers.MapResult,
) -> None:
    """Atlas PNGs come out of the decoder; the bundles they were cut from are already in ``data.treeSha256``."""
    record["decoder"] = {
        "name": unity_decoder.DECODER_NAME,
        "imageId": image.id,
        "contextSha256": image.context_sha256,
        "tool": session.tool,
    }
    record["pixels"] = {
        "source": "server-depot",
        "decoder": unity_decoder.DECODER_NAME,
        "selectors": route.pixel_selectors,
        "atlases": mapped.details["atlasFiles"],
        "sprites": mapped.details["sprites"],
    }
    record["tools"] = provenance.tools_record(f"{unity_decoder.DECODER_NAME} container ({image.tag})")


def _server_pixels_record(route: IconsRoute, mapped: mappers.MapResult) -> dict[str, Any]:
    """The pixel files came down in the data download, so their hashes are already in ``data.treeSha256``."""
    icons = mapped.details["icons"]
    return {
        "source": "server-depot",
        "selectors": route.pixel_selectors,
        "files": icons["files"],
        "used": icons["used"],
    }


def _add_unreal_pixels(
    record: dict[str, Any],
    route: IconsRoute,
    session: UnrealSession,
    image: container.DecoderImage,
    oodle_sha256: str,
    settings: unreal_decoder.UnrealSettings,
) -> None:
    assert session.mirror is not None
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
        "decoder": route.block["pixels"]["decoder"],
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
