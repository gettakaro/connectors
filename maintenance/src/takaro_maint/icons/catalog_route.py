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

from .. import net, paths
from ..catalog import schema
from ..catalog.loader import Catalog, Target
from ..exit_codes import UsageError
from ..steam.install import COMPLETE_MARKER, SteamInput
from . import mappers
from .public_download import PublicInputs

APPMANIFEST = "appmanifest_{app}.acf"


@dataclass(frozen=True)
class IconsRoute:
    catalog_game: str
    target: Target
    block: dict[str, Any]
    steam_input: SteamInput | None
    public_input: PublicInputs | None = None

    @property
    def public(self) -> PublicInputs:
        """The pins of a public-download route; only such a route has them."""
        if self.public_input is None:
            raise UsageError(
                f"game '{self.catalog_game}' takes its data from {self.data_source}, not a public download"
            )
        return self.public_input

    @property
    def public_reports(self) -> list[str]:
        return [str(name) for name in self.block["data"].get("reports") or []]

    @property
    def steam(self) -> SteamInput:
        """The target's steam-depots input; only a server-depot route has one."""
        if self.steam_input is None:
            raise UsageError(f"game '{self.catalog_game}' takes its data from {self.data_source}, not a server depot")
        return self.steam_input

    @property
    def pack_game(self) -> str:
        return str(self.block["game"])

    @property
    def mapper(self) -> str:
        return str(self.block["mapper"])

    @property
    def data_source(self) -> str:
        """``server-depot``, ``connector`` (files of this repository) or ``public-download``."""
        return str(self.block["data"]["source"])

    @property
    def connector_files(self) -> list[str]:
        """Repo-relative connector files of a connector route; empty for a server-depot route."""
        if self.data_source != "connector":
            return []
        return [str(name) for name in self.block["data"]["files"]]

    @property
    def code_files(self) -> list[str]:
        return [str(name) for name in (self.block.get("code") or {}).get("files") or []]

    @property
    def code_decoder(self) -> str | None:
        decoder = (self.block.get("code") or {}).get("decoder")
        return str(decoder) if decoder is not None else None

    @property
    def client_selectors(self) -> list[str]:
        """The exact files and ``regex:`` entries of the pixel and code roles, which share one client mirror."""
        return list(dict.fromkeys([*self.pixel_files, *self.code_files]))

    @property
    def decoders(self) -> set[str]:
        """The decoder containers this route runs."""
        names = {str(self.block["pixels"].get("decoder")), str(self.block["data"].get("decoder")), self.code_decoder}
        return {name for name in names if name in ("unreal", "unity", "xnb", "clr", "minecraft-datagen")}

    @property
    def data_selectors(self) -> list[str]:
        if self.data_source != "server-depot":
            return []
        return [str(selector) for selector in self.block["data"]["files"]]

    @property
    def pixel_source(self) -> str:
        """``client`` (an installed game client) or ``server-depot`` (the pixel files ride the data download)."""
        return str(self.block["pixels"]["source"])

    @property
    def selectors(self) -> list[str]:
        """Everything the filtered depot download fetches: the data files and, for server pixels, the pixel files."""
        listed = self.data_selectors
        if self.pixel_source == "server-depot":
            listed = listed + self.pixel_selectors
        return list(dict.fromkeys(listed))

    @property
    def depots(self) -> dict[str, str]:
        """Depot id to the pinned manifest, for the depots the data comes from; empty for a connector route."""
        if self.steam_input is None:
            return {}
        wanted = self.block["data"].get("depots") or sorted(self.steam.depots)
        return {str(depot): str(self.steam.depots[str(depot)]["manifest"]) for depot in wanted}

    @property
    def uses_unreal(self) -> bool:
        return "unreal" in self.block

    @property
    def decoder(self) -> str | None:
        """The decoder container that reads the data files (``unreal``, ``unity``), else ``None``."""
        decoder = self.block["data"].get("decoder")
        return str(decoder) if decoder is not None else None

    @property
    def uses_unity(self) -> bool:
        return self.decoder == "unity"

    @property
    def pixel_selectors(self) -> list[str]:
        """The server-depot pixel selectors; empty for a client route."""
        if self.pixel_source != "server-depot":
            return []
        return [str(selector) for selector in self.block["pixels"]["files"]]

    @property
    def pixel_access(self) -> str:
        return str(self.block["pixels"].get("access", "files"))

    @property
    def pixel_files(self) -> list[str]:
        """Exact client-relative paths of a client route; empty for server pixels."""
        if self.pixel_source != "client":
            return []
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
    if block["data"]["source"] == "connector":
        missing = [
            name
            for name in block["data"]["files"]
            if not (paths.repo_root() / paths.safe_relative(name, field="icons.data.files")).is_file()
        ]
        if missing:
            raise UsageError(f"game '{game}' reads connector files that do not exist: {', '.join(missing)}")
        return IconsRoute(catalog_game=game, target=target, block=block, steam_input=None)
    if block["data"]["source"] == "public-download":
        public = PublicInputs.of(catalog.game(game).record, target, block)
        return IconsRoute(catalog_game=game, target=target, block=block, steam_input=None, public_input=public)
    steam = SteamInput.of(target.record)
    if steam.name != block["data"]["input"]:
        raise UsageError(f"target '{target.id}' has no steam-depots input named '{block['data']['input']}'")
    return IconsRoute(catalog_game=game, target=target, block=block, steam_input=steam)


def _unreal_inputs(route: IconsRoute, cache: Path) -> dict[str, Any]:
    from .decoders import container
    from .decoders import unreal as unreal_decoder

    spec = unreal_decoder.read_oodle_lock()
    return {
        "engine": route.block["unreal"]["engine"],
        "aesKeyEnv": route.block["unreal"]["aesKeyEnv"],
        "decoder": container.inspect_state(unreal_decoder.DECODER_NAME),
        "oodle": {"version": spec.version, "sha256": spec.sha256, "cached": unreal_decoder.oodle_cached(cache)},
    }


def filtered_cache_complete(directory: Path) -> bool:
    return (directory / COMPLETE_MARKER).is_file()


def _pixels_input(route: IconsRoute, cache: Path, cache_paths: dict[str, Path], cached: bool) -> dict[str, Any]:
    pixels = route.block["pixels"]
    if route.pixel_source == "server-depot":
        server_pixels: dict[str, Any] = {
            "role": "pixels",
            "source": "server-depot",
            "depots": route.depots,
            "files": route.pixel_selectors,
            "cache": {depot: str(path) for depot, path in cache_paths.items()},
            "cached": cached,
        }
        if "decoder" in pixels:
            server_pixels["decoder"] = pixels["decoder"]
        return server_pixels
    return {
        "role": "pixels",
        "source": "client",
        "app": pixels["app"],
        "depot": pixels["depot"],
        "decoder": pixels["decoder"],
        "files": route.pixel_files,
        "clientManifest": APPMANIFEST.format(app=pixels["app"]),
        "expectedClientBuilds": route.client_builds.get(route.target.revision, []),
        "suggestedMirror": str(cache / "icons" / "clients" / route.pack_game / str(pixels["app"])),
    }


def _connector_inputs_document(route: IconsRoute) -> dict[str, Any]:
    """The ``icons inputs`` answer of a route whose item codes are files of this repository."""
    from .decoders import container

    cache = paths.cache_dir()
    pixels_input = _pixels_input(route, cache, {}, False)
    inputs: list[dict[str, Any]] = [
        {
            "role": "data",
            "source": "connector",
            "files": {name: net.hash_file(paths.repo_root() / name)["sha256"] for name in route.connector_files},
        }
    ]
    if route.code_files:
        inputs.append({"role": "code", "source": "client", "decoder": route.code_decoder, "files": route.code_files})
    inputs.append(pixels_input)
    return {
        "game": route.catalog_game,
        "packGame": route.pack_game,
        "target": route.target.id,
        "fingerprint": route.target.fingerprint,
        "revision": route.target.revision,
        "mapper": route.mapper,
        "inputs": inputs,
        "decoders": {name: container.inspect_state(name) for name in sorted(route.decoders)},
        "env": [{"name": name, "set": bool(os.environ.get(name))} for name in route.secrets],
    }


def _public_inputs_document(route: IconsRoute) -> dict[str, Any]:
    """The ``icons inputs`` answer of a route whose data and pixels are public downloads pinned by hash."""
    from .decoders import container

    cache = paths.cache_dir()
    public = route.public
    return {
        "game": route.catalog_game,
        "packGame": route.pack_game,
        "target": route.target.id,
        "fingerprint": route.target.fingerprint,
        "revision": route.target.revision,
        "mapper": route.mapper,
        "inputs": [
            {
                "role": "data",
                "source": "public-download",
                "input": public.input,
                "artifact": "server",
                "version": public.version,
                "manifest": {"url": public.manifest.url, "sha1": public.manifest.sha1},
                "url": public.server.url,
                "sha1": public.server.sha1,
                "size": public.server.size,
                "decoder": route.decoder,
                "reports": route.public_reports,
                "cached": public.server.cached(cache) and public.manifest.cached(cache),
            },
            {
                "role": "pixels",
                "source": "public-download",
                "artifact": "client",
                "url": public.client.url,
                "sha1": public.client.sha1,
                "size": public.client.size,
                "cached": public.client.cached(cache),
            },
        ],
        "decoders": {name: container.inspect_state(name) for name in sorted(route.decoders)},
        "env": [{"name": name, "set": bool(os.environ.get(name))} for name in route.secrets],
    }


def inputs_document(route: IconsRoute) -> dict[str, Any]:
    """The ``icons inputs`` answer: no network, no DepotDownloader."""
    if route.data_source == "connector":
        return _connector_inputs_document(route)
    if route.data_source == "public-download":
        return _public_inputs_document(route)
    cache = paths.cache_dir()
    caches = {depot: route.data_cache(cache, depot) for depot in route.depots}
    cached = all(filtered_cache_complete(path) for path in caches.values())
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
                "files": route.data_selectors,
                "cache": {depot: str(path) for depot, path in caches.items()},
                "cached": cached,
            },
            _pixels_input(route, cache, caches, cached),
        ],
        "env": [{"name": name, "set": bool(os.environ.get(name))} for name in route.secrets],
    }
    if route.uses_unity:
        from .decoders import container
        from .decoders import unity as unity_decoder

        document["unity"] = {"decoder": container.inspect_state(unity_decoder.DECODER_NAME)}
    if route.uses_unreal:
        document["unreal"] = _unreal_inputs(route, cache)
        pixels_input = document["inputs"][1]
        pixels_input["access"] = route.pixel_access
        if route.pixel_access == "iostore-ranges":
            pixels_input["containers"] = route.pixel_files
            pixels_input["ranges"] = "computed during build from the mapping (expected about 130 MB)"
    return document
