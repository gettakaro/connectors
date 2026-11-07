#!/usr/bin/env python3
"""A docker CLI stand-in for the icon decoder containers.

``TAKARO_MAINT_ICONS_DECODER`` points the real code path at this file, which plays the three docker
commands a decoder build uses (``image inspect``, ``build`` and ``run``). A ``run`` does not decode game
files: it answers from recorded outputs, and it refuses (exit 99) any run that is not hardened the way
the contract demands, so a regression in the argument list fails a test instead of passing silently.

Environment it reads:
  FAKE_DECODER_ROOT   recorded outputs: ``index.json`` maps a key to ``{file, meta, error, errorKind, requires}``.
                      Unreal requests (``request["decoder"] == "unreal"``) use ``<kind>:<package>`` (``asset-registry``
                      for the registry); ``requires`` lists ``{file, offset, length, sha256}`` ranges the mounted game
                      files must hold. Unity requests use ``bundle-index``, ``monobehaviours:<class>`` and
                      ``<kind>:<cab>:<pathId>`` for ``sprite``, ``texture`` and ``sprite-image``; ``bundle-index`` and
                      ``monobehaviours`` refuse a ``files`` entry that is not a file under the /game mount, and
                      the other kinds refuse a request without ``settings.cabIndex``. XNB requests use
                      ``texture:<file>`` and CLR requests ``assembly-info:<file>``, ``callers:<member>`` and
                      ``decompile-type:<type>``; both refuse a ``file`` that is not a regular file under the /game
                      mount, which proves the client-file mirror is what is mounted
  FAKE_DECODER_STATE  directory with the image store (``images.json``), so ``build`` then ``image inspect``
                      round-trips and carries the label given to ``build``
  FAKE_DECODER_LOG    append one JSON line per invocation (its argv)
  FAKE_DECODER_EXIT   force a ``run`` to exit with this non-zero status
  FAKE_DECODER_FAIL   comma-separated export ids answered with ``errorKind: failed``
  FAKE_DECODER_DROP   an export id left out of the manifest
  FAKE_DECODER_ESCAPE an export id whose file is named ``../escape.png``
  FAKE_DECODER_PLANT  ``symlink`` or ``hardlink``: plant that kind of entry in /out, as a hostile decoder could
                      (a hardlink to any file of the /game mount)
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

HARDENING = (
    ("--network", "none"),
    ("--tmpfs", "/tmp"),
    ("--cap-drop", "ALL"),
    ("--security-opt", "no-new-privileges"),
)


def _state() -> Path:
    return Path(os.environ["FAKE_DECODER_STATE"])


def _images() -> dict[str, Any]:
    path = _state() / "images.json"
    return json.loads(path.read_text()) if path.is_file() else {}  # type: ignore[no-any-return]


def _save(images: dict[str, Any]) -> None:
    _state().mkdir(parents=True, exist_ok=True)
    (_state() / "images.json").write_text(json.dumps(images, indent=2))


def _refuse(message: str) -> int:
    print(f"fake decoder refuses this run: {message}", file=sys.stderr)
    return 99


def _inspect(tag: str) -> int:
    image = _images().get(tag)
    if image is None:
        print("Error: No such image: " + tag, file=sys.stderr)
        return 1
    print(json.dumps([{"Id": image["Id"], "Config": {"Labels": {image["labelKey"]: image["label"]}}}]))
    return 0


def _build(argv: list[str]) -> int:
    label_key, _, label = argv[argv.index("--label") + 1].partition("=")
    tag = argv[argv.index("-t") + 1]
    images = _images()
    builds = images.get("_builds", 0) + 1
    images["_builds"] = builds
    images[tag] = {
        "Id": "sha256:" + hashlib.sha256(f"{tag}|{label}|{builds}".encode()).hexdigest(),
        "label": label,
        "labelKey": label_key,
    }
    _save(images)
    return 0


def _flag_values(argv: list[str], flag: str) -> list[str]:
    return [argv[index + 1] for index, item in enumerate(argv) if item == flag and index + 1 < len(argv)]


def _hardened(argv: list[str]) -> str | None:
    for flag, value in HARDENING:
        if value not in _flag_values(argv, flag):
            return f"missing {flag} {value}"
    if "--read-only" not in argv:
        return "missing --read-only"
    for mount in _flag_values(argv, "-v"):
        if not mount.endswith(":/out") and not mount.endswith(":ro"):
            return f"mount {mount.rsplit(':', 1)[-1]!r} is writable: {mount}"
    for name in _flag_values(argv, "-e"):
        if "=" in name:
            return "a secret value is on the command line (-e NAME=VALUE)"
    ids = {image["Id"] for key, image in _images().items() if key != "_builds"}
    reference = argv[-2]
    if reference not in ids:
        return f"image reference {reference!r} is not a stored image Id"
    return None


def _host_of(argv: list[str], container: str) -> Path:
    for mount in _flag_values(argv, "-v"):
        parts = mount.split(":")
        if parts[1] == container:
            return Path(parts[0])
    raise SystemExit(_refuse(f"no mount for {container}"))


def _key(decoder: str, export: dict[str, Any]) -> str:
    kind = export["kind"]
    if decoder == "unity":
        if kind == "bundle-index":
            return kind
        if kind == "monobehaviours":
            return f"{kind}:{export['script']['class']}"
        return f"{kind}:{export['ref']['cab']}:{export['ref']['pathId']}"
    if decoder == "xnb":
        return f"{kind}:{export['file']}"
    if decoder == "clr":
        return f"{kind}:{export.get('member') or export.get('type') or export['file']}"
    return kind if kind == "asset-registry" else f"{kind}:{export['package']}"


def _tool(request: dict[str, Any]) -> dict[str, Any]:
    if request["decoder"] == "unity":
        return {"name": "fake-unity-export", "version": "1", "unitypy": "0.0-fake"}
    if request["decoder"] == "xnb":
        return {"name": "fake-xnb-export", "version": "1", "xnb": "0.0-fake"}
    if request["decoder"] == "clr":
        return {"name": "fake-clr-export", "version": "1", "decompiler": "0.0-fake"}
    return {
        "name": "fake-ue-export",
        "version": "1",
        "cue4parse": "0.0-fake",
        "engine": request["settings"]["engine"],
    }


def _unity_refusal(request: dict[str, Any], export: dict[str, Any], game: Path) -> str | None:
    if export["kind"] in ("bundle-index", "monobehaviours"):
        for name in export["files"]:
            if not (game / name).is_file():
                return f"{export['id']}: {name} is not a file under the /game mount"
    elif not (request.get("settings") or {}).get("cabIndex"):
        return f"{export['id']}: a {export['kind']} export needs settings.cabIndex"
    return None


def _file_refusal(export: dict[str, Any], game: Path) -> str | None:
    name = export["file"]
    path = game / name
    if path.is_symlink() or not path.is_file():
        return f"{export['id']}: {name} is not a regular file under the /game mount"
    return None


def _run(argv: list[str]) -> int:
    problem = _hardened(argv)
    if problem:
        return _refuse(problem)
    if os.environ.get("FAKE_DECODER_EXIT"):
        print("decoder crashed on purpose", file=sys.stderr)
        return int(os.environ["FAKE_DECODER_EXIT"])
    root = Path(os.environ["FAKE_DECODER_ROOT"])
    index = json.loads((root / "index.json").read_text())
    request = json.loads(_host_of(argv, "/work/request.json").read_text())
    out = _host_of(argv, "/out")
    game = _host_of(argv, "/game")
    failing = {part for part in os.environ.get("FAKE_DECODER_FAIL", "").split(",") if part}
    results = []
    for export in request["exports"]:
        export_id = export["id"]
        if export_id == os.environ.get("FAKE_DECODER_DROP"):
            continue
        if export_id in failing:
            results.append({"id": export_id, "ok": False, "errorKind": "failed", "error": "forced failure"})
            continue
        key = _key(request["decoder"], export)
        refusal = None
        if request["decoder"] == "unity":
            refusal = _unity_refusal(request, export, game)
        elif request["decoder"] in ("xnb", "clr"):
            refusal = _file_refusal(export, game)
        if refusal:
            return _refuse(refusal)
        entry = index.get(key)
        if entry is None:
            results.append({"id": export_id, "ok": False, "errorKind": "not-found", "error": key})
            continue
        if "error" in entry:
            results.append({"id": export_id, "ok": False, "errorKind": entry["errorKind"], "error": entry["error"]})
            continue
        for requirement in entry.get("requires", []):
            source = game / requirement["file"]
            with source.open("rb") as handle:
                handle.seek(requirement["offset"])
                data = handle.read(requirement["length"])
            if hashlib.sha256(data).hexdigest() != requirement["sha256"]:
                return _refuse(f"{key}: the mounted {requirement['file']} lacks the planned range {requirement}")
        folder = "png" if entry["file"].endswith(".png") else "json"
        relative = f"{folder}/{export_id}.{entry['file'].rsplit('.', 1)[1]}"
        (out / folder).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / entry["file"], out / relative)
        if export_id == os.environ.get("FAKE_DECODER_ESCAPE"):
            relative = "../escape.png"
        results.append({"id": export_id, "ok": True, "file": relative, "meta": entry.get("meta", {})})
    manifest = {"schemaVersion": 1, "tool": _tool(request), "results": results}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    plant = os.environ.get("FAKE_DECODER_PLANT")
    if plant:
        (out / "json").mkdir(exist_ok=True)
    if plant == "symlink":
        (out / "json" / "planted.json").symlink_to("/etc/hostname")
    elif plant == "hardlink":
        os.link(
            next(path for path in sorted(game.rglob("*")) if path.is_file() and not path.is_symlink()),
            out / "json" / "planted.json",
        )
    return 0


def main(argv: list[str]) -> int:
    log = os.environ.get("FAKE_DECODER_LOG")
    if log:
        with open(log, "a", encoding="utf-8") as handle:
            handle.write(json.dumps(argv) + "\n")
    if argv[:2] == ["image", "inspect"]:
        return _inspect(argv[2])
    if argv[:1] == ["build"]:
        return _build(argv)
    if argv[:1] == ["run"]:
        return _run(argv)
    print(f"fake decoder: unsupported command {argv[:2]}", file=sys.stderr)
    return 2


def argv_log(path: Path) -> list[list[str]]:
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
