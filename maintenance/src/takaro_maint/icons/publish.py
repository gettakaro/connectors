"""Publish a pack to the icon bucket: a rolling merge with the live index, objects first, index last.

Everything up to resolving credentials is local, so a refused pack or environment never reaches the
network. ``merge`` is pure so that a later promotion step can reuse it.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

from .. import output
from ..exit_codes import ConflictError, IntegrityError, UsageError
from . import contract
from .bucket import Bucket

PUBLISH_ENV = "staging"


def merge(live: contract.Index | None, new: contract.Index) -> tuple[contract.Index, dict[str, Any]]:
    """Every earlier code survives; a changed code points at its new object; a live takedown is kept."""
    live_icons = live.icons if live else {}
    diff = {
        "added": sorted(code for code in new.icons if code not in live_icons),
        "repointed": [
            {"code": code, "from": live_icons[code], "to": name}
            for code, name in sorted(new.icons.items())
            if code in live_icons and live_icons[code] != name
        ],
        "unchanged": sum(1 for code, name in new.icons.items() if live_icons.get(code) == name),
        "kept": sorted(code for code in live_icons if code not in new.icons),
    }
    enabled = live.enabled if live else new.enabled
    return contract.Index(game=new.game, enabled=enabled, icons={**live_icons, **new.icons}), diff


def publish_pack(env: str, pack: Path, *, dry_run: bool) -> dict[str, Any]:
    if env != PUBLISH_ENV:
        raise UsageError("icons publish writes staging only; production is written by icons promote")
    pack = pack.resolve()
    if f"{pack.name}/".startswith(contract.STORE_PREFIX):
        contract.guard_keys([f"{pack.name}/{contract.INDEX_NAME}"])
    problems = contract.check_pack_dir(pack)
    if problems:
        raise ConflictError("the pack violates the pack contract", problems=problems)
    game = pack.name
    files = sorted(entry.name for entry in pack.iterdir() if entry.name != contract.INDEX_NAME)
    index_key = f"{game}/{contract.INDEX_NAME}"
    contract.guard_keys([index_key, *(f"{game}/{name}" for name in files)])
    local_index = _parse_raw((pack / contract.INDEX_NAME).read_bytes(), game, "the local pack index")

    bucket = Bucket.for_env(env)
    live_raw = bucket.get(index_key)
    live = _parse_raw(live_raw, game, "the live index") if live_raw is not None else None
    merged, diff = merge(live, local_index)
    merged_raw = contract.serialize_index(game, merged.enabled, merged.icons)
    _parse_raw(merged_raw, game, "the merged index")
    if live and not live.enabled:
        output.warn(f"the live {game} index is disabled; the pack stays disabled (icons set-enabled flips it)")

    present = [name for name in files if bucket.exists(f"{game}/{name}")]
    missing = [name for name in files if name not in present]
    index_action = "unchanged" if merged_raw == live_raw else ("would-upload" if dry_run else "uploaded")
    report: dict[str, Any] = {
        "env": env,
        "game": game,
        "dryRun": dry_run,
        "objects": {("wouldUpload" if dry_run else "uploaded"): [f"{game}/{n}" for n in missing]},
        "index": {
            "action": index_action,
            "codes": len(merged.icons),
            "enabled": merged.enabled,
            **diff,
        },
    }
    report["objects"]["skipped"] = [f"{game}/{n}" for n in present]
    if dry_run:
        return report

    for name in missing:
        output.info(f"upload {game}/{name}")
        bucket.put(
            f"{game}/{name}",
            (pack / name).read_bytes(),
            contract.CONTENT_TYPES["webp"],
            contract.CACHE_CONTROL["webp"],
        )
    if index_action == "uploaded":
        output.info(f"upload {index_key}")
        bucket.put(index_key, merged_raw, contract.CONTENT_TYPES["index"], contract.CACHE_CONTROL["index"])
        read_back = bucket.get(index_key)
        if read_back is None or hashlib.sha256(read_back).digest() != hashlib.sha256(merged_raw).digest():
            raise IntegrityError(f"{index_key} read back after upload differs from what was sent")
    return report


def _parse_raw(raw: bytes, game: str, what: str) -> contract.Index:
    parsed = contract.parse_index(raw, game)
    if isinstance(parsed, list):
        raise ConflictError(f"{what} violates the pack contract", problems=parsed)
    return parsed
