"""The build of a route whose data and pixels are public downloads pinned by hash (Minecraft).

Everything that can fail on a missing input fails before the first byte is downloaded: the output directory, the target,
the client pin, the mapper, ``--client`` and docker are all checked first. The server jar is mounted read-only into the
data-generator container; the client jar is read in-process.
"""

from __future__ import annotations

import hashlib
import json
import tempfile
from pathlib import Path
from typing import Any

from .. import net, output, paths
from ..exit_codes import UsageError
from . import build, mappers, provenance, public_download
from .catalog_route import IconsRoute
from .datagen_session import DatagenSession
from .decoders import container
from .decoders import minecraft_datagen as datagen_decoder


def build_from_public(route: IconsRoute, mapper: mappers.Mapper, client: Path | None, out: Path) -> dict[str, Any]:
    if client is not None:
        raise UsageError(f"game {route.catalog_game} takes its pixels from a public download; --client is not used")
    container.require_docker()
    image = container.ensure_image(datagen_decoder.DECODER_NAME)

    cache = paths.cache_dir()
    files = public_download.acquire(route.public, cache)
    session = DatagenSession(
        datagen_decoder.DatagenDecoder(image, cache), jar_root=files.server.path.parent, jar_sha256=files.server.sha256
    )
    reports = session.reports(route.public_reports)
    report_hashes = {name: net.sha256_file(path) for name, path in reports.items()}

    work_root = cache / "icons" / "work"
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work_root, prefix="build-") as scratch:
        work = Path(scratch)
        context = mappers.MapContext(
            data_root=files.server.path.parent,
            pixels=None,
            work=work,
            codes_from="",
            codes_from_inputs={"version": files.version, "serverSha1": files.server.sha1},
            unreal=None,
            warn=output.warn,
            minecraft=mappers.MinecraftInputs(
                client_jar=files.client.path,
                version=files.version,
                server_sha1=files.server.sha1,
                reports=reports,
            ),
        )
        mapped = mapper.run(context)
        document = {**mapped.document, "game": route.pack_game}
        build.validate_recipes(document)
        recipes_bytes = (json.dumps(document, indent=2) + "\n").encode("utf-8")
        decoders = [
            {"name": image.name, "imageId": image.id, "contextSha256": image.context_sha256, "tool": session.tool}
        ]
        record: dict[str, Any] = {
            "source": "catalog",
            "catalogGame": route.catalog_game,
            "target": route.target.id,
            "revision": route.target.revision,
            "fingerprint": route.target.fingerprint,
            "data": {
                "source": "public-download",
                "input": route.public.input,
                "version": files.version,
                "manifest": {"url": files.manifest.url, "sha1": files.manifest.sha1},
                "server": files.server.record(),
                "reports": report_hashes,
            },
            "mapper": {"name": mapper.name, "version": mapper.version, "notes": mapped.notes, **mapped.details},
            "pixels": {
                "source": "public-download",
                "client": files.client.record(),
                "textures": mapped.details["textures"],
                "models": mapped.details["models"]["elements"],
            },
            "decoders": decoders,
            "tools": provenance.tools_record(
                f"{datagen_decoder.DECODER_NAME} container ({image.tag}); client.jar read in-process"
            ),
            "warnings": [],
        }
        result = build.build_pack(
            document,
            work / "textures-root",
            out,
            recipes_sha256=hashlib.sha256(recipes_bytes).hexdigest(),
            provenance=record,
        )
    (out / "recipes.json").write_bytes(recipes_bytes)
    written = {**result.reports, "recipes": out / "recipes.json"}
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
        "reports": {name: str(path.resolve()) for name, path in written.items()},
        "encoder": result.encoder,
        "inputs": {
            "data": {
                "source": "public-download",
                "version": files.version,
                "serverSha1": files.server.sha1,
                "reports": report_hashes,
            },
            "pixels": {"source": "public-download", "clientSha1": files.client.sha1},
        },
        "decoders": [
            {"name": item["name"], "imageId": item["imageId"], "contextSha256": item["contextSha256"]}
            for item in decoders
        ],
        "warnings": [],
    }
