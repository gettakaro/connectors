"""The build of a route whose item codes are files of this repository and whose pixels and code come from a client.

Everything that can fail on a missing input (docker, the client, its files) fails before an image is built or a file
is copied. The decoder containers read one read-only mirror of the selected client files, never ``--client`` itself.
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Any

from .. import net, output, paths
from . import acquire, client_files, mappers, provenance, route_common
from .catalog_route import IconsRoute
from .clr_session import ClrSession
from .decoders import clr as clr_decoder
from .decoders import container
from .decoders import xnb as xnb_decoder
from .xnb_session import XnbSession


def _decoder_record(image: container.DecoderImage, tool: dict[str, Any]) -> dict[str, Any]:
    return {"name": image.name, "imageId": image.id, "contextSha256": image.context_sha256, "tool": tool}


def build_from_connector(route: IconsRoute, mapper: mappers.Mapper, client: Path | None, out: Path) -> dict[str, Any]:
    container.require_docker()
    directory, selected = acquire.check_client_selectors(client, route.client_selectors)
    client_side = route_common.client_build_side(route, directory)
    xnb_image = container.ensure_image(xnb_decoder.DECODER_NAME)
    clr_image = container.ensure_image(clr_decoder.DECODER_NAME)

    cache = paths.cache_dir()
    work_root = cache / "icons" / "work"
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work_root, prefix="build-") as scratch:
        work = Path(scratch)
        data = acquire.read_connector_data(route, work)
        data_files = {
            name: {key: net.hash_file(data.root / name)[key] for key in ("sha256", "size")}
            for name in route.connector_files
        }
        mirror = client_files.mirror(
            cache,
            pack=route.pack_game,
            app=int(route.block["pixels"]["app"]),
            buildid=client_side.buildid,
            client=directory,
            files=selected,
        )
        xnb_session = XnbSession(
            xnb_decoder.XnbDecoder(xnb_image, cache), mirror_root=mirror.root, mirror_tree_sha256=mirror.tree_sha256
        )
        clr_session = ClrSession(
            clr_decoder.ClrDecoder(clr_image, cache), mirror_root=mirror.root, mirror_tree_sha256=mirror.tree_sha256
        )
        context = mappers.MapContext(
            data_root=data.root,
            pixels=None,
            work=work,
            codes_from="",
            codes_from_inputs={
                "source": "connector",
                "files": {name: row["sha256"] for name, row in data_files.items()},
            },
            unreal=None,
            warn=output.warn,
            xnb=xnb_session,
            clr=clr_session,
            client_files=selected,
        )
        mapped, document, recipes_bytes = route_common.map_recipes(route, mapper, context)
        pixel_files = [name for name in selected if name not in route.code_files]
        decoders = [_decoder_record(xnb_image, xnb_session.tool), _decoder_record(clr_image, clr_session.tool)]
        record: dict[str, Any] = {
            "source": "catalog",
            "catalogGame": route.catalog_game,
            "target": route.target.id,
            "revision": route.target.revision,
            "fingerprint": route.target.fingerprint,
            "data": {"source": "connector", "files": data_files, "treeSha256": data.tree_sha256},
            "mapper": {"name": mapper.name, "version": mapper.version, "notes": mapped.notes, **mapped.details},
            "client": client_side.record(route),
            "code": {
                "source": "client",
                "decoder": route.code_decoder,
                "files": {name: mirror.files[name] for name in route.code_files},
                "assembly": mapped.details["assembly"],
            },
            "pixels": {
                "source": "client",
                "decoder": xnb_decoder.DECODER_NAME,
                "selectors": route.pixel_files,
                "files": len(pixel_files),
                "bytes": sum(mirror.files[name]["size"] for name in pixel_files),
                "treeSha256": mirror.tree_sha256,
                "used": mapped.details["textures"],
                "alpha": route.block["xnb"]["alpha"],
            },
            "decoders": decoders,
            "tools": provenance.tools_record(
                f"{xnb_decoder.DECODER_NAME} container ({xnb_image.tag}), "
                f"{clr_decoder.DECODER_NAME} container ({clr_image.tag})"
            ),
            "warnings": client_side.warnings,
        }
        result, reports = route_common.write_pack(document, recipes_bytes, work, out, record)
    return {
        **route_common.report_head(route, result, reports),
        "inputs": {
            "data": {
                "source": "connector",
                "files": {name: row["sha256"] for name, row in data_files.items()},
                "treeSha256": data.tree_sha256,
            },
            "pixels": {"source": "client", "decoder": xnb_decoder.DECODER_NAME, "files": len(pixel_files)},
        },
        "clientBuild": {"id": client_side.buildid, "status": client_side.status, "expected": client_side.expected},
        "decoders": [
            {"name": item["name"], "imageId": item["imageId"], "contextSha256": item["contextSha256"]}
            for item in decoders
        ],
        "warnings": client_side.warnings,
    }
