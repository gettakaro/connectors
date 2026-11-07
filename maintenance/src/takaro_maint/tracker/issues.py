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

#: The prose above the owned block of a filed issue, for a provider that says nothing of its
#: own. It names no game and no upstream mechanism; a provider with better words for its own
#: releases returns them as ``intro`` from :meth:`Provider.presentation`.
INTRO = (
    "`takaro-maint scan` saw a new release on a branch this catalog watches. Everything between the "
    "owned markers is rewritten by `takaro-maint scan`; edit anything else freely \u2014 but leave the "
    "first line where it is, because that marker is how this issue is recognised."
)

PREVIEW_INTRO = (
    "`takaro-maint scan` saw a new preview on a branch this catalog watches. Everything between the "
    "owned markers is rewritten by `takaro-maint scan`; edit anything else freely \u2014 but leave the "
    "first line where it is, because that marker is how this issue is recognised."
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
class IconPack:
    """How a game's Icon Pack is built and published, as far as the rebuild line needs it."""

    catalog_game: str
    pack_game: str
    needs_client: bool


@dataclass(frozen=True)
class GameTargets:
    """What the issue body needs to know about the game the observation belongs to."""

    name: str
    platforms: list[str] = field(default_factory=list)
    rows: list[TargetRow] = field(default_factory=list)
    icons: IconPack | None = None


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


def _presentation(observation: Observation, game_name: str) -> dict[str, Any]:
    """What the observation's own provider says its issue should read like.

    Looked up by provider id at render time, so this module knows nothing about any
    upstream: a provider with no opinion, or one this build does not carry at all, gets
    the generic rendering below. Adding a game therefore never edits this file.
    """
    from ..providers import providers

    hook = getattr(providers().get(observation.provider), "presentation", None)
    view = hook(observation, game_name) if hook is not None else None
    return view if isinstance(view, dict) else {}


def _lines(view: dict[str, Any], key: str, fallback: list[str]) -> list[str]:
    """One presentation key as rendered lines, or the generic rendering."""
    supplied = view.get(key)
    return [str(line) for line in supplied] if supplied else fallback


def observation_rows(observation: Observation, extra: list[str] | None = None) -> list[str]:
    """The Observation table's body: what every observation answers, plus the provider's own.

    ``extra`` is spliced between the facts every observation carries and the closing
    "Observed" row, which is where an upstream's own rows read naturally.
    """
    return [
        f"| Provider / component / branch | {observation.provider} / {observation.component} / {observation.branch} |",
        f"| Revision | `{observation.rev}` |",
        f"| Released | {observation.facts.get('releaseTime', DASH)} |",
        *(extra or []),
        f"| Observed | {observation.observed_at} by takaro-maint scan |",
    ]


def next_steps(observation: Observation, *, pin: str | None = None) -> list[str]:
    """The numbered steps that turn one observation into a verified target.

    ``pin`` is the parenthetical naming the upstream digests the new record has to pin.
    An upstream that publishes none (or pins by some other identity) leaves it out.
    """
    component = observation.component
    rev = observation.rev
    note = f" ({pin})" if pin else ""
    return [
        f"1. `maintenance/bin/takaro-maint targets list --game {component}`",
        f"2. Add `catalog/{component}/targets/<platform>-{rev}.json` following `catalog/README.md` "
        f'\u2192 "Adding a target"{note}.',
        "3. `maintenance/bin/takaro-maint catalog validate --online`",
        f"4. `maintenance/bin/takaro-maint build --game {component} --target <platform>-{rev} "
        "--version <version> --out dist`",
        f"5. `maintenance/bin/takaro-maint verify --game {component} --target <platform>-{rev} "
        "--artifacts dist --out reports`",
    ]


def title_for(observation: Observation, game_name: str) -> str:
    """The title of the issue one observation files, by kind and then by branch."""
    if observation.kind == "branch-review":
        from .. import readiness

        return readiness.review_title(observation, game_name)
    title = _presentation(observation, game_name).get("title")
    if title:
        return str(title)
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
    presentation: dict[str, Any] | None = None,
) -> str:
    """The generated section of a support issue.

    ``readiness`` is the framework table, when a framework was observed this run.
    ``presentation`` is what the observation's provider says its issue reads like, already
    looked up by the caller; leaving it out looks it up here. Everything the provider does
    not answer for is rendered generically, out of the observation alone.
    """
    rev = observation.rev
    view = _presentation(observation, targets.name) if presentation is None else presentation
    lines = [
        OWNED_BEGIN,
        "## Observation",
        "",
        "| Field | Value |",
        "| --- | --- |",
        *_lines(view, "observationRows", observation_rows(observation)),
        "",
        "## Affected targets",
        "",
        f"| Platform | Target | Revision | Support | Covers {rev} |",
        "| --- | --- | --- | --- | --- |",
        *_target_rows(targets, rev),
        "",
        "## Readiness",
        "",
        *(readiness if readiness else _lines(view, "readinessLines", [READINESS_SENTENCE])),
        "",
        f"<!-- takaro-maint:state={state} -->",
        "",
        "## Next steps (reproducible)",
        "",
        *_lines(view, "nextSteps", next_steps(observation)),
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
        *([icon_rebuild_line(targets.icons)] if targets.icons is not None else []),
        OWNED_END,
    ]
    return "\n".join(lines)


def icon_rebuild_line(pack: IconPack) -> str:
    """The acceptance line that reminds a maintainer to rebuild and republish the Icon Pack."""
    client = " --client <client install>" if pack.needs_client else ""
    return (
        "- [ ] the Icon Pack is rebuilt for the new target and published: "
        f"`maintenance/bin/takaro-maint icons build --game {pack.catalog_game} "
        f"--target <new target>{client} --out <out>`, "
        f"then `maintenance/bin/takaro-maint icons publish --env staging --pack <out>/pack/{pack.pack_game}`, "
        f"then `maintenance/bin/takaro-maint icons promote --game {pack.pack_game}` "
        "(see `maintenance/docs/icons.md`)"
    )


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
    # Once per reconciled observation: both renderings below are of the same observation,
    # and a provider must not be asked to describe it twice.
    view = _presentation(observation, targets.name)

    if existing is None:
        state = readiness.state_for(rows, branch=observation.branch, current=None) if rows is not None else "detected"
        block = render_owned_block(observation, targets, state=state, readiness=lines, presentation=view)
        intro = str(view.get("intro") or (INTRO if observation.branch == "release" else PREVIEW_INTRO))
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
    block = render_owned_block(observation, targets, state=current, readiness=lines, presentation=view)
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
