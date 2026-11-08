"""Pack controls on one environment: flip the ``enabled`` flag, or take codes and objects down."""

from __future__ import annotations

from typing import Any

from .. import output
from ..exit_codes import IntegrityError, PublishFailed, UpstreamUnavailable, UsageError
from . import contract, indexio
from .bucket import Bucket

ENVIRONMENTS = ("staging", "production")


def _open(env: str, game: str) -> tuple[Bucket, bytes, contract.Index]:
    if env not in ENVIRONMENTS:
        raise UsageError(f"--env must be exactly one of {list(ENVIRONMENTS)}, got {env!r}")
    contract.validate_game(game)
    bucket = Bucket.for_env(env)
    live = indexio.read_live(bucket, game, f"the {env} index")
    if live is None:
        raise UpstreamUnavailable(f"no pack for {game} in {env}")
    return bucket, live[0], live[1]


def set_enabled(env: str, game: str, enabled: str, *, dry_run: bool) -> dict[str, Any]:
    if enabled not in ("true", "false"):
        raise UsageError(f"--enabled must be exactly 'true' or 'false', got {enabled!r}")
    requested = enabled == "true"
    bucket, live_raw, live = _open(env, game)
    raw = contract.serialize_index(game, requested, live.icons)
    action = _index_action(raw, live_raw, dry_run)
    if action == "uploaded":
        indexio.write_if_changed(bucket, game, raw, live_raw)
    return {
        "env": env,
        "game": game,
        "dryRun": dry_run,
        "enabled": {"from": live.enabled, "to": requested},
        "index": {"action": action, "codes": len(live.icons)},
    }


def takedown(env: str, game: str, codes: list[str], objects: list[str], *, dry_run: bool) -> dict[str, Any]:
    if bool(codes) == bool(objects):
        raise UsageError("name what to take down with --code or --object, exactly one of them")
    codes, objects = sorted(set(codes)), sorted(set(objects))
    malformed = [name for name in objects if not contract.OBJECT_PATTERN.fullmatch(name)]
    if malformed:
        raise UsageError("--object takes a bare <sha256>.webp name as it appears in the index", malformed=malformed)
    bucket, live_raw, live = _open(env, game)

    if codes:
        unknown = [code for code in codes if code not in live.icons]
        if unknown:
            raise UsageError("these codes are not in the index", unknown=unknown)
        removed = codes
        candidates = sorted({live.icons[code] for code in codes})
    else:
        referenced = set(live.icons.values())
        unknown = [name for name in objects if name not in referenced and not bucket.exists(f"{game}/{name}")]
        if unknown:
            raise UsageError("these objects are neither in the index nor in the bucket", unknown=unknown)
        removed = sorted(code for code, name in live.icons.items() if name in objects)
        candidates = objects

    remaining = {code: name for code, name in live.icons.items() if code not in removed}
    retained = [
        {"object": name, "codes": sorted(code for code, value in remaining.items() if value == name)}
        for name in candidates
        if name in remaining.values()
    ]
    kept = {entry["object"] for entry in retained}
    to_delete = [name for name in candidates if name not in kept]
    raw = contract.serialize_index(game, live.enabled, remaining)
    indexio.parse_raw(raw, game, "the new index")
    action = _index_action(raw, live_raw, dry_run)
    report: dict[str, Any] = {
        "env": env,
        "game": game,
        "dryRun": dry_run,
        "removedCodes": removed,
        "objects": {
            ("toDelete" if dry_run else "deleted"): [f"{game}/{name}" for name in to_delete],
            "retained": retained,
        },
        "index": {"action": action, "codes": len(remaining), "enabled": live.enabled},
    }
    if dry_run:
        return report

    if action == "uploaded":
        output.info(f"upload {indexio.index_key(game)}")
        indexio.write_if_changed(bucket, game, raw, live_raw)
    _delete_objects(bucket, game, to_delete)
    if to_delete:
        output.warn(
            "deleted objects were served with 'immutable, max-age=31536000' and may stay in the Cloudflare edge "
            "cache until purged; purge: " + ", ".join(f"{game}/{name}" for name in to_delete)
        )
    if removed:
        output.warn(
            f"publishing or promoting a {game} pack that still holds these codes re-adds them to {env}; "
            f"apply this takedown again afterwards: {', '.join(removed)}"
        )
    return report


def _index_action(raw: bytes, live_raw: bytes, dry_run: bool) -> str:
    if raw == live_raw:
        return "unchanged"
    return "would-upload" if dry_run else "uploaded"


def _delete_objects(bucket: Bucket, game: str, names: list[str]) -> None:
    for position, name in enumerate(names):
        key = f"{game}/{name}"
        output.info(f"delete {key}")
        try:
            bucket.delete(key)
            still_there = bucket.exists(key)
        except PublishFailed as exc:
            pending = names[position:]
            raise PublishFailed(
                f"{exc.message}; the index is already republished, so rerun "
                f"`icons takedown --object {' '.join(pending)}` to finish",
                **{**exc.detail, "pendingObjects": pending},
            ) from None
        if still_there:
            raise IntegrityError(f"{key} is still present after delete", pendingObjects=names[position:])
