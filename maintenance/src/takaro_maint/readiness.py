"""Per-platform readiness: is there a framework a new game version can be built against?

A game support issue answers "Minecraft 26.3 is out, what do we owe?"; this module answers
the question underneath it — "can anyone build for 26.3 yet?" — as a table inside that same
issue, and moves its hidden lifecycle state between ``blocked-upstream`` and
``ready-for-agent``. It never files a second issue for a game version, never edits a catalog
record, and never overwrites a lifecycle state a later stage wrote.

Three pieces:

*The registry.* ``scan`` calls ``observe()`` once per source and ``reconcile()`` once per
unseen revision, in the catalog's sorted source order (``fabric-meta``, ``mojang-meta``,
``neoforge-maven``, ``paper-fill``). The game issue is rendered while the Mojang source is
being reconciled, so the Fabric listing observed moments earlier has to be reachable from
there — hence a run-scoped registry that each framework provider records its result in at
the end of ``observe()``. NeoForge and Paper come *after* Mojang in that order and write
their rows through their own reconcile pass later in the same run. The registry lives for
one process; the CLI is one process per run, and the test rig resets it explicitly.

*The table.* ``render_rows`` draws one row per platform and appends a hidden, sorted JSON
line so the next run can read back rows it did not observe itself. That durability is what
lets a single framework source update one row without erasing the other two.

*The reconcilers.* A framework observation finds the game issue by marker and rewrites only
the Readiness section of the owned block. A branch-review observation — an upstream branch
the catalog does not declare — gets an issue of its own that a human either accepts (by
adding a channel) or declines (by closing it as not planned).
"""

from __future__ import annotations

import json
import re
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import channels, net
from .exit_codes import UpstreamUnavailable

if TYPE_CHECKING:  # pragma: no cover - imported for types only
    from .github import GitHub
    from .providers.base import Observation
    from .tracker.issues import GameTargets, ReconcileResult

#: Readiness states, worst first. ``missing`` is also what an absent row renders as.
MISSING = "missing"
READY = "ready"
PREVIEW_ONLY = "preview-only"

#: The hidden line that makes a rendered row readable again on the next run.
READINESS_MARK = "<!-- takaro-maint:readiness="
READINESS_END = " -->"
_READINESS_RE = re.compile(r"^<!-- takaro-maint:readiness=(?P<payload>.*) -->$", re.MULTILINE)

HEADING = "## Readiness"
STATE_MARK = "<!-- takaro-maint:state="

BLOCKED = "blocked-upstream"
READY_FOR_AGENT = "ready-for-agent"
REVIEW = "review"
SUPERSEDED = "superseded"

#: Lifecycle states readiness is allowed to overwrite. Anything else (``implementation-pr``,
#: ``awaiting-release``, ``released``, ``superseded``, ``declined``) was written by a later
#: stage that knows more than this scan does, and is left exactly as it is.
OWNED_STATES = (None, "detected", BLOCKED, READY_FOR_AGENT)

DASH = "—"


@dataclass
class Row:
    """One platform's readiness for one game version, as the table and the hidden line hold it."""

    platform: str
    status: str
    rev: str | None = None
    artifact: str | None = None
    sha256: str | None = None
    observed_at: str | None = None
    rollback_from: str | None = None
    branch: str | None = None

    @property
    def is_ready(self) -> bool:
        return self.status == READY or self.status.startswith(READY + " ")


# -- the run-scoped registry ---------------------------------------------------
@dataclass
class Registry:
    """What each framework provider observed in this run, keyed by its component."""

    results: dict[str, tuple[dict[str, Any], Any]] = field(default_factory=dict)

    def record(self, *, watch: dict[str, Any], result: Any) -> None:
        self.results[str(watch.get("component") or "")] = (watch, result)

    def watch_for(self, component: str) -> dict[str, Any] | None:
        found = self.results.get(component)
        return found[0] if found else None

    def observed(self, component: str, observation: Any) -> None:
        """Replace a recorded observation with the enriched version of itself.

        ``observe()`` records cheap listings; ``enrich()`` returns a *new* observation
        carrying the per-revision detail, and a row rendered from the listing alone would
        show an artifact with no digest beside one the same run verified. The swap is by
        identity, so an enrichment for a revision this result never listed is ignored.
        """
        found = self.results.get(component)
        if found is None:
            return
        listed = getattr(found[1], "observations", [])
        for index, existing in enumerate(listed):
            if existing.identity == observation.identity:
                listed[index] = observation
                return

    def forget(self, component: str) -> None:
        """Drop a component's result: a source that failed claims nothing.

        ``observe()`` records the listing; the per-revision ``enrich()`` can still fail
        afterwards, and the scan then marks the whole source failed and advances none of its
        checkpoint. A readiness row left behind from that listing would be a claim made on
        behalf of a source the same run is reporting as broken, so it is withdrawn.
        """
        self.results.pop(component, None)


_REGISTRY = Registry()


def registry() -> Registry:
    """The registry for the current run."""
    return _REGISTRY


def reset_registry() -> None:
    """Forget every framework result. Called by the test rig; a CLI run is one process."""
    _REGISTRY.results.clear()


# -- listing helpers the framework providers share -----------------------------
def fetch_json(url: str, *, what: str) -> Any:
    """GET ``url`` and parse it as JSON, or fail this source.

    Deliberately uncached: a listing is the one thing that must be current, and a stale
    listing would look exactly like a rollback.
    """
    with tempfile.TemporaryDirectory(prefix="takaro-maint-observe-") as tmp:
        destination = Path(tmp) / "listing.json"
        net.fetch(url, destination, net.Expectation(), no_cache=True)
        text = destination.read_text(encoding="utf-8")
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise UpstreamUnavailable(f"{url}: {what} is not JSON ({exc})", url=url) from exc


def fetch_xml_versions(url: str) -> list[str]:
    """Every ``<version>`` of a Maven metadata document, in publish order (oldest first).

    The ``<latest>`` and ``<release>`` pointers are never read: NeoForge's ``<release>``
    points at a beta, so the only trustworthy thing in the document is the order.
    """
    with tempfile.TemporaryDirectory(prefix="takaro-maint-observe-") as tmp:
        destination = Path(tmp) / "maven-metadata.xml"
        net.fetch(url, destination, net.Expectation(), no_cache=True)
        text = destination.read_text(encoding="utf-8")
    try:
        root = ET.fromstring(text)  # noqa: S314 - Maven metadata from a pinned catalog host
    except ET.ParseError as exc:
        raise UpstreamUnavailable(f"{url}: not maven metadata XML ({exc})", url=url) from exc
    versions = [(element.text or "").strip() for element in root.findall("versioning/versions/version")]
    return [version for version in versions if version]


_HEX_RE = re.compile(r"^[0-9a-f]{64}$")


def fetch_sha256(url: str) -> str:
    """The plain-hex digest a ``.sha256`` sidecar carries.

    A sidecar that is not a digest means the mirror is serving something else (an error
    page, an index); that is a failed source, not an artifact without a digest, because a
    row claiming a digest is a claim this tool has to be able to stand behind.
    """
    with tempfile.TemporaryDirectory(prefix="takaro-maint-observe-") as tmp:
        destination = Path(tmp) / "digest"
        net.fetch(url, destination, net.Expectation(), no_cache=True)
        body = destination.read_text(encoding="utf-8")
    digest = body.strip().split()[0].lower() if body.strip() else ""
    if not _HEX_RE.match(digest):
        raise UpstreamUnavailable(f"{url}: sidecar is not a sha256 digest", url=url)
    return digest


def artifact(name: str, url: str, *, sha256: str | None = None, size: int | None = None) -> dict[str, Any]:
    """The ``facts.artifact`` shape every framework observation carries."""
    return {"name": name, "url": url, "sha256": sha256, "size": size}


# -- building rows out of what the registry holds ------------------------------
def _preview_detail(observation: Any) -> str:
    """How a preview row names the thing it saw: a build number when there is one."""
    build = observation.facts.get("build")
    head, _ = channels.split_rollback_rev(observation.rev)
    return f"build {build}" if build is not None else head


def _row_from(platform: str, observation: Any, *, ready: bool) -> Row:
    head, rollback_from = channels.split_rollback_rev(observation.rev)
    facts = observation.facts
    asset = facts.get("artifact") or {}
    status = READY if ready else f"{PREVIEW_ONLY} ({observation.branch} {_preview_detail(observation)})"
    return Row(
        platform=platform,
        status=status,
        rev=head,
        artifact=str(asset.get("name")) if asset.get("name") else None,
        sha256=str(asset.get("sha256")) if asset.get("sha256") else None,
        observed_at=observation.observed_at or None,
        rollback_from=rollback_from or str(facts.get("rollbackFrom") or "") or None,
        branch=observation.branch,
    )


def row_from(platform: str, observation: Any, *, branch: str) -> Row:
    """One observation as a row on a game issue of ``branch``.

    ``ready`` needs an observation on the *same* branch as the game issue, carrying a named
    artifact. An observation on another branch is a preview and is reported as such; it
    never counts as ready. A preview branch is only ever observed when the catalog enables
    that channel, so an unwatched channel cannot produce a row at all.
    """
    if observation.branch != branch:
        return _row_from(platform, observation, ready=False)
    if not (observation.facts.get("artifact") or {}).get("name"):
        return Row(platform=platform, status=MISSING)
    return _row_from(platform, observation, ready=True)


def row_for(platform: str, result: Any, game_version: str, *, branch: str) -> Row:
    """This platform's row for one game version, out of everything ``observe()`` reported."""
    on_branch: Any = None
    preview: Any = None
    for observation in getattr(result, "observations", []):
        if observation.kind != "framework" or str(observation.facts.get("gameVersion")) != game_version:
            continue
        if observation.branch == branch:
            on_branch = observation
        elif preview is None:
            preview = observation
    if on_branch is not None:
        return row_from(platform, on_branch, branch=branch)
    if preview is not None:
        return _row_from(platform, preview, ready=False)
    return Row(platform=platform, status=MISSING)


def rows_from_registry(game_version: str, platforms: list[str], *, branch: str) -> dict[str, Row] | None:
    """Readiness rows for one game version, or ``None`` when no framework ran this run.

    ``None`` is not "nothing is ready": it means this run observed no framework at all (a
    ``--source mojang-meta`` run, say), and the caller must leave the issue's Readiness
    section exactly as it found it rather than claim everything is missing.
    """
    if not _REGISTRY.results:
        return None
    rows: dict[str, Row] = {}
    for platform in platforms:
        found = _REGISTRY.results.get(platform)
        if found is None:
            continue
        rows[platform] = row_for(platform, found[1], game_version, branch=branch)
    return rows


# -- the hidden line, merging, rendering ---------------------------------------
def dump_rows(rows: dict[str, Row]) -> str:
    """The hidden line: sorted keys, one line, no whitespace surprises."""
    payload = {platform: asdict(row) for platform, row in sorted(rows.items())}
    return READINESS_MARK + json.dumps(payload, sort_keys=True, separators=(",", ":")) + READINESS_END


def parse_rows(body: str) -> dict[str, Row] | None:
    """The rows a previous run recorded in ``body``, or ``None`` when there are none.

    A hand-mangled line is treated as absent rather than as an error: the next render
    replaces it, and no readiness claim is ever invented from an unparseable one.
    """
    match = _READINESS_RE.search(body or "")
    if match is None:
        return None
    try:
        payload = json.loads(match.group("payload"))
    except json.JSONDecodeError:
        return None
    if not isinstance(payload, dict):
        return None
    rows: dict[str, Row] = {}
    for platform, values in payload.items():
        if not isinstance(values, dict):
            continue
        optional = {
            key: (str(values[key]) if values.get(key) is not None else None)
            for key in Row.__dataclass_fields__
            if key in values and key not in ("platform", "status")
        }
        rows[str(platform)] = Row(platform=str(platform), status=str(values.get("status") or MISSING), **optional)
    return rows


def merge(existing: dict[str, Row] | None, fresh: dict[str, Row]) -> dict[str, Row]:
    """Fresh rows win; rows nobody observed this run are kept as they were.

    This is what makes one source's update safe: Paper reconciling its own observation
    rewrites the Paper row and carries Fabric's and NeoForge's through untouched.
    """
    merged = dict(existing or {})
    merged.update(fresh)
    return merged


def _cell(value: str | None) -> str:
    return value if value else DASH


def render_rows(rows: dict[str, Row], platforms: list[str]) -> list[str]:
    """The Readiness section's lines: the table, then the hidden line.

    Platforms are sorted, like the affected-targets table above it, so the rendering is
    stable whatever order a game record happens to list them in. A platform with no row
    renders ``missing`` — an unobserved platform and an observed-but-absent one say the same
    thing to a reader, and both are honest.
    """
    wanted = sorted(set(platforms) | set(rows))
    lines = [
        "| Platform | Readiness | Framework release | Artifact | sha256 | Observed |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for platform in wanted:
        row = rows.get(platform) or Row(platform=platform, status=MISSING)
        status = row.status
        if row.rollback_from:
            status = f"{status} (rolled back from {row.rollback_from})"
        lines.append(
            f"| {platform} | {status} | {f'`{row.rev}`' if row.rev else DASH} | {_cell(row.artifact)} "
            f"| {f'`{row.sha256}`' if row.sha256 else DASH} | {_cell(row.observed_at)} |"
        )
    lines += ["", dump_rows(rows)]
    return lines


def state_for(rows: dict[str, Row], *, branch: str, current: str | None) -> str:
    """``ready-for-agent`` when some platform is ready, else ``blocked-upstream``.

    Applied only over the states readiness owns: a lifecycle state a later stage wrote is
    returned unchanged, so a scan can never walk an issue backwards.
    """
    del branch  # readiness is the same question on every branch; the rows differ, not the rule
    if current not in OWNED_STATES:
        return str(current)
    return READY_FOR_AGENT if any(row.is_ready for row in rows.values()) else BLOCKED


def has_owned_readiness(body: str) -> bool:
    """Whether ``body`` still has the three landmarks :func:`replace_section` writes between."""
    from .tracker import issues

    _, begin, rest = (body or "").partition(issues.OWNED_BEGIN)
    if not begin:
        return False
    block = rest.partition(issues.OWNED_END)[0]
    return HEADING in block and STATE_MARK in block.partition(HEADING)[2]


def replace_section(body: str, lines: list[str], state: str) -> str:
    """Rewrite the Readiness section and the state token, inside the owned block only.

    Everything before ``## Readiness`` and everything from the state token onwards is
    preserved byte for byte. A body with no owned block, no heading or no state token has
    been edited beyond recognition; it is returned untouched and the caller reports it,
    because guessing where the section used to be would destroy someone's writing.
    """
    from .tracker import issues

    before, begin, rest = (body or "").partition(issues.OWNED_BEGIN)
    if not begin:
        return body
    block, end, after = rest.partition(issues.OWNED_END)
    head, heading, tail = block.partition(HEADING)
    if not heading:
        return body
    state_index = tail.find(STATE_MARK)
    if state_index < 0:
        return body
    trailing = tail[state_index:]
    _, _, rest_after_state = trailing.partition(READINESS_END)
    new_block = head + heading + "\n\n" + "\n".join(lines) + "\n\n" + STATE_MARK + state + READINESS_END
    return before + begin + new_block + rest_after_state + end + after


# -- reconciling a framework observation ---------------------------------------
def _result(action: str, number: int | None = None, changed: bool = False, reason: str | None = None) -> Any:
    from .tracker.issues import ReconcileResult

    return ReconcileResult(action, number, changed, reason)


def _game_marker(observation: Any, targets: Any, branch: str) -> dict[str, str]:
    watch = registry().watch_for(observation.component) or {}
    game = watch.get("game") or {}
    # Documented fallback: a provider that recorded no ``watch.game`` still joins to the
    # game this catalog game record is for, which is what every real watch block names.
    provider = str(game.get("provider") or "mojang")
    component = str(game.get("component") or targets.name.lower())
    return {
        "kind": "support",
        "provider": provider,
        "component": component,
        "branch": branch,
        "rev": str(observation.facts.get("gameVersion") or ""),
    }


def _closed_result(issue: dict[str, Any]) -> Any:
    number = int(issue["number"])
    reason = str(issue.get("state_reason") or "completed")
    if reason == "not_planned":
        return _result("declined", number, False, "declined")
    if reason == "completed":
        return _result("closed-completed", number, False, "closed-completed")
    return _result("closed-other", number, False, "closed-other")


def reconcile_framework(
    client: GitHub,
    observation: Observation,
    targets: GameTargets,
    *,
    publish: bool,
    cache: Any,
) -> ReconcileResult:
    """Write this framework observation's row into the game issue it belongs to.

    Never creates an issue: a framework release nobody is waiting for is not work. The
    game version is joined on the support marker of the game provider named by the watch
    block, on both game branches, so a preview issue gets its row as well as the release.
    """
    from .tracker import issues

    game_version = str(observation.facts.get("gameVersion") or "")
    if not game_version:
        return _result("noop", None, False, "no-game-version")
    recorded = registry().results.get(observation.component)

    found: list[tuple[str, dict[str, Any]]] = []
    for branch in ("release", "snapshot"):
        marker = _game_marker(observation, targets, branch)
        issue = issues.find(client, marker, cache)
        if issue is not None:
            found.append((branch, issue))
    if not found:
        return _result("noop", None, False, "no-support-issue")

    outcome = _result("noop", int(found[0][1]["number"]), False, "readiness-current")
    for branch, issue in found:
        if str(issue.get("state") or "open") == "closed":
            outcome = _closed_result(issue)
            continue
        number = int(issue["number"])
        body = str(issue.get("body") or "")
        if not has_owned_readiness(body):
            outcome = _result("noop", number, False, "unrecognised-body")
            continue
        current = issues.existing_state(body)
        if current == SUPERSEDED:
            # A superseded preview needs no readiness: the release it was leading up to
            # shipped. Its Readiness section holds the "Superseded by #n" sentence that says
            # so, and a table rendered over the top would delete the only explanation the
            # issue has. Every other state keeps its rows updated; only the state is kept.
            outcome = _result("noop", number, False, "superseded")
            continue
        # The whole provider result, not this one observation: with several channels
        # enabled a game version has a row per channel, and which of them happens to be
        # reconciled last must not decide whether the platform reads ready or preview-only.
        if recorded is not None:
            row = row_for(observation.component, recorded[1], game_version, branch=branch)
        else:
            row = row_from(observation.component, observation, branch=branch)
        rows = merge(parse_rows(body), {observation.component: row})
        new_body = replace_section(
            body,
            render_rows(rows, targets.platforms),
            state_for(rows, branch=branch, current=current),
        )
        if new_body == body:
            outcome = _result("noop", number, False, "readiness-current")
            continue
        if publish:
            client.issue_update(number, body=new_body)
        issue["body"] = new_body
        outcome = _result("update", number, True)
    return outcome


# -- reconciling a branch-review observation -----------------------------------
REVIEW_HEADING = "## Scope"

REVIEW_INTRO = (
    "`takaro-maint scan` saw a release on an upstream branch the catalog does not declare, so it was recorded "
    "rather than turned into something to support. Decide which it is: add a channel for this branch to the "
    "game record's watch block to start watching it, or close this issue as not planned to decline it for "
    "good — a declined branch is never re-filed, and the first line of this body is the marker that makes "
    "that stick."
)


def review_title(observation: Any, game_name: str) -> str:
    return (
        f"{game_name} {observation.component}: unrecognised upstream branch "
        f"'{observation.branch}' ({observation.rev}) needs review"
    )


def render_review_block(observation: Any) -> str:
    """The generated section of a branch-review issue."""
    from .tracker import issues

    facts = observation.facts
    asset = facts.get("artifact") or {}
    listing = facts.get("listing") or {}
    return "\n".join(
        [
            issues.OWNED_BEGIN,
            "## Observation",
            "",
            "| Field | Value |",
            "| --- | --- |",
            f"| Component | {observation.component} |",
            f"| Upstream branch label | `{facts.get('channel', DASH)}` (branch `{observation.branch}`) |",
            f"| Revision | `{observation.rev}` |",
            f"| Reason | {facts.get('reason', DASH)} |",
            f"| Artifact | {asset.get('url') or DASH} |",
            f"| Listing | {listing.get('url') or DASH} |",
            f"| Observed | {observation.observed_at} by takaro-maint scan |",
            "",
            REVIEW_HEADING,
            "",
            "A branch under review is not a support commitment: no readiness row anywhere changes because "
            "of it, and no target is added, retired or re-pointed.",
            "",
            f"{STATE_MARK}{REVIEW}{READINESS_END}",
            "",
            "## Acceptance",
            "",
            "- [ ] decided: watch it (channel added) / ignore it (closed as not planned)",
            issues.OWNED_END,
        ]
    )


def reconcile_branch_review(
    client: GitHub,
    observation: Observation,
    targets: GameTargets,
    *,
    publish: bool,
    cache: Any,
) -> ReconcileResult:
    """One issue per undeclared ``(provider, branch, revision)``, declinable and never reopened."""
    from .tracker import identity, issues

    marker = {
        "kind": "branch-review",
        "provider": observation.provider,
        "component": observation.component,
        "branch": observation.branch,
        "rev": observation.rev,
    }
    existing = issues.find(client, marker, cache)
    block = render_review_block(observation)
    if existing is None:
        body = "\n\n".join([identity.render_marker(marker), REVIEW_INTRO, block]) + "\n"
        if publish:
            created = client.issue_create(review_title(observation, targets.name), body, [identity.LABEL])
            cache_index = getattr(cache, "index", None)
            if cache_index is not None:
                cache_index[identity.render_marker(marker)] = created
            return _result("create", int(created["number"]), True)
        return _result("create", None, True)

    number = int(existing["number"])
    if str(existing.get("state") or "open") == "closed":
        return _closed_result(existing)
    old_body = str(existing.get("body") or "")
    new_body = issues.render_body(marker, block, old_body)
    if new_body == old_body:
        return _result("noop", number, False, "tracked")
    if publish:
        client.issue_update(number, body=new_body)
    existing["body"] = new_body
    return _result("update", number, True)
