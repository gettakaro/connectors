#!/usr/bin/env python3
"""takaro-unity-export: facts out of Unity asset bundles, as JSON and PNG, for the icon pipeline.

Runs inside the decoder container: ``python -I /app/export.py /work/request.json`` with the game files
mounted read-only at the request's ``root`` and a writable ``/out``. Nothing here knows a game: it reads
serialized files and answers the generic questions in the request.

Request::

    {"schemaVersion": 1, "decoder": "unity", "settings": {"cabIndex": {<cab>: <bundle path>}} | {},
     "root": "/game", "exports": [{"id", "kind", ...}]}

Kinds: ``bundle-index {files}``, ``monobehaviours {files, script, fields}``, ``sprite {ref}``,
``texture {ref}`` and ``sprite-image {ref}`` (the last three need ``settings.cabIndex``). An object reference is
``{"cab": <lower-cased serialized-file name>, "pathId": <int>}`` or ``null``.

Bundles are opened one at a time (at most ``RESIDENT_BUNDLES`` stay open for reference kinds), so the peak
memory is about the largest bundle. Progress goes to stderr. The manifest carries no timings or memory figures.
"""

from __future__ import annotations

import gc
import json
import os
import sys
from collections import OrderedDict
from pathlib import Path, PurePosixPath
from typing import Any

import PIL
import UnityPy
from UnityPy.enums import SpritePackingMode, SpritePackingRotation, TextureFormat
from UnityPy.files import SerializedFile

OUT = Path("/out")
MAX_EDGE = 4096
RESIDENT_BUNDLES = 4
TOOL_VERSION = "1"
PACKING_MODES = {SpritePackingMode.kSPMTight: "tight", SpritePackingMode.kSPMRectangle: "rectangle"}
PACKING_ROTATIONS = {
    SpritePackingRotation.kSPRNone: "none",
    SpritePackingRotation.kSPRFlipHorizontal: "flip-horizontal",
    SpritePackingRotation.kSPRFlipVertical: "flip-vertical",
    SpritePackingRotation.kSPRRotate180: "rotate-180",
    SpritePackingRotation.kSPRRotate90: "rotate-90",
}


class Failed(Exception):
    """An export that cannot be answered; ``kind`` is the manifest ``errorKind``."""

    def __init__(self, message: str, kind: str = "failed") -> None:
        super().__init__(message)
        self.kind = kind


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def resolve_inside(root: Path, relative: str) -> Path:
    """A path below ``root``; an absolute path, a ``..`` part or a link out of the tree is refused."""
    pure = PurePosixPath(relative)
    if pure.is_absolute() or ".." in pure.parts or not pure.parts:
        raise Failed(f"refusing file path {relative!r}")
    path = (root / pure).resolve()
    if not path.is_relative_to(root.resolve()):
        raise Failed(f"{relative!r} resolves outside {root}")
    return path


def cab_of(path: str) -> str:
    return path.rsplit("/", 1)[-1].lower()


class Bundle:
    """One loaded bundle: its serialized files by lower-cased name."""

    def __init__(self, source: Path | list[Path]) -> None:
        listed = source if isinstance(source, list) else [source]
        self.environment = UnityPy.load(*[str(path) for path in listed])
        self.files: dict[str, SerializedFile] = {}
        for container in self.environment.files.values():
            for name, member in getattr(container, "files", {}).items():
                if isinstance(member, SerializedFile):
                    self.files[name.lower()] = member

    def reference(self, owner: SerializedFile, pointer: dict[str, Any] | None) -> dict[str, Any] | None:
        """The decoder's form of a ``{m_FileID, m_PathID}`` pointer seen from ``owner``."""
        if pointer is None:
            return None
        file_id, path_id = int(pointer["m_FileID"]), int(pointer["m_PathID"])
        if file_id == 0 and path_id == 0:
            return None
        if file_id == 0:
            return {"cab": cab_name(owner), "pathId": path_id}
        return {"cab": cab_of(owner.externals[file_id - 1].path), "pathId": path_id}


def cab_name(serialized: SerializedFile) -> str:
    return str(serialized.name).lower()


def is_pointer(value: Any) -> bool:
    return isinstance(value, dict) and set(value) == {"m_FileID", "m_PathID"}


def normalize(bundle: Bundle, owner: SerializedFile, value: Any) -> Any:
    if is_pointer(value):
        return bundle.reference(owner, value)
    if isinstance(value, dict):
        return {key: normalize(bundle, owner, item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [normalize(bundle, owner, item) for item in value]
    if isinstance(value, (bytes, bytearray, memoryview)):
        raise Failed("a requested field holds raw bytes")
    return value


def field_at(tree: dict[str, Any], dotted: str) -> Any:
    value: Any = tree
    for part in dotted.split("."):
        if not isinstance(value, dict) or part not in value:
            raise Failed(f"field path {dotted!r} is missing from the MonoBehaviour typetree")
        value = value[part]
    return value


def script_pointer(obj: Any) -> tuple[int, int]:
    """The ``m_Script`` pointer of a MonoBehaviour from its fixed header: GameObject, enabled flag, Script."""
    obj.reset()
    reader = obj.reader
    reader.read_int()
    reader.read_long()
    reader.read_byte()
    reader.read_bytes(3)
    return reader.read_int(), reader.read_long()


def objects_of(bundle: Bundle) -> Any:
    for name in sorted(bundle.files):
        serialized = bundle.files[name]
        for path_id in sorted(serialized.objects):
            yield serialized, serialized.objects[path_id]


def open_bundle(root: Path, relative: str) -> Bundle:
    return Bundle(resolve_inside(root, relative))


# ------------------------------------------------------------------------------------------ scanning


def scan(root: Path, exports: list[dict[str, Any]]) -> dict[str, Any]:
    """Answer every ``bundle-index`` and ``monobehaviours`` export in passes over the bundles, one at a time."""
    files = sorted({name for export in exports for name in export["files"]})
    wanted = [export for export in exports if export["kind"] == "monobehaviours"]
    index: dict[str, dict[str, Any]] = {}
    scripts: dict[tuple[str, int], tuple[str, str, str]] = {}
    for number, name in enumerate(files, start=1):
        log(f"scan pass A: bundle {number} of {len(files)}")
        bundle = open_bundle(root, name)
        cabs = sorted(bundle.files)
        versions = {str(bundle.files[cab].unity_version) for cab in cabs}
        index[name] = {"file": name, "unityVersion": sorted(versions)[0] if versions else "", "cabs": cabs}
        if wanted:
            for serialized, obj in objects_of(bundle):
                if obj.type.name == "MonoScript":
                    tree = obj.read_typetree()
                    assembly = str(tree.get("m_AssemblyName", ""))
                    scripts[(cab_name(serialized), obj.path_id)] = (
                        str(tree.get("m_ClassName", "")),
                        str(tree.get("m_Namespace", "")),
                        assembly[: -len(".dll")] if assembly.endswith(".dll") else assembly,
                    )
        del bundle
        gc.collect()
    results: dict[str, Any] = {"bundle-index": [index[name] for name in files]}
    dumps: dict[str, list[dict[str, Any]]] = {export["id"]: [] for export in wanted}
    failures: dict[str, str] = {}
    by_script: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    for export in wanted:
        spec = export["script"]
        by_script.setdefault((spec["class"], spec["namespace"], spec["assembly"]), []).append(export)
    if by_script:
        for number, name in enumerate(files, start=1):
            log(f"scan pass B: bundle {number} of {len(files)}")
            bundle = open_bundle(root, name)
            for serialized, obj in objects_of(bundle):
                if obj.type.name != "MonoBehaviour":
                    continue
                file_id, path_id = script_pointer(obj)
                if not 0 <= file_id <= len(serialized.externals):
                    continue
                owner = cab_name(serialized) if file_id == 0 else cab_of(serialized.externals[file_id - 1].path)
                script = scripts.get((owner, path_id))
                if script is None or script not in by_script:
                    continue
                tree = obj.read_typetree()
                game_object = bundle.reference(serialized, tree.get("m_GameObject"))
                entry_name = ""
                if game_object is not None and game_object["cab"] == cab_name(serialized):
                    target = serialized.objects.get(game_object["pathId"])
                    if target is not None:
                        entry_name = str(target.read_typetree().get("m_Name", ""))
                for export in by_script[script]:
                    try:
                        fields = {
                            path: normalize(bundle, serialized, field_at(tree, path)) for path in export["fields"]
                        }
                    except Failed as failure:
                        failures[export["id"]] = str(failure)
                        continue
                    dumps[export["id"]].append(
                        {
                            "bundle": name,
                            "object": {"cab": cab_name(serialized), "pathId": obj.path_id},
                            "gameObject": {**(game_object or {}), "name": entry_name},
                            "fields": fields,
                        }
                    )
            del bundle
        gc.collect()
    results.update(dumps)
    results["failures"] = failures
    return results


# ------------------------------------------------------------------------------------ object lookups


class Bundles:
    """Bundles opened on demand through the CAB index, the most recently used few kept open."""

    def __init__(self, root: Path, cab_index: dict[str, str]) -> None:
        self.root = root
        self.cab_index = {cab.lower(): path for cab, path in cab_index.items()}
        self.open: OrderedDict[str, Bundle] = OrderedDict()

    def bundle_for(self, cab: str) -> Bundle:
        path = self.cab_index.get(cab)
        if path is None:
            raise Failed(f"no bundle holds serialized file {cab!r}", "not-found")
        if path in self.open:
            self.open.move_to_end(path)
            return self.open[path]
        while len(self.open) >= RESIDENT_BUNDLES:
            self.open.popitem(last=False)
            gc.collect()
        log(f"opening bundle {path}")
        self.open[path] = open_bundle(self.root, path)
        return self.open[path]

    def object(self, cab: str, path_id: int) -> tuple[Bundle, SerializedFile, Any]:
        bundle = self.bundle_for(cab)
        serialized = bundle.files.get(cab)
        if serialized is None or path_id not in serialized.objects:
            raise Failed(f"object {cab}:{path_id} does not exist", "not-found")
        return bundle, serialized, serialized.objects[path_id]


def ref_arg(export: dict[str, Any]) -> tuple[str, int]:
    ref = export["ref"]
    return str(ref["cab"]).lower(), int(ref["pathId"])


def render_data(
    bundles: Bundles, sprite_tree: dict[str, Any], bundle: Bundle, owner: SerializedFile
) -> tuple[dict[str, Any], Bundle, SerializedFile, dict[str, Any] | None]:
    """The sprite's render data, from its SpriteAtlas entry when it has one, as UnityPy's SpriteHelper reads it."""
    atlas_ref = bundle.reference(owner, sprite_tree.get("m_SpriteAtlas"))
    if atlas_ref is None:
        return sprite_tree["m_RD"], bundle, owner, None
    atlas_bundle, atlas_file, atlas_obj = bundles.object(atlas_ref["cab"], atlas_ref["pathId"])
    atlas_tree = atlas_obj.read_typetree()
    key = sprite_tree["m_RenderDataKey"]
    for entry_key, value in atlas_tree["m_RenderDataMap"]:
        if entry_key == key:
            return value, atlas_bundle, atlas_file, atlas_ref
    raise Failed(f"the sprite atlas has no render data for key {key!r}")


def export_sprite(bundles: Bundles, export: dict[str, Any], out_json: Path) -> dict[str, Any]:
    cab, path_id = ref_arg(export)
    bundle, owner, obj = bundles.object(cab, path_id)
    if obj.type.name != "Sprite":
        raise Failed(f"{cab}:{path_id} is a {obj.type.name}, not a Sprite")
    tree = obj.read_typetree()
    data, data_bundle, data_file, atlas_ref = render_data(bundles, tree, bundle, owner)
    settings = int(data["settingsRaw"])
    texture_rect, offset, rect = data["textureRect"], data["textureRectOffset"], tree["m_Rect"]
    packed = bool(settings & 1)
    meta = {
        "name": str(tree["m_Name"]),
        "rect": [rect["x"], rect["y"], rect["width"], rect["height"]],
        "textureRect": [texture_rect["x"], texture_rect["y"], texture_rect["width"], texture_rect["height"]],
        "textureRectOffset": [offset["x"], offset["y"]],
        "packed": packed,
        "packingMode": PACKING_MODES[SpritePackingMode((settings >> 1) & 1)],
        "packingRotation": PACKING_ROTATIONS[SpritePackingRotation((settings >> 2) & 0xF)],
        "texture": data_bundle.reference(data_file, data["texture"]),
        "alphaTexture": data_bundle.reference(data_file, data["alphaTexture"]),
        "atlas": atlas_ref,
    }
    out_json.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return {}


def export_texture(bundles: Bundles, export: dict[str, Any], out_png: Path) -> dict[str, Any]:
    cab, path_id = ref_arg(export)
    _, _, obj = bundles.object(cab, path_id)
    if obj.type.name != "Texture2D":
        raise Failed(f"{cab}:{path_id} is a {obj.type.name}, not a Texture2D")
    texture = obj.parse_as_object()
    width, height = int(texture.m_Width), int(texture.m_Height)
    if width > MAX_EDGE or height > MAX_EDGE:
        raise Failed(f"texture is {width}x{height}, larger than {MAX_EDGE}")
    name = TextureFormat(int(texture.m_TextureFormat)).name
    try:
        image = texture.image
    except NotImplementedError as exc:
        raise Failed(str(exc), "unsupported-format") from exc
    image.convert("RGBA").save(out_png, format="PNG")
    return {"width": image.width, "height": image.height, "format": name, "name": str(texture.m_Name)}


class Environments:
    """UnityPy resolves a sprite's atlas and texture through the files of one environment, so a reference cut
    loads every bundle the sprite touches into one; the last environment is kept for the next sprite."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.members: tuple[str, ...] = ()
        self.bundle: Bundle | None = None

    def get(self, members: tuple[str, ...]) -> Bundle:
        if self.bundle is None or members != self.members:
            self.bundle = None
            gc.collect()
            log(f"opening bundles {', '.join(members)}")
            self.bundle = Bundle([resolve_inside(self.root, member) for member in members])
            self.members = members
        return self.bundle


def export_sprite_image(
    bundles: Bundles, environments: Environments, export: dict[str, Any], out_png: Path
) -> dict[str, Any]:
    cab, path_id = ref_arg(export)
    bundle, owner, obj = bundles.object(cab, path_id)
    if obj.type.name != "Sprite":
        raise Failed(f"{cab}:{path_id} is a {obj.type.name}, not a Sprite")
    tree = obj.read_typetree()
    data, data_bundle, data_file, atlas_ref = render_data(bundles, tree, bundle, owner)
    cabs = {cab}
    texture = data_bundle.reference(data_file, data["texture"])
    for reference in (atlas_ref, texture):
        if reference is not None:
            cabs.add(reference["cab"])
    members = tuple(sorted({bundles.cab_index[name] for name in cabs if name in bundles.cab_index}))
    combined = environments.get(members)
    sprite = combined.files[cab].objects[path_id].parse_as_object()
    image = sprite.image.convert("RGBA")
    if image.width > MAX_EDGE or image.height > MAX_EDGE:
        raise Failed(f"sprite image is {image.width}x{image.height}, larger than {MAX_EDGE}")
    image.save(out_png, format="PNG")
    return {"width": image.width, "height": image.height}


# ------------------------------------------------------------------------------------------- the run


def _sprite_image_order(bundles: Bundles, export: dict[str, Any]) -> tuple[int, str, str]:
    """Reference cuts grouped by the bundle of their sprite, so a combined environment is reused."""
    if export["kind"] != "sprite-image":
        return (0, "", "")
    cab = str(export["ref"]["cab"]).lower()
    return (1, bundles.cab_index.get(cab, ""), export["id"])


def main(argv: list[str]) -> int:
    request = json.loads(Path(argv[0]).read_text(encoding="utf-8"))
    if request.get("schemaVersion") != 1 or request.get("decoder") != "unity":
        log("unsupported request")
        return 2
    root = Path(request["root"])
    exports: list[dict[str, Any]] = request["exports"]
    cab_index = (request.get("settings") or {}).get("cabIndex")
    (OUT / "json").mkdir(parents=True, exist_ok=True)
    (OUT / "png").mkdir(parents=True, exist_ok=True)
    results: dict[str, dict[str, Any]] = {}

    def ok(export_id: str, file: str, meta: dict[str, Any]) -> None:
        results[export_id] = {"id": export_id, "ok": True, "file": file, "meta": meta}

    def fail(export_id: str, failure: Failed) -> None:
        results[export_id] = {"id": export_id, "ok": False, "error": str(failure), "errorKind": failure.kind}

    scanned = [export for export in exports if export["kind"] in ("bundle-index", "monobehaviours")]
    if scanned:
        try:
            answers = scan(root, scanned)
        except Failed as failure:
            for export in scanned:
                fail(export["id"], failure)
        else:
            for export in scanned:
                if export["id"] in answers["failures"]:
                    fail(export["id"], Failed(answers["failures"][export["id"]]))
                    continue
                payload = answers["bundle-index"] if export["kind"] == "bundle-index" else answers[export["id"]]
                relative = f"json/{export['id']}.json"
                (OUT / relative).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
                ok(export["id"], relative, {})
    lookups = [export for export in exports if export["kind"] in ("sprite", "texture", "sprite-image")]
    if lookups:
        if cab_index is None:
            for export in lookups:
                fail(export["id"], Failed(f"a {export['kind']} export needs settings.cabIndex"))
        else:
            bundles = Bundles(root, cab_index)
            environments = Environments(root)
            for export in sorted(lookups, key=lambda item: _sprite_image_order(bundles, item)):
                kind = export["kind"]
                suffix = "json" if kind == "sprite" else "png"
                relative = f"{suffix}/{export['id']}.{suffix}"
                try:
                    if kind == "sprite-image":
                        meta = export_sprite_image(bundles, environments, export, OUT / relative)
                    elif kind == "sprite":
                        meta = export_sprite(bundles, export, OUT / relative)
                    else:
                        meta = export_texture(bundles, export, OUT / relative)
                    ok(export["id"], relative, meta)
                except Failed as failure:
                    fail(export["id"], failure)
                except (KeyError, IndexError, ValueError) as exc:
                    fail(export["id"], Failed(f"{type(exc).__name__}: {exc}"))
    manifest = {
        "schemaVersion": 1,
        "tool": {
            "name": "takaro-unity-export",
            "version": TOOL_VERSION,
            "unitypy": UnityPy.__version__,
            "pillow": PIL.__version__,
            "python": sys.version.split()[0],
        },
        "results": [results[export["id"]] for export in exports],
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    os.umask(0o022)
    raise SystemExit(main(sys.argv[1:]))
