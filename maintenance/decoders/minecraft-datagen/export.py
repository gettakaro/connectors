#!/usr/bin/env python3
"""takaro-minecraft-datagen: the data reports of a Minecraft server jar, as JSON, for the icon pipeline.

Runs inside the decoder container: ``python -I /app/export.py /work/request.json`` with the jar mounted
read-only at the request's ``root`` and a writable ``/out``. The generator is Mojang's own code, run with no
network, a read-only root file system and ``/tmp`` as its only scratch space.

Request::

    {"schemaVersion": 1, "decoder": "minecraft-datagen", "settings": {}, "root": "/game",
     "exports": [{"id", "kind": "datagen-report", "file": <jar below root>, "report": "<name>.json"}]}

The generator runs once per jar. Progress goes to stderr; the manifest carries no timings or memory figures.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from collections import defaultdict
from pathlib import Path, PurePosixPath
from typing import Any

OUT = Path("/out")
WORK = Path("/tmp/datagen")
JAVA_RELEASE = Path("/opt/java/openjdk/release")
TOOL_VERSION = "1"
TIMEOUT_SECONDS = 900
MAX_REPORT_BYTES = 256 * 1024 * 1024
STDERR_TAIL = 20
REPORT_NAME = re.compile(r"^[a-z0-9_]+\.json$")


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def check_export(root: Path, export: dict[str, Any]) -> Path:
    """The jar of ``export``; a path that is not a plain relative regular file inside ``root`` is refused."""
    if export.get("kind") != "datagen-report":
        raise ValueError(f"unsupported export kind {export.get('kind')!r}")
    if not REPORT_NAME.fullmatch(str(export.get("report"))):
        raise ValueError(f"refusing report name {export.get('report')!r}")
    relative = str(export["file"])
    pure = PurePosixPath(relative)
    if pure.is_absolute() or ".." in pure.parts or not pure.parts:
        raise ValueError(f"refusing file path {relative!r}")
    path = root / pure
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"{relative!r} resolves outside {root}")
    if not stat.S_ISREG(os.lstat(path).st_mode):
        raise ValueError(f"{relative!r} is not a regular file")
    return path


def java_description() -> str:
    fields: dict[str, str] = {}
    for line in JAVA_RELEASE.read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition("=")
        fields[key] = value.strip('"')
    return f"{fields.get('JAVA_VERSION', '?')} {fields.get('IMPLEMENTOR_VERSION', '')}".strip()


def run_generator(jar: Path) -> tuple[Path | None, str]:
    """Run the data generator once; ``(reports directory, "")`` or ``(None, the problem)``."""
    shutil.rmtree(WORK, ignore_errors=True)
    output, workdir = WORK / "out", WORK / "work"
    output.mkdir(parents=True)
    workdir.mkdir(parents=True)
    argv = [
        "java",
        "-Xmx2g",
        "-XX:+UseSerialGC",
        "-XX:-UsePerfData",
        "-Djava.awt.headless=true",
        "-DbundlerMainClass=net.minecraft.data.Main",
        "-jar",
        str(jar),
        "--reports",
        "--output",
        str(output),
    ]
    log("running the data generator: " + " ".join(argv))
    try:
        completed = subprocess.run(
            argv,
            cwd=workdir,
            env={"PATH": os.environ["PATH"], "JAVA_HOME": os.environ["JAVA_HOME"], "HOME": "/tmp"},
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return None, f"the data generator timed out after {TIMEOUT_SECONDS} s"
    if completed.returncode != 0:
        tail = "\n".join(completed.stderr.splitlines()[-STDERR_TAIL:])
        return None, f"the data generator exited {completed.returncode}: {tail}"
    return output / "reports", ""


def failed(export: dict[str, Any], message: str, kind: str = "failed") -> dict[str, Any]:
    return {"id": export["id"], "ok": False, "error": message, "errorKind": kind}


def main() -> int:
    request = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    root = Path(request["root"])
    results: dict[str, dict[str, Any]] = {}
    by_jar: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for export in request["exports"]:
        try:
            check_export(root, export)
        except (ValueError, OSError) as exc:
            results[export["id"]] = failed(export, str(exc))
        else:
            by_jar[str(export["file"])].append(export)
    (OUT / "json").mkdir(parents=True, exist_ok=True)
    for file, exports in by_jar.items():
        reports, problem = run_generator(root / file)
        for export in exports:
            if reports is None:
                results[export["id"]] = failed(export, problem)
                continue
            source = reports / export["report"]
            if not source.is_file():
                results[export["id"]] = failed(export, f"the generator wrote no {export['report']}", "not-found")
            elif source.stat().st_size > MAX_REPORT_BYTES:
                results[export["id"]] = failed(export, f"{export['report']} is larger than {MAX_REPORT_BYTES} bytes")
            else:
                relative = f"json/{export['id']}.json"
                shutil.copyfile(source, OUT / relative)
                payload = (OUT / relative).read_bytes()
                results[export["id"]] = {
                    "id": export["id"],
                    "ok": True,
                    "file": relative,
                    "meta": {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()},
                }
    manifest = {
        "schemaVersion": 1,
        "tool": {"name": "takaro-minecraft-datagen", "version": TOOL_VERSION, "java": java_description()},
        "results": [results[export["id"]] for export in request["exports"]],
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
