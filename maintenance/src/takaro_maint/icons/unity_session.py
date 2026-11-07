"""What the pipeline hands a mapper for Unity games: bundle scans, sprite geometry and decoded atlas textures.

A scan answers "which serialized files live in which bundle" and dumps selected MonoBehaviours. Sprites,
textures and reference cuts are then requested by object reference, with the CAB index of the scan so the
decoder opens only the bundles it needs. Every call is one container run through the decode cache.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..exit_codes import BuildFailed
from .decoders import container
from .decoders.outcomes import Missing, Outcome, Texture, Unsupported
from .decoders.unity import UnityDecoder

__all__ = [
    "Missing",
    "Outcome",
    "Ref",
    "Scan",
    "ScriptSpec",
    "SpriteMeta",
    "Texture",
    "UnitySession",
    "Unsupported",
    "ref_of",
]

MAX_LISTED_FAILURES = 20

Ref = tuple[str, int]


def ref_of(value: dict[str, Any] | None) -> Ref | None:
    """``(cab, pathId)`` of a decoder object reference; ``None`` for the null reference."""
    if value is None:
        return None
    return (str(value["cab"]), int(value["pathId"]))


def _ref_json(ref: Ref) -> dict[str, Any]:
    return {"cab": ref[0], "pathId": ref[1]}


@dataclass(frozen=True)
class ScriptSpec:
    """A MonoBehaviour script by class, namespace and assembly (without ``.dll``), and the field paths to dump."""

    cls: str
    namespace: str
    assembly: str
    fields: list[str]


@dataclass(frozen=True)
class Scan:
    bundles: list[dict[str, Any]]
    behaviours: dict[str, list[dict[str, Any]]]

    def cab_index(self) -> dict[str, str]:
        """Serialized-file name to the bundle holding it; a name in two bundles is a failed build."""
        index: dict[str, str] = {}
        for bundle in self.bundles:
            for cab in bundle["cabs"]:
                if cab in index:
                    raise BuildFailed(
                        f"serialized file {cab!r} is in two bundles", bundles=sorted([index[cab], bundle["file"]])
                    )
                index[cab] = bundle["file"]
        return index


@dataclass(frozen=True)
class SpriteMeta:
    name: str
    rect: list[float]
    texture_rect: list[float]
    texture_rect_offset: list[float]
    packed: bool
    packing_mode: str
    packing_rotation: str
    texture: Ref
    alpha_texture: Ref | None
    atlas: Ref | None


class UnitySession:
    def __init__(self, decoder: UnityDecoder, *, data_root: Path, data_tree_sha256: str) -> None:
        self.decoder = decoder
        self._data_root = data_root
        self._data_tree_sha256 = data_tree_sha256
        self.tool: dict[str, Any] = {}

    def _export(self, exports: list[dict[str, Any]], cab_index: dict[str, str] | None = None) -> container.Manifest:
        manifest = self.decoder.export(
            self._data_root, exports, inputs_sha256=self._data_tree_sha256, cab_index=cab_index
        )
        self.tool = manifest.tool
        return manifest

    @staticmethod
    def _raise_failed(manifest: container.Manifest, labels: dict[str, str]) -> None:
        failed = [
            f"{result['id']} {labels.get(result['id'], '')}: {result['error']}".replace("  ", " ")
            for result in manifest.failures()
            if result["errorKind"] == "failed"
        ]
        if failed:
            raise BuildFailed(f"the decoder failed on {len(failed)} exports", failures=failed[:MAX_LISTED_FAILURES])

    def scan(self, files: list[str], scripts: list[ScriptSpec]) -> Scan:
        exports: list[dict[str, Any]] = [{"id": "bundles", "kind": "bundle-index", "files": files}]
        for number, script in enumerate(scripts):
            exports.append(
                {
                    "id": f"ms-{number}",
                    "kind": "monobehaviours",
                    "files": files,
                    "script": {"class": script.cls, "namespace": script.namespace, "assembly": script.assembly},
                    "fields": script.fields,
                }
            )
        manifest = self._export(exports)
        self._raise_failed(manifest, {"bundles": "bundle-index", **{f"ms-{n}": s.cls for n, s in enumerate(scripts)}})
        for result in manifest.results.values():
            if not result["ok"]:
                raise BuildFailed(f"the decoder could not scan {result['id']}: {result['error']}")
        bundles = json.loads(manifest.file("bundles").read_text(encoding="utf-8"))
        behaviours = {
            script.cls: json.loads(manifest.file(f"ms-{number}").read_text(encoding="utf-8"))
            for number, script in enumerate(scripts)
        }
        return Scan(bundles, behaviours)

    def sprites(self, refs: set[Ref], cab_index: dict[str, str]) -> dict[Ref, SpriteMeta | Missing]:
        ordered = sorted(refs)
        ids = {ref: f"sp{number:05d}" for number, ref in enumerate(ordered, start=1)}
        manifest = self._export(
            [{"id": ids[ref], "kind": "sprite", "ref": _ref_json(ref)} for ref in ordered], cab_index
        )
        self._raise_failed(manifest, {ids[ref]: f"{ref[0]}:{ref[1]}" for ref in ordered})
        outcomes: dict[Ref, SpriteMeta | Missing] = {}
        for ref in ordered:
            result = manifest.results[ids[ref]]
            if not result["ok"]:
                outcomes[ref] = Missing(f"{ref[0]}:{ref[1]}")
                continue
            meta = json.loads(manifest.file(ids[ref]).read_text(encoding="utf-8"))
            texture = ref_of(meta["texture"])
            assert texture is not None
            outcomes[ref] = SpriteMeta(
                name=str(meta["name"]),
                rect=[float(v) for v in meta["rect"]],
                texture_rect=[float(v) for v in meta["textureRect"]],
                texture_rect_offset=[float(v) for v in meta["textureRectOffset"]],
                packed=bool(meta["packed"]),
                packing_mode=str(meta["packingMode"]),
                packing_rotation=str(meta["packingRotation"]),
                texture=texture,
                alpha_texture=ref_of(meta["alphaTexture"]),
                atlas=ref_of(meta["atlas"]),
            )
        return outcomes

    def textures(self, refs: set[Ref], cab_index: dict[str, str]) -> dict[Ref, Outcome]:
        ordered = sorted(refs)
        ids = {ref: f"tx{number:05d}" for number, ref in enumerate(ordered, start=1)}
        manifest = self._export(
            [{"id": ids[ref], "kind": "texture", "ref": _ref_json(ref)} for ref in ordered], cab_index
        )
        self._raise_failed(manifest, {ids[ref]: f"{ref[0]}:{ref[1]}" for ref in ordered})
        outcomes: dict[Ref, Outcome] = {}
        for ref in ordered:
            result = manifest.results[ids[ref]]
            if result["ok"]:
                meta = result["meta"]
                outcomes[ref] = Texture(
                    manifest.file(ids[ref]), int(meta["width"]), int(meta["height"]), str(meta["format"])
                )
            elif result["errorKind"] == "unsupported-format":
                outcomes[ref] = Unsupported(str(result["error"]))
            else:
                outcomes[ref] = Missing(f"{ref[0]}:{ref[1]}")
        return outcomes

    def references(self, refs: set[Ref], cab_index: dict[str, str]) -> dict[Ref, Path | Missing]:
        """UnityPy's own cut of each sprite, the reference the atlas-cut op is checked against."""
        ordered = sorted(refs)
        ids = {ref: f"rf{number:05d}" for number, ref in enumerate(ordered, start=1)}
        manifest = self._export(
            [{"id": ids[ref], "kind": "sprite-image", "ref": _ref_json(ref)} for ref in ordered], cab_index
        )
        self._raise_failed(manifest, {ids[ref]: f"{ref[0]}:{ref[1]}" for ref in ordered})
        outcomes: dict[Ref, Path | Missing] = {}
        for ref in ordered:
            result = manifest.results[ids[ref]]
            outcomes[ref] = manifest.file(ids[ref]) if result["ok"] else Missing(f"{ref[0]}:{ref[1]}")
        return outcomes
