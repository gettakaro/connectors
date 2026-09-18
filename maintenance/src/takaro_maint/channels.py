"""Channels: which upstream labels are watched, and what a head moving means.

Two jobs, both deliberately free of version arithmetic:

*Grammar.* Mojang's preview ids are ``<base>-(snapshot|pre|rc)-N`` (and the legacy
``<base>-rcN``), so a release ``26.4`` can be recognised as the promotion of the preview
``26.4-rc-1``. That is the **only** relation between two revisions this codebase claims.
Nothing here ever decides that one version is "newer" than another: upstream publish order
and checkpoint membership are the only orderings, because every framework watched here
numbers its releases differently and at least one of them (NeoForge) points its Maven
``<release>`` marker at a beta.

*Head movement.* A framework's head for one ``(game version, branch)`` can move back to a
revision that was already seen — a build is withdrawn, a Maven artifact is deleted. That is
a rollback, and it is detected by asking two questions of the checkpoint: is the current
head already in ``seen``, and is some other seen revision of the same key more recently
first-seen than it is? Both are set operations. See ``maintenance/docs/discovery.md``.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from typing import Any

#: Mojang's preview id grammar. ``26.3-rc-3``, ``26.3-pre-1``, ``26.3-snapshot-10`` and the
#: legacy dashless ``1.21.5-rc1`` all name the release they lead up to; ``25w40a`` does not.
PREVIEW_RE = re.compile(r"^(?P<base>[0-9][0-9A-Za-z.]*)-(?:(?:snapshot|pre|rc)-(\d+)|(?:pre|rc)(\d+))$")

#: Separator of a rollback revision: ``<head>/rollback/<previous head>``. ``/`` is a legal
#: revision character (``observation.schema.json``) and appears in no upstream version.
ROLLBACK = "/rollback/"

#: A branch label is a small token; anything else is reported as ``unknown`` rather than
#: guessed at, because a branch name ends up in an issue marker and is an identity forever.
_BRANCH_RE = re.compile(r"^[A-Za-z0-9._+-]+$")

UNKNOWN_BRANCH = "unknown"


def preview_base(rev: str) -> str | None:
    """The release ``rev`` is a preview of, or ``None`` when it is not a preview id.

    >>> preview_base("26.3-rc-3"), preview_base("26.3"), preview_base("25w40a")
    ('26.3', None, None)
    """
    match = PREVIEW_RE.match(rev or "")
    return match.group("base") if match else None


def is_promotion_of(preview_rev: str, release_rev: str) -> bool:
    """True when ``release_rev`` is the release ``preview_rev`` was leading up to."""
    return preview_base(preview_rev) == release_rev


def enabled_channels(watch: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """The declared channels a provider should actually observe.

    The skip rules are the ones ``providers.mojang._channels`` already applies: a channel
    that is not a mapping, or carries ``"enabled": false``, is dropped. A dropped channel is
    not "unknown upstream" — its revisions are known and deliberately not watched, so they
    become neither observations nor branch-review candidates.
    """
    channels: dict[str, dict[str, Any]] = {}
    for key, channel in (watch.get("channels") or {}).items():
        if not isinstance(channel, dict) or not channel.get("enabled", True):
            continue
        channels[str(key)] = {**channel, "branch": str(channel.get("branch") or key)}
    return channels


def channel_label(channel: dict[str, Any], key: str) -> str:
    """The raw upstream label a declared channel claims.

    Each framework labels its releases in its own vocabulary, and the catalog declares the
    label verbatim so this code never has to translate: Paper names a ``channel`` enum
    (``STABLE``), NeoForge a version ``suffix`` (``-beta``, or ``""`` for a release), Fabric
    a ``stable`` boolean (rendered ``stable:true``). A channel that declares none of them
    is labelled by its own key.
    """
    if "channel" in channel:
        return str(channel["channel"])
    if "suffix" in channel:
        return str(channel["suffix"])
    if "stable" in channel:
        return f"stable:{str(bool(channel['stable'])).lower()}"
    return key


def declared_labels(watch: dict[str, Any]) -> dict[str, str]:
    """Every declared upstream label mapped to its branch, **enabled or not**.

    Membership here is what separates "a channel we chose not to watch" from "a branch
    nobody has decided about yet": only a label missing from this mapping is a
    branch-review candidate.
    """
    labels: dict[str, str] = {}
    for key, channel in (watch.get("channels") or {}).items():
        if not isinstance(channel, dict):
            continue
        labels[channel_label(channel, str(key))] = str(channel.get("branch") or key)
    return labels


def branch_name(label: str) -> str:
    """A branch name for an undeclared upstream label, or ``unknown`` when it is unusable."""
    candidate = str(label or "").strip().lower().lstrip("-")
    return candidate if candidate and _BRANCH_RE.match(candidate) else UNKNOWN_BRANCH


def _seen(checkpoint_json: dict[str, Any] | None) -> list[tuple[str, str]]:
    entries: list[tuple[str, str]] = []
    for item in (checkpoint_json or {}).get("seen") or []:
        if isinstance(item, list | tuple) and len(item) == 2:
            entries.append((str(item[0]), str(item[1])))
        else:
            entries.append((str(item), ""))
    return entries


def first_seen(checkpoint_json: dict[str, Any] | None, rev: str, now: str) -> str:
    """When ``rev`` was first recorded, or ``now`` for a revision never seen before.

    Fabric and NeoForge publish no per-version timestamp, so this is the ``releaseTime``
    every one of their observations carries: stable across runs (it comes back out of the
    checkpoint), and therefore usable as the checkpoint's floor.
    """
    for seen_rev, release_time in _seen(checkpoint_json):
        if seen_rev == rev and release_time:
            return release_time
    return now


def rollback_rev(head_rev: str, previous: str) -> str:
    """The revision a rollback is filed under: new to the checkpoint exactly once."""
    return f"{head_rev}{ROLLBACK}{previous}"


def split_rollback_rev(rev: str) -> tuple[str, str | None]:
    """``(head, previous)`` — ``previous`` is ``None`` for an ordinary revision."""
    head, marker, previous = (rev or "").partition(ROLLBACK)
    return (head, previous) if marker else (rev, None)


def head_event(
    checkpoint_json: dict[str, Any] | None,
    key: tuple[str, str],
    head_rev: str,
    revs_of_key: Callable[[str], bool],
) -> str | None:
    """The head this key has rolled back *from*, or ``None`` when nothing moved backwards.

    ``key`` is ``(game version, branch)`` and exists only for the caller's benefit; the
    decision is made from the checkpoint alone:

    1. the current head must already be in ``seen`` — a head nobody has seen is simply new;
    2. some other head of the same key must have been first seen *strictly later* than it.

    Heads are compared only for equality, and first-seen times only for "is this one later".
    When two heads share a first-seen time — which Fabric and NeoForge revisions routinely do,
    since neither publishes a timestamp and a whole run shares one clock reading — the
    checkpoint simply does not record which came first, and no rollback is claimed. Reaching
    for the version strings to break that tie is exactly the comparison this module exists to
    avoid.

    A rollback is filed under :func:`rollback_rev`, so it is new to the checkpoint exactly
    once; on later runs that revision is already seen and is filtered out before it can
    become a second issue.
    """
    del key  # named for the caller's benefit; the decision uses the checkpoint only
    latest_by_head: dict[str, str] = {}
    for rev, at in _seen(checkpoint_json):
        if not revs_of_key(rev):
            continue
        head, _ = split_rollback_rev(rev)
        latest_by_head[head] = max(latest_by_head.get(head, ""), at)
    if head_rev not in latest_by_head:
        return None
    head_seen_at = latest_by_head[head_rev]
    later = [(head, at) for head, at in latest_by_head.items() if head != head_rev and at > head_seen_at]
    if not later:
        return None
    # Ties keep the checkpoint's own order rather than inventing one from the revisions.
    return max(later, key=lambda entry: entry[1])[0]


SUPERSEDED_STATE = "superseded"  # the state readiness.SUPERSEDED names from the other side


def supersede_previews(
    client: Any,
    observation: Any,
    issue_number: int | None,
    cache: Any,
    *,
    publish: bool,
) -> list[int]:
    """Mark every open preview issue that ``observation`` is the promotion of.

    Only a revision observed on the destination branch gets here (the caller checks), so a
    framework listing the release, or a newer snapshot appearing, never supersedes anything.
    A preview closed as ``not_planned`` was declined by a human and is skipped, as is one a
    later lifecycle state already moved on from. Superseding *marks*; closing is lifecycle
    work and belongs to the stage that owns it.

    Candidates come from the issue search — ``26.4`` matches ``26.4-rc-1`` because the search
    ignores punctuation — plus the full marker index when this run has already paid for it.
    The search alone is allowed to miss; what makes the decision exact is that every
    candidate's marker is parsed and checked with :func:`is_promotion_of`.
    """
    # Imported here, not at module scope: ``tracker.issues`` imports this module and
    # ``readiness`` imports ``tracker.issues``.
    from . import readiness
    from .tracker import identity, issues

    candidates: dict[int, dict[str, Any]] = {}
    for hit in client.issues_search(identity.search_terms({"kind": "support", "rev": observation.rev})):
        candidates[int(hit["number"])] = hit
    if cache is not None and getattr(cache, "index", None) is not None:
        for issue in cache.index.values():
            candidates.setdefault(int(issue["number"]), issue)

    touched: list[int] = []
    for number in sorted(candidates):
        issue = candidates[number]
        fields = identity.parse_marker(str(issue.get("body") or "")) or {}
        if fields.get("kind") != "support" or fields.get("branch") == observation.branch:
            continue
        if fields.get("provider") != observation.provider or fields.get("component") != observation.component:
            continue
        if not is_promotion_of(str(fields.get("rev") or ""), observation.rev):
            continue
        if str(issue.get("state") or "open") != "open":
            continue
        body = str(issue.get("body") or "")
        if issues.existing_state(body) == SUPERSEDED_STATE:
            continue
        reference = f"#{issue_number}" if issue_number is not None else f"`{observation.rev}`"
        new_body = readiness.replace_section(
            body,
            [
                f"Superseded by {reference}: release `{observation.rev}` was observed on the "
                f"{observation.branch} branch at {observation.observed_at}. This preview needs no target."
            ],
            SUPERSEDED_STATE,
        )
        if new_body == body:
            continue
        if publish:
            client.issue_update(number, body=new_body)
        issue["body"] = new_body
        touched.append(number)
    return touched
