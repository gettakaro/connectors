"""One observation, one issue — found by marker, rewritten only inside its own block.

Everything between ``OWNED_BEGIN`` and ``OWNED_END`` belongs to the command and is
regenerated on every run. Everything else in the body, and the title, belongs to whoever
typed it and is preserved byte for byte. A closed issue is never edited and never reopened.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any

from ..github import GitHub
from ..providers.base import Observation
from . import identity

OWNED_BEGIN = "<!-- takaro-maint:owned:begin -->"
OWNED_END = "<!-- takaro-maint:owned:end -->"
STATE_RE = re.compile(r"<!-- takaro-maint:state=([a-z-]+) -->")

INTRO = (
    "Mojang published a new stable Minecraft release. Everything between the owned markers is "
    "rewritten by `takaro-maint scan`; edit anything else freely \u2014 but leave the first line "
    "where it is, because that marker is how this issue is recognised."
)

PREVIEW_INTRO = (
    "Mojang published a new preview (snapshot channel). Everything between the owned markers is "
    "rewritten by `takaro-maint scan`; edit anything else freely \u2014 but leave the first line "
    "where it is, because that marker is how this issue is recognised."
)

READINESS_SENTENCE = (
    "Framework readiness is not assessed by this scan; it arrives with platform-readiness "
    "tracking. Nothing here changes a target's support status."
)

DASH = "—"


@dataclass(frozen=True)
class TargetRow:
    """One non-retired catalog target, as the affected-targets table shows it."""

    platform: str
    id: str
    revision: str
    status: str


@dataclass(frozen=True)
class GameTargets:
    """What the issue body needs to know about the game the observation belongs to."""

    name: str
    platforms: list[str] = field(default_factory=list)
    rows: list[TargetRow] = field(default_factory=list)


@dataclass
class LookupCache:
    """Per-run memo so the paginated full listing happens at most once."""

    index: dict[str, dict[str, Any]] | None = None


@dataclass(frozen=True)
class ReconcileResult:
    action: str  # create | update | noop | closed-completed | declined | closed-other
    issue_number: int | None = None
    changed: bool = False
    reason: str | None = None


def _index(client: GitHub, cache: LookupCache) -> dict[str, dict[str, Any]]:
    if cache.index is None:
        index: dict[str, dict[str, Any]] = {}
        for issue in client.issues_list(state="all", labels=identity.LABEL):
            fields = identity.parse_marker(str(issue.get("body") or ""))
            if fields:
                index.setdefault(identity.render_marker(fields), issue)
        cache.index = index
    return cache.index


def find(client: GitHub, marker: dict[str, str], cache: LookupCache) -> dict[str, Any] | None:
    """Search pre-filter, exact marker parse, then a paginated open+closed listing.

    The search is allowed to over-match (it ignores punctuation) and allowed to miss (the
    index lags); the listing is what makes "no duplicate, ever" true.
    """
    wanted = identity.render_marker(marker)
    for hit in client.issues_search(identity.search_terms(marker)):
        if identity.parse_marker(str(hit.get("body") or "")) == marker:
            return hit
    return _index(client, cache).get(wanted)


def title_for(observation: Observation, game_name: str) -> str:
    """The title of the issue one observation files, by kind and then by branch."""
    if observation.kind == "branch-review":
        from .. import readiness

        return readiness.review_title(observation, game_name)
    if observation.branch == "release":
        return f"{game_name} {observation.rev}: new stable release needs a target"
    return f"{game_name} {observation.rev}: new {observation.branch} preview"


def _target_rows(targets: GameTargets, rev: str) -> list[str]:
    rows = [
        f"| {row.platform} | `{row.id}` | {row.revision} | {row.status} | {'yes' if row.revision == rev else 'no'} |"
        for row in sorted(targets.rows, key=lambda row: row.id)
    ]
    covered = {row.platform for row in targets.rows}
    rows += [
        f"| {platform} | {DASH} | {DASH} | {DASH} | no |"
        for platform in sorted(p for p in targets.platforms if p not in covered)
    ]
    return rows or [f"| (none) | {DASH} | {DASH} | {DASH} | no |"]


def render_owned_block(
    observation: Observation,
    targets: GameTargets,
    *,
    state: str = "detected",
    readiness: list[str] | None = None,
) -> str:
    """The generated section of a support issue. ``readiness`` is the hook for #154."""
    facts = observation.facts
    rev = observation.rev
    manifest_list = facts.get("manifestList") or {}
    manifest = facts.get("manifest") or {}
    server = facts.get("server") or {}
    component = observation.component
    lines = [
        OWNED_BEGIN,
        "## Observation",
        "",
        "| Field | Value |",
        "| --- | --- |",
        f"| Provider / component / branch | {observation.provider} / {component} / {observation.branch} |",
        f"| Revision | `{rev}` |",
        f"| Released | {facts.get('releaseTime', DASH)} |",
        f"| Version list | {manifest_list.get('url', DASH)} |",
        f"| Version manifest | {manifest.get('url', DASH)} (sha1 `{manifest.get('sha1', DASH)}`) |",
        f"| Server jar | {server.get('url', DASH)} (sha1 `{server.get('sha1', DASH)}`, "
        f"{server.get('size', DASH)} bytes) |",
        f"| Java | {facts.get('javaMajor', DASH)} |",
        f"| Observed | {observation.observed_at} by takaro-maint scan |",
        "",
        "## Affected targets",
        "",
        f"| Platform | Target | Revision | Support | Covers {rev} |",
        "| --- | --- | --- | --- | --- |",
        *_target_rows(targets, rev),
        "",
        "## Readiness",
        "",
        *(readiness if readiness else [READINESS_SENTENCE]),
        "",
        f"<!-- takaro-maint:state={state} -->",
        "",
        "## Next steps (reproducible)",
        "",
        f"1. `maintenance/bin/takaro-maint targets list --game {component}`",
        f"2. Add `catalog/{component}/targets/<platform>-{rev}.json` following `catalog/README.md` "
        f'→ "Adding a target" (pin manifest sha1 `{manifest.get("sha1", DASH)}`, '
        f"server sha1 `{server.get('sha1', DASH)}` size `{server.get('size', DASH)}`).",
        "3. `maintenance/bin/takaro-maint catalog validate --online`",
        f"4. `maintenance/bin/takaro-maint build --game {component} --target <platform>-{rev} "
        "--version <version> --out dist`",
        f"5. `maintenance/bin/takaro-maint verify --game {component} --target <platform>-{rev} "
        "--artifacts dist --out reports`",
        "",
        "## Expected evidence",
        "",
        "- `catalog validate --online` exit 0 on the new record",
        "- `reports/<target>/report.json` with `level` ≥ `protocol` and every check `pass`",
        "- a pull request whose body contains `Refs #<this issue>`",
        "",
        "## Acceptance",
        "",
        f"- [ ] a catalog target for {rev} exists with `support.status: candidate` and upstream hashes",
        "- [ ] the connector builds and verifies against it (report attached or linked)",
        "- [ ] the target is promoted to `maintained` with the evidence cited in `support.evidence`",
        OWNED_END,
    ]
    return "\n".join(lines)


def existing_state(body: str) -> str | None:
    """The lifecycle state a previous run (or #156) wrote into the owned block."""
    match = STATE_RE.search(body or "")
    return match.group(1) if match else None


def render_body(
    marker: dict[str, str],
    owned_block: str,
    existing_body: str | None = None,
    intro: str = INTRO,
) -> str:
    """A new body, or the existing one with only the owned block replaced.

    ``intro`` is only ever read for a body that does not exist yet: the prose above an
    existing owned block belongs to whoever typed it and is never rewritten.
    """
    if existing_body is None:
        return "\n\n".join([identity.render_marker(marker), intro, owned_block]) + "\n"
    before, begin, rest = existing_body.partition(OWNED_BEGIN)
    if not begin:
        # A human deleted the markers. Append one fresh block and never a second.
        return existing_body.rstrip("\n") + "\n\n" + owned_block + "\n"
    _, end, after = rest.partition(OWNED_END)
    return before + owned_block + (after if end else "\n")


def reconcile(
    client: GitHub,
    observation: Observation,
    targets: GameTargets,
    *,
    publish: bool,
    cache: LookupCache,
) -> ReconcileResult:
    """Bring the tracker in line with one observation, writing at most one issue."""
    if observation.kind != "game":
        # Imported here: ``readiness`` imports this module to render and find issues.
        from .. import readiness

        reconciler = (
            readiness.reconcile_branch_review if observation.kind == "branch-review" else readiness.reconcile_framework
        )
        return reconciler(client, observation, targets, publish=publish, cache=cache)

    from .. import readiness  # same cycle, same reason

    marker = identity.support_marker(observation)
    existing = find(client, marker, cache)

    rows = readiness.rows_from_registry(observation.rev, targets.platforms, branch=observation.branch)
    lines = readiness.render_rows(rows, targets.platforms) if rows is not None else None

    if existing is None:
        state = readiness.state_for(rows, branch=observation.branch, current=None) if rows is not None else "detected"
        block = render_owned_block(observation, targets, state=state, readiness=lines)
        intro = INTRO if observation.branch == "release" else PREVIEW_INTRO
        created_number: int | None = None
        if publish:
            created = client.issue_create(
                title_for(observation, targets.name),
                render_body(marker, block, None, intro),
                [identity.LABEL],
            )
            created_number = int(created["number"])
            if cache.index is not None:
                # So a later observation in the same run finds this issue by marker instead
                # of re-listing (and, worse, filing a second one).
                cache.index[identity.render_marker(marker)] = created
        superseded = _supersede(client, observation, created_number, cache, publish=publish)
        return ReconcileResult("create", created_number, True, superseded)

    number = int(existing["number"])
    if str(existing.get("state") or "open") == "closed":
        reason = str(existing.get("state_reason") or "completed")
        if reason == "not_planned":
            return ReconcileResult("declined", number, False, "declined")
        if reason == "completed":
            return ReconcileResult("closed-completed", number, False, "closed-completed")
        return ReconcileResult("closed-other", number, False, "closed-other")

    old_body = str(existing.get("body") or "")
    current = existing_state(old_body) or "detected"
    if rows is not None:
        # A row a framework source wrote in an earlier run is carried through even when that
        # source did not run this time, so a partial scan never erases readiness it knows.
        rows = readiness.merge(readiness.parse_rows(old_body), rows)
        lines = readiness.render_rows(rows, targets.platforms)
        current = readiness.state_for(rows, branch=observation.branch, current=current)
    block = render_owned_block(observation, targets, state=current, readiness=lines)
    new_body = render_body(marker, block, old_body)
    changed = new_body != old_body
    if changed and publish:
        client.issue_update(number, body=new_body)
    if changed:
        existing["body"] = new_body
    superseded = _supersede(client, observation, number, cache, publish=publish)
    if not changed:
        return ReconcileResult("noop", number, False, superseded or "tracked")
    return ReconcileResult("update", number, True, superseded)


def _supersede(
    client: GitHub,
    observation: Observation,
    number: int | None,
    cache: LookupCache,
    *,
    publish: bool,
) -> str | None:
    """Mark the previews this release promotes, and say so in the result's reason.

    Only a release-branch observation promotes anything: a framework listing the version, or
    a newer snapshot appearing, is not evidence that the release shipped.
    """
    if observation.branch != "release":
        return None
    from .. import channels  # same cycle as above

    touched = channels.supersede_previews(client, observation, number, cache, publish=publish)
    return f"superseded:{','.join(str(issue) for issue in touched)}" if touched else None
