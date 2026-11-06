"""The ``icons`` block of a game record, resolved against one target.

Shared by ``icons inputs`` and ``icons build``: both start from the same answer to "which inputs does this
target's pack need", and neither touches the network to get it.
"""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import paths
from ..catalog import schema
from ..catalog.loader import Catalog, Target
from ..exit_codes import UsageError
from ..steam.install import COMPLETE_MARKER, SteamInput
from . import mappers

APPMANIFEST = "appmanifest_{app}.acf"


@dataclass(frozen=True)
class IconsRoute:
    catalog_game: str
    target: Target
    block: dict[str, Any]
    steam: SteamInput

    @property
    def pack_game(self) -> str:
        return str(self.block["game"])

    @property
    def mapper(self) -> str:
        return str(self.block["mapper"])

    @property
    def selectors(self) -> list[str]:
        return [str(selector) for selector in self.block["data"]["files"]]

    @property
    def depots(self) -> dict[str, str]:
        """Depot id to the pinned manifest, for the depots the data comes from."""
        wanted = self.block["data"].get("depots") or sorted(self.steam.depots)
        return {str(depot): str(self.steam.depots[str(depot)]["manifest"]) for depot in wanted}

    @property
    def pixel_files(self) -> list[str]:
        return [str(name) for name in self.block["pixels"]["files"]]

    @property
    def client_builds(self) -> dict[str, list[int]]:
        return {
            str(rev): [int(i) for i in ids] for rev, ids in (self.block["pixels"].get("clientBuilds") or {}).items()
        }

    @property
    def secrets(self) -> list[str]:
        return [str(name) for name in self.block.get("secrets") or []]

    def data_cache(self, cache: Path, depot: str) -> Path:
        """Where a filtered download of one depot lives; the selectors are part of the key.

        Deliberately not ``steam/<app>/<depot>/<manifest>``: that directory means "the whole depot" and the
        installer trusts its marker, so a partial download there would be installed as if it were complete.
        """
        canonical = json.dumps(sorted(set(self.selectors)), separators=(",", ":"))
        key = hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:16]
        return cache / "icons" / "steam" / str(self.steam.app) / depot / self.depots[depot] / key


def route_for(catalog: Catalog, game: str, target_id: str | None, platform: str | None) -> IconsRoute:
    target = catalog.select(game, target_id=target_id, platform=platform)
    block = catalog.game(game).record.get("icons")
    if block is None:
        raise UsageError(f"game '{game}' has no icons block")
    errors = schema.errors_for("icons.schema.json", block)
    if errors:
        raise UsageError(f"game '{game}' has an invalid icons block", problems=errors)
    if block["mapper"] not in mappers.names():
        raise UsageError(f"game '{game}' names mapper '{block['mapper']}'; known mappers: {', '.join(mappers.names())}")
    steam = SteamInput.of(target.record)
    if steam.name != block["data"]["input"]:
        raise UsageError(f"target '{target.id}' has no steam-depots input named '{block['data']['input']}'")
    return IconsRoute(catalog_game=game, target=target, block=block, steam=steam)


def filtered_cache_complete(directory: Path) -> bool:
    return (directory / COMPLETE_MARKER).is_file()


def inputs_document(route: IconsRoute) -> dict[str, Any]:
    """The ``icons inputs`` answer: no network, no DepotDownloader."""
    cache = paths.cache_dir()
    caches = {depot: route.data_cache(cache, depot) for depot in route.depots}
    pixels = route.block["pixels"]
    document: dict[str, Any] = {
        "game": route.catalog_game,
        "packGame": route.pack_game,
        "target": route.target.id,
        "fingerprint": route.target.fingerprint,
        "revision": route.target.revision,
        "mapper": route.mapper,
        "inputs": [
            {
                "role": "data",
                "source": "server-depot",
                "app": route.steam.app,
                "branch": route.steam.branch,
                "buildid": route.steam.buildid,
                "os": route.steam.os_,
                "depots": route.depots,
                "files": route.selectors,
                "cache": {depot: str(path) for depot, path in caches.items()},
                "cached": all(filtered_cache_complete(path) for path in caches.values()),
            },
            {
                "role": "pixels",
                "source": "client",
                "app": pixels["app"],
                "depot": pixels["depot"],
                "decoder": pixels["decoder"],
                "files": route.pixel_files,
                "clientManifest": APPMANIFEST.format(app=pixels["app"]),
                "expectedClientBuilds": route.client_builds.get(route.target.revision, []),
                "suggestedMirror": str(cache / "icons" / "clients" / route.pack_game / str(pixels["app"])),
            },
        ],
        "env": [{"name": name, "set": bool(os.environ.get(name))} for name in route.secrets],
    }
    return document
