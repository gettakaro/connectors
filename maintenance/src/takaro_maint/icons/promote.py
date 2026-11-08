"""Promote a game's pack from staging to production: rolling merge, missing objects first, index last.

Staging is only ever read. Every staging object that production lacks is fetched and checked before
the first production write, so a damaged staging pack leaves production untouched.
"""

from __future__ import annotations

from typing import Any

from .. import output
from ..exit_codes import ConflictError, IntegrityError, UpstreamUnavailable, UsageError
from . import contract, indexio
from .bucket import Bucket
from .publish import merge


def promote_pack(game: str, *, dry_run: bool) -> dict[str, Any]:
    contract.validate_game(game)
    source = Bucket.for_env("staging")
    destination = Bucket.for_env("production")
    if source.identity == destination.identity:
        raise UsageError("staging and production resolve to the same bucket")

    staged = indexio.read_live(source, game, "the staging index")
    if staged is None:
        raise UpstreamUnavailable(f"no staging pack for {game}; publish it to staging first")
    staged_index = staged[1]
    live = indexio.read_live(destination, game, "the production index")
    live_raw, live_index = live if live else (None, None)

    merged, diff = merge(live_index, staged_index)
    merged_raw = contract.serialize_index(game, merged.enabled, merged.icons)
    indexio.parse_raw(merged_raw, game, "the merged index")
    if live_index and not live_index.enabled:
        output.warn(f"the production {game} index is disabled; it stays disabled (icons set-enabled flips it)")
    if live_index is None and not staged_index.enabled:
        output.warn(f"the staging {game} index is disabled; production will be created disabled")

    if diff["kept"]:
        listed = ", ".join(diff["kept"][:10])
        more = f" and {len(diff['kept']) - 10} more" if len(diff["kept"]) > 10 else ""
        output.warn(
            f"production keeps {len(diff['kept'])} {game} codes that staging lacks ({listed}{more}); "
            "promote never removes codes, so a staging takedown needs `icons takedown --env production`"
        )

    needed = sorted(set(staged_index.icons.values()))
    present = [name for name in needed if destination.exists(f"{game}/{name}")]
    to_copy = [name for name in needed if name not in present]
    index_action = "unchanged" if merged_raw == live_raw else ("would-upload" if dry_run else "uploaded")
    report: dict[str, Any] = {
        "from": "staging",
        "to": "production",
        "game": game,
        "dryRun": dry_run,
        "objects": {},
        "index": {
            "action": index_action,
            "codes": len(merged.icons),
            "enabled": merged.enabled,
            **diff,
        },
    }
    skipped = [f"{game}/{name}" for name in present]

    if dry_run:
        missing = [name for name in to_copy if not source.exists(f"{game}/{name}")]
        if missing:
            raise ConflictError("the staging index references objects missing from staging", problems=missing)
        report["objects"] = {"toCopy": [f"{game}/{name}" for name in to_copy], "skipped": skipped}
        return report

    bodies = {name: _fetch_staged(source, game, name) for name in to_copy}
    for name, body in bodies.items():
        output.info(f"copy {game}/{name}")
        destination.put(f"{game}/{name}", body, contract.CONTENT_TYPES["webp"], contract.CACHE_CONTROL["webp"])
    if index_action == "uploaded":
        output.info(f"upload {indexio.index_key(game)}")
        indexio.write_if_changed(destination, game, merged_raw, live_raw)
    report["objects"] = {"copied": [f"{game}/{name}" for name in to_copy], "skipped": skipped}
    return report


def _fetch_staged(source: Bucket, game: str, name: str) -> bytes:
    body = source.get(f"{game}/{name}")
    if body is None:
        raise ConflictError("the staging index references an object missing from staging", problems=[name])
    if contract.object_name(body) != name:
        raise IntegrityError(f"{game}/{name} in staging does not hash to its name")
    problems = contract.check_object(name, body)
    if problems:
        raise ConflictError("a staging object violates the pack contract", problems=problems)
    return body
