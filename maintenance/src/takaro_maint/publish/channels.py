"""The three release channels and the protocol each one follows.

``stable`` is a release-please draft that already has its tag: the publisher fills it, re-reads
it from GitHub and only then un-drafts it, so a half-uploaded set is never visible and a retry
of an interrupted run is a no-op rather than a clobber. ``rolling`` and ``pr`` have no such
draft to fill, so they build a complete staging draft first and swap it in at the end; until the
swap the previous build is still the one consumers see.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import net, output
from ..exit_codes import ConflictError, MaintError, TrackerError, VerificationFailed
from . import compat_record
from .release_client import ReleaseClient

ASSETS_BEGIN = "<!-- takaro-maint:assets:begin -->"
ASSETS_END = "<!-- takaro-maint:assets:end -->"

CHANNELS = ("stable", "rolling", "pr")


@dataclass(frozen=True)
class LocalAsset:
    """One file in the assembled directory, already hashed."""

    name: str
    path: Path
    sha256: str
    size: int


def local_assets(assembled: Path) -> list[LocalAsset]:
    found = []
    for path in sorted(assembled.iterdir(), key=lambda p: p.name):
        if not path.is_file():
            continue
        digests = net.hash_file(path)
        found.append(LocalAsset(path.name, path, digests["sha256"], int(digests["size"])))
    return found


def parse_checksums(payload: bytes) -> dict[str, str]:
    """GNU ``sha256sum`` output -> ``{name: sha256}``."""
    sums: dict[str, str] = {}
    for line in payload.decode("utf-8", "replace").splitlines():
        if not line.strip():
            continue
        digest, _, name = line.partition("  ")
        if digest and name:
            sums[name.strip()] = digest.strip()
    return sums


# -- titles and bodies -------------------------------------------------------------------


def display_name(record: dict[str, Any], game_record: dict[str, Any] | None) -> str:
    if game_record and game_record.get("name"):
        return str(game_record["name"])
    return str(record["connector"])


def default_title(channel: str, name: str, pr_number: str | None) -> str:
    if channel == "pr":
        return f"{name} Connector — build for PR #{pr_number}"
    return f"{name} Connector — latest dev build"


def default_notes(channel: str, version: str, pr_number: str | None) -> str:
    if channel == "pr":
        return (
            f"Disposable build (`{version}`) for PR #{pr_number}. Replaced on every push, "
            "deleted when the PR closes. Not for production."
        )
    return f"Rolling development build (`{version}`), updated on every push to main. Not for production."


def render_table(record: dict[str, Any]) -> str:
    """The per-target table that goes into the release body and the sticky PR comment."""
    repo = str(record["source"]["repo"])
    tag = str(record["tag"])
    lines: list[str] = []
    if record["mode"] == "catalog" and record["targets"]:
        # Admins read this as "my server build -> this file", so the build leads (#756).
        lines.append("Download the file for your server build:")
        lines.append("")
        lines.append("| your server build | download | sha256 | target | verified |")
        lines.append("|---|---|---|---|---|")
        for target_id, entry in sorted(record["targets"].items()):
            verified = _verified_cell(entry["verification"])
            for artifact in entry["artifacts"]:
                lines.append(
                    f"| {entry['platform']} {entry['revision']} | "
                    f"[{artifact['name']}]({artifact['url']}) | `{artifact['sha256'][:12]}` | "
                    f"{target_id} | {verified} |"
                )
        for name, alias in sorted(record["aliases"].items()):
            url = compat_record.download_url(repo, tag, name)
            target = record["targets"].get(alias["target"]) or {}
            build = f"{target.get('platform', '')} {target.get('revision', '')}".strip() or alias["target"]
            lines.append(
                f"| {build} (old name) | [{name}]({url}) | "
                f"`{alias['sha256'][:12]}` | {alias['target']} | copy of {alias['of']} |"
            )
    else:
        for asset in sorted(record["assets"], key=lambda a: str(a["name"])):
            lines.append(f"- [`{asset['name']}`]({asset['url']}) — `{asset['sha256'][:12]}`")

    extras = [
        f"[SHA256SUMS]({compat_record.download_url(repo, tag, 'SHA256SUMS')})",
        f"[compat record]({compat_record.download_url(repo, tag, str(record['self']))})",
    ]
    reports = [a for a in record["assets"] if a["kind"] == "verify-report"]
    if reports:
        joined = ", ".join(
            f"[{a['name'].rsplit('.verify-', 1)[-1].removesuffix('.json')}]({a['url']})" for a in reports
        )
        extras.append(f"verify reports: {joined}")
    lines.append("")
    lines.append("Also: " + " · ".join(extras))
    return "\n".join(lines)


def _verified_cell(verification: dict[str, Any]) -> str:
    executed = verification.get("executed")
    if not executed:
        return f"not required (`{verification['required']}`)"
    return f"{executed} ({verification.get('takaro') or 'local'})"


def merge_body(existing: str, block: str) -> str:
    """Put ``block`` between the markers, leaving release-please's own notes alone."""
    wrapped = f"{ASSETS_BEGIN}\n{block}\n{ASSETS_END}"
    if ASSETS_BEGIN in existing and ASSETS_END in existing:
        head, _, rest = existing.partition(ASSETS_BEGIN)
        _, _, tail = rest.partition(ASSETS_END)
        return head + wrapped + tail
    # The download table goes first: which file fits which server build matters more to an
    # admin than the changelog below it (gettakaro/connectors#756).
    suffix = "\n\n" + existing.strip() + "\n" if existing.strip() else "\n"
    return wrapped + suffix


# -- verification from GitHub ------------------------------------------------------------


def verify_release(
    client: ReleaseClient,
    release: dict[str, Any],
    *,
    connector: str | None = None,
    expect: dict[str, Path] | None = None,
    tag: str | None = None,
) -> dict[str, Any]:
    """Download every asset of ``release`` and prove it is the set the record describes.

    ``tag`` is the tag the record has to claim, which is the release's own tag everywhere but a
    staging draft, where the record already names the tag the draft is about to become.
    """
    remote = {str(asset["name"]): asset for asset in client.assets(int(release["id"]))}
    if "SHA256SUMS" not in remote:
        raise VerificationFailed(f"{release['tag_name']} has no SHA256SUMS", tag=release["tag_name"])
    sums_bytes = client.download(remote["SHA256SUMS"])
    sums = parse_checksums(sums_bytes)

    prefix = f"takaro-{connector}-" if connector else "takaro-"
    candidates = sorted(n for n in remote if n.startswith(prefix) and n.endswith(".compat.json"))
    if len(candidates) != 1:
        raise VerificationFailed(
            f"{release['tag_name']} carries {len(candidates)} compatibility records, expected exactly one",
            tag=release["tag_name"],
            found=candidates,
        )
    record_bytes = client.download(remote[candidates[0]])
    record = compat_record.loads(candidates[0], record_bytes)

    # Hashes prove the bytes are intact; they say nothing about whether this set belongs here.
    # A record copied to another tag, or assembled for another repository, is internally
    # consistent and still describes assets that live somewhere else.
    expected_tag = tag if tag is not None else str(release["tag_name"])
    if str(record["tag"]) != expected_tag:
        raise ConflictError(
            f"the compatibility record on {release['tag_name']} is for tag '{record['tag']}', not '{expected_tag}'",
            tag=expected_tag,
            recordTag=str(record["tag"]),
        )
    if str(record["source"]["repo"]) != client.repo:
        raise ConflictError(
            f"the compatibility record on {release['tag_name']} was assembled for "
            f"{record['source']['repo']}, this is {client.repo}",
            tag=expected_tag,
            recordRepo=str(record["source"]["repo"]),
        )

    required = {str(asset["name"]) for asset in record["assets"]} | {"SHA256SUMS", str(record["self"])}
    missing = sorted(required - set(remote))
    if missing:
        raise VerificationFailed(
            f"{release['tag_name']} is missing " + ", ".join(missing), tag=release["tag_name"], missing=missing
        )

    by_name = {str(asset["name"]): asset for asset in record["assets"]}
    known = {"SHA256SUMS": sums_bytes, str(record["self"]): record_bytes}
    checked: list[dict[str, Any]] = []
    for name in sorted(required):
        payload = known.get(name) or client.download(remote[name])
        digest = hashlib.sha256(payload).hexdigest()
        if name != "SHA256SUMS" and sums.get(name) != digest:
            raise ConflictError(
                f"{name} on {release['tag_name']} hashes to {digest}, SHA256SUMS says {sums.get(name)}", asset=name
            )
        entry = by_name.get(name)
        if entry is not None and entry["sha256"] != digest:
            raise ConflictError(
                f"{name} on {release['tag_name']} hashes to {digest}, the compat record says {entry['sha256']}",
                asset=name,
            )
        if expect is not None:
            local = expect.get(name)
            if local is None or local.read_bytes() != payload:
                raise ConflictError(
                    f"{name} on {release['tag_name']} is not byte-identical to the assembled file", asset=name
                )
        checked.append({"name": name, "sha256": digest, "size": len(payload), "action": "verified"})

    return {
        "tag": str(release["tag_name"]),
        "releaseId": int(release["id"]),
        "htmlUrl": release.get("html_url"),
        "draft": bool(release.get("draft")),
        "prerelease": bool(release.get("prerelease")),
        "targetCommitish": release.get("target_commitish"),
        "connector": str(record["connector"]),
        "version": str(record["version"]),
        "assets": checked,
        "unexpectedAssets": sorted(set(remote) - required),
        "record": record,
    }


# -- stable ------------------------------------------------------------------------------


def publish_stable(
    client: ReleaseClient,
    *,
    tag: str,
    assets: list[LocalAsset],
    record: dict[str, Any],
    target_commit: str,
    finalize: bool,
) -> dict[str, Any]:
    """Fill release-please's draft, re-read it from GitHub, then un-draft it."""
    release = client.find_release(tag)
    if release is None:
        raise ConflictError(f"no release for {tag} — release-please creates it when the Release PR merges", tag=tag)
    tag_sha = client.tag_commit(tag)
    if not tag_sha:
        raise ConflictError(f"tag {tag} does not exist: release-please must run with force-tag-creation", tag=tag)
    if tag_sha != target_commit:
        raise ConflictError(
            f"tag {tag} points at {tag_sha}, this checkout is {target_commit} — recovery must check out the tag",
            tag=tag,
            tagCommit=tag_sha,
            targetCommit=target_commit,
        )

    release_id = int(release["id"])
    remote = {str(asset["name"]): asset for asset in client.assets(release_id)}
    ordered = sorted(assets, key=lambda a: a.name)

    # Every remote asset is compared before the first upload. A conflict found half-way through
    # the list would otherwise leave the assets uploaded before it beside the old ones — a new
    # SHA256SUMS next to an old archive — on a release that may already be public.
    planned: list[tuple[LocalAsset, str]] = []
    for asset in ordered:
        found = remote.get(asset.name)
        if found is None:
            planned.append((asset, "upload"))
        elif _remote_matches(client, found, asset):
            planned.append((asset, "skipped-identical"))
        else:
            planned.append((asset, "conflict"))
    conflicts = [asset.name for asset, state in planned if state == "conflict"]
    if conflicts:
        draft = bool(release.get("draft"))
        hint = " The release is still a draft: delete its assets and re-run." if draft else ""
        raise ConflictError(
            f"conflicting bytes for {', '.join(conflicts)} on {tag}: the release already has a different file "
            f"under that name; nothing was uploaded.{hint}",
            tag=tag,
            asset=conflicts[0],
            conflicts=conflicts,
            draft=draft,
            assets=[_action(asset, state if state == "conflict" else "not-attempted") for asset, state in planned],
        )

    actions: list[dict[str, Any]] = []
    for asset, state in planned:
        if state == "upload":
            client.upload(str(release["upload_url"]), asset.path)
            actions.append(_action(asset, "uploaded"))
        else:
            actions.append(_action(asset, "skipped-identical"))

    body = merge_body(str(release.get("body") or ""), render_table(record))
    if body != str(release.get("body") or ""):
        release = client.update(release_id, tag_name=tag, body=body)

    verification = verify_release(
        client, release, connector=str(record["connector"]), expect={a.name: a.path for a in assets}, tag=tag
    )

    finalized = False
    already_published = not bool(release.get("draft"))
    if finalize and not already_published:
        release = client.update(release_id, tag_name=tag, draft=False)
        finalized = True

    return {
        "releaseId": release_id,
        "htmlUrl": release.get("html_url"),
        "draft": bool(release.get("draft")),
        "prerelease": bool(release.get("prerelease")),
        "assets": actions,
        "unexpectedAssets": verification["unexpectedAssets"],
        "verified": True,
        "finalized": finalized,
        "alreadyPublished": already_published,
        "staging": None,
        "replaced": None,
    }


def _action(asset: LocalAsset, action: str) -> dict[str, Any]:
    return {"name": asset.name, "action": action, "sha256": asset.sha256, "size": asset.size}


def _remote_matches(client: ReleaseClient, remote: dict[str, Any], local: LocalAsset) -> bool:
    """The cheap check first: GitHub records a ``digest`` for assets it stored recently."""
    digest = remote.get("digest")
    if isinstance(digest, str) and digest.startswith("sha256:"):
        return digest.split(":", 1)[1] == local.sha256
    return hashlib.sha256(client.download(remote)).hexdigest() == local.sha256


# -- rolling and pr ----------------------------------------------------------------------


def publish_staged(
    client: ReleaseClient,
    *,
    channel: str,
    tag: str,
    run_id: str,
    assets: list[LocalAsset],
    record: dict[str, Any],
    target_commit: str,
    title: str,
    notes: str,
) -> dict[str, Any]:
    """Build the whole replacement as a draft, verify it, and only then swap it in."""
    staging_tag = f"{tag}.staging-{run_id}"
    _drop_stale_staging(client, tag=tag, keep=staging_tag)

    body = merge_body(notes, render_table(record))
    staging = client.create_draft(
        tag=staging_tag, target_commitish=target_commit, name=title, body=body, prerelease=True
    )
    staging_id = int(staging["id"])
    actions: list[dict[str, Any]] = []
    try:
        for asset in sorted(assets, key=lambda a: a.name):
            client.upload(str(staging["upload_url"]), asset.path)
            actions.append(_action(asset, "uploaded"))
        verification = verify_release(
            client, staging, connector=str(record["connector"]), expect={a.name: a.path for a in assets}, tag=tag
        )
    except MaintError as exc:
        left = _drop_staging(client, staging_id, staging_tag)
        if left is not None:
            exc.detail["stagingLeft"] = left
        raise

    # From here the old release is being taken apart, so every step runs under one handler: a
    # failure anywhere in the swap has to name the staging draft that holds the complete set,
    # otherwise the operator is told only that a request failed and not that the replacement is
    # already built and one rerun away.
    old = client.find_release(tag)
    old_id = int(old["id"]) if old else None
    removed = False
    tag_existed = False
    try:
        if old is not None:
            client.delete_release(old_id)  # type: ignore[arg-type]
            removed = True
        tag_existed = client.tag_commit(tag) is not None
        if tag_existed:
            client.delete_tag(tag)
        published = client.update(
            staging_id, tag_name=tag, draft=False, prerelease=True, target_commitish=target_commit
        )
    except MaintError as exc:
        state = "old release removed" if removed else "old release still in place"
        raise TrackerError(
            f"{state}; the complete new set is draft {staging_tag} (id {staging_id}); "
            f"rerun to publish it ({exc.message})",
            staging={"tag": staging_tag, "releaseId": staging_id},
            replaced={"releaseId": old_id, "removed": removed},
        ) from exc

    return {
        "releaseId": staging_id,
        "htmlUrl": published.get("html_url"),
        "draft": bool(published.get("draft")),
        "prerelease": bool(published.get("prerelease")),
        "assets": actions,
        "unexpectedAssets": verification["unexpectedAssets"],
        "verified": True,
        "finalized": True,
        "alreadyPublished": False,
        "staging": {"tag": staging_tag, "releaseId": staging_id},
        "replaced": {"releaseId": old_id, "tagMoved": tag_existed},
    }


def _drop_stale_staging(client: ReleaseClient, *, tag: str, keep: str) -> None:
    """Remove staging drafts left by a cancelled or dead run of the same channel.

    Only drafts, only this tag's ``.staging-`` prefix: another connector's releases, another
    PR's builds and every published release are out of reach of this loop by construction.
    """
    for release in client.list_releases():
        name = str(release.get("tag_name") or "")
        if release.get("draft") and name.startswith(f"{tag}.staging-") and name != keep:
            output.info(f"removing stale staging draft {name}")
            client.delete_release(int(release["id"]))


def _drop_staging(client: ReleaseClient, release_id: int, tag: str) -> dict[str, Any] | None:
    try:
        client.delete_release(release_id)
    except MaintError:
        return {"tag": tag, "releaseId": release_id}
    return None
