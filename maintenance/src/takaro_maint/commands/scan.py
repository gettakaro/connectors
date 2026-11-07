"""``scan`` — turn what upstream published into deduplicated maintenance issues.

One pass: read every watched source, work out which revisions have never been seen, and
reconcile each one into exactly one GitHub issue. Read-only is the default and writes
nothing at all; ``--publish`` is the only way to change the tracker. No state is kept on
disk — the checkpoints and the work identities live in the dashboard issue (the shared
download cache is the only thing a run leaves behind), so a fresh runner with an empty
home directory resumes exactly where the last run stopped.
"""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import exit_codes, github, observations, output, paths, redact
from ..catalog.loader import Catalog, Game, Target
from ..exit_codes import MaintError, TrackerError, UsageError
from ..providers import Provider, provider_for
from ..providers.base import Observation
from ..tracker import checkpoints, dashboard, issues
from ..tracker.checkpoints import Checkpoint, Entry
from . import load_catalog

SCHEMA = "takaro-maint-scan/1"
COVERING_STATUSES = ("candidate", "maintained")


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser(
        "scan",
        help="observe watched sources and reconcile new releases into maintenance issues",
    )
    parser.add_argument("--publish", action="store_true", help="actually write to the tracker (default: read-only)")
    parser.add_argument(
        "--bootstrap",
        action="store_true",
        help="initialise a source: record what is already published and file only the uncovered heads",
    )
    parser.add_argument("--game", default=None, help="restrict the scan to one catalog game")
    parser.add_argument("--source", action="append", default=None, help="watched source id; repeatable")
    parser.add_argument("--repo", default=None, help="tracker repository (default: $TAKARO_MAINT_REPO, then origin)")
    parser.add_argument("--api-url", default=None, help="GitHub API base (default: $TAKARO_MAINT_GITHUB_API_URL)")
    parser.add_argument("--out", default=None, help="also write the report to this file (mode 0600)")
    parser.set_defaults(handler=_scan, op="scan")


@dataclass(frozen=True)
class WatchedSource:
    """One catalog source that carries a ``watch`` block."""

    game: Game
    source_id: str
    record: dict[str, Any]

    @property
    def key(self) -> str:
        return f"{self.game.id}/{self.source_id}"


@dataclass
class SourceOutcome:
    """What one source produced, held back until the dashboard write."""

    status: str
    history: str
    heads: dict[str, str]
    checkpoint: Checkpoint | None
    error: str | None
    work: list[tuple[str, int]]


def _watched(catalog: Catalog, args: Any) -> list[WatchedSource]:
    games = [catalog.game(args.game)] if args.game else [catalog.games[key] for key in sorted(catalog.games)]
    found: list[WatchedSource] = []
    for game in games:
        for source_id, record in sorted((game.record.get("sources") or {}).items()):
            if record.get("watch"):
                found.append(WatchedSource(game=game, source_id=source_id, record=record))

    if args.source:
        by_name: dict[str, WatchedSource] = {}
        for source in found:
            by_name.setdefault(source.source_id, source)
            by_name[source.key] = source
        chosen: list[WatchedSource] = []
        for name in args.source:
            if name not in by_name:
                known = ", ".join(sorted(source.key for source in found)) or "<none>"
                raise UsageError(f"no watched source '{name}'; the catalog watches: {known}")
            if by_name[name] not in chosen:
                chosen.append(by_name[name])
        return chosen

    if not found:
        scope = f"game '{args.game}'" if args.game else "the catalog"
        raise UsageError(f"{scope} declares no source with a watch block; there is nothing to scan")
    return found


def _icon_pack(game: Game) -> issues.IconPack | None:
    """The game's Icon Pack route, tolerant of a malformed block (``catalog validate`` reports it)."""
    block = game.record.get("icons")
    if not isinstance(block, dict):
        return None
    pack_game = block.get("game")
    pixels = block.get("pixels")
    return issues.IconPack(
        catalog_game=game.id,
        pack_game=pack_game if isinstance(pack_game, str) and pack_game else game.id,
        needs_client=isinstance(pixels, dict) and pixels.get("source") == "client",
    )


def _game_targets(game: Game) -> issues.GameTargets:
    return issues.GameTargets(
        name=str(game.record.get("name") or game.id),
        platforms=[str(platform) for platform in game.record.get("platforms") or []],
        rows=[
            issues.TargetRow(platform=t.platform, id=t.id, revision=t.revision, status=t.status)
            for t in game.targets
            if t.status != "retired"
        ],
        icons=_icon_pack(game),
    )


def _targets_snapshot(game: Game) -> list[dict[str, Any]]:
    return [
        {"id": t.id, "platform": t.platform, "revision": t.revision, "status": t.status}
        for t in sorted(game.targets, key=lambda t: t.id)
        if t.status != "retired"
    ]


def _enrich(provider: Provider, observation: Observation, source: dict[str, Any]) -> Observation:
    """The provider convention's second step: fetch the per-revision detail.

    ``observe()`` is cheap and lists everything; ``enrich()`` is the per-revision fetch and
    runs only for revisions this run actually reconciles. A provider without one (nothing
    more to fetch) is served by its lightweight observation as it stands.
    """
    enrich = getattr(provider, "enrich", None)
    if enrich is None:
        return observation
    enriched: Observation = enrich(observation, source)
    return enriched


def _covered(provider: Provider, observation: Observation, targets: list[Target]) -> bool:
    """Whether the catalog already ships what this head is, on the provider's own terms.

    A bootstrap files the heads nobody has shipped yet and records the rest as seen.
    Comparing the observation's revision with each target's ``revision`` is right whenever
    a target is named by the string the provider observes; a provider whose upstream
    identity is richer than that answers ``covers()`` instead, and ``None`` from it means
    "no opinion, compare the strings".
    """
    covers = getattr(provider, "covers", None)
    verdict = covers(observation, targets) if covers is not None else None
    if verdict is not None:
        return bool(verdict)
    return observation.rev in {target.revision for target in targets}


def _entry_for(
    outcome: issues.ReconcileResult,
    observation: Observation,
    targets: issues.GameTargets,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """``(what was intended, what was applied)`` for one reconciled observation."""
    if outcome.action == "create":
        intent: dict[str, Any] = {
            "action": "create-issue",
            "identity": observation.identity,
            "title": issues.title_for(observation, targets.name),
        }
        return intent, {**intent, "issue": outcome.issue_number}
    if outcome.action == "update":
        update = {"action": "update-issue", "issue": outcome.issue_number, "identity": observation.identity}
        return update, dict(update)
    noop = {
        "action": "noop",
        "issue": outcome.issue_number,
        "identity": observation.identity,
        "reason": outcome.reason,
    }
    return noop, dict(noop)


def _write_out(path: str, text: str) -> None:
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8")
    os.chmod(destination, 0o600)


def _emit(report: dict[str, Any], *, code: int, out: str | None) -> int:
    ok = code == exit_codes.OK
    fields = dict(report)
    if ok:
        fields.pop("error", None)
    elif not fields.get("error"):
        fields["error"] = exit_codes.DESCRIPTIONS.get(code, "scan failed")
    output.emit("scan", ok, **fields)
    if out:
        document = {"schemaVersion": 1, "op": "scan", "ok": ok, **fields}
        _write_out(out, redact.redact(json.dumps(document, indent=2, ensure_ascii=False)))
    return code


def _observe_one(
    source: WatchedSource,
    mapping: dict[str, Any],
    before: Checkpoint | None,
    *,
    args: Any,
    publish: bool,
    client: github.GitHub,
    cache: issues.LookupCache,
    now: str,
    report: dict[str, Any],
    entry: dict[str, Any],
) -> SourceOutcome:
    """Observe one source and reconcile every unseen revision it reports.

    All or nothing: a provider failure part-way through leaves this source's checkpoint
    exactly where it was, so the next run retries the whole source rather than skipping
    the revisions whose issues were already filed (those are found by marker, not refiled).
    """
    provider = provider_for(str(source.record["provider"]))
    plan: list[dict[str, Any]] = report["plan"]
    applied: list[dict[str, Any]] = report["applied"]

    result = provider.observe(mapping)
    entry["history"] = result.history
    entry["heads"] = dict(result.heads)

    if before is None:
        heads = set(result.heads.values())
        covering = [t for t in source.game.targets if t.status in COVERING_STATUSES]
        fresh = [o for o in result.observations if o.rev in heads and not _covered(provider, o, covering)]
        fresh_ids = {o.rev for o in fresh}
        seeded: list[Entry] = [
            (o.rev, str(o.facts["releaseTime"])) for o in result.observations if o.rev not in fresh_ids
        ]
        checkpoint = checkpoints.seed(seeded, at=now)
        plan.append(
            {
                "action": "bootstrap-checkpoint",
                "source": source.key,
                "seeded": len(seeded),
                "heads": dict(result.heads),
            }
        )
        output.info(f"{source.key}: bootstrap seeds {len(seeded)} published revisions; {len(fresh)} to file")
        unseen = fresh
    else:
        checkpoint = before
        unseen = [
            o for o in result.observations if not checkpoints.is_seen(checkpoint, o.rev, str(o.facts["releaseTime"]))
        ]

    advanced: list[Entry] = []
    work: list[tuple[str, int]] = []
    targets = _game_targets(source.game)
    for observation in unseen:
        enriched = _enrich(provider, observation, mapping)
        document = observations.to_json(enriched, game=source.game.id, source_id=source.source_id)
        errors = observations.validate(document)
        if errors:
            raise RuntimeError(
                f"{source.key}: the '{enriched.provider}' provider produced an observation that does not "
                f"match {observations.SCHEMA_NAME}: {'; '.join(errors)}"
            )
        report["observations"].append(document)
        entry["observations"] = int(entry["observations"]) + 1

        outcome = issues.reconcile(client, enriched, targets, publish=publish, cache=cache)
        intent, done = _entry_for(outcome, enriched, targets)
        plan.append(intent)
        if publish:
            applied.append(done)
        output.info(f"{source.key}: {enriched.rev} -> {outcome.action}")
        if outcome.issue_number is not None:
            work.append((enriched.identity, outcome.issue_number))
        advanced.append((enriched.rev, str(enriched.facts["releaseTime"])))

    return SourceOutcome(
        status="ok",
        history=result.history,
        heads=dict(result.heads),
        checkpoint=checkpoints.advance(checkpoint, advanced, at=now),
        error=None,
        work=work,
    )


def _scan(args: Any) -> int:
    catalog = load_catalog()
    watched = _watched(catalog, args)
    client = github.client(args.repo, None, args.api_url, paths.repo_root())
    cache = issues.LookupCache()
    board = dashboard.load(client, cache)
    now = observations.utcnow()

    report: dict[str, Any] = {
        "schema": SCHEMA,
        "mode": "publish" if args.publish else "read-only",
        "bootstrap": bool(args.bootstrap),
        "repo": client.repo,
        "dashboard": {"issue": board.issue, "created": False},
        "sources": {},
        "observations": [],
        "plan": [],
        "applied": [],
    }
    if board.issue is None:
        report["plan"].append({"action": "create-dashboard"})

    outcomes: dict[str, SourceOutcome] = {}
    uninitialized: list[str] = []
    failed: list[str] = []
    blocked = False
    try:
        pending: list[tuple[WatchedSource, Checkpoint | None, dict[str, Any]]] = []
        for source in watched:
            before = board.checkpoint(source.key)
            entry: dict[str, Any] = {
                "status": "ok",
                "history": None,
                "heads": {},
                "checkpoint": {"before": before.summary() if before else None, "after": None},
                "observations": 0,
                "error": None,
            }
            report["sources"][source.key] = entry

            if before is None and not args.bootstrap:
                entry["status"] = "uninitialized"
                report["plan"].append({"action": "bootstrap-required", "source": source.key})
                output.info(f"{source.key}: no checkpoint yet; run with --bootstrap first")
                uninitialized.append(source.key)
                continue
            pending.append((source, before, entry))

        # Decided before anything is observed or filed: an uninitialised source stops the
        # whole publishing run, and "nothing was written" has to be literally true. Doing
        # this after the loop would file the other sources' issues first and then abandon
        # the run without recording them in the dashboard.
        blocked = bool(uninitialized) and bool(args.publish)
        if blocked:
            # Nothing at all is written: guessing what to file on a first run is how a
            # tracker gets flooded, so an uninitialised source stops the whole run. The
            # remaining sources are still scanned — read-only, so the report still says
            # what is out there and which source failed — but no issue and no checkpoint
            # is written, which is why the decision is taken here and not after the loop.
            names = ", ".join(uninitialized)
            output.error(f"{names}: no checkpoint yet; run with --bootstrap first (nothing was written)")
        publish = bool(args.publish) and not blocked

        for source, before, entry in pending:
            if before is not None and args.bootstrap:
                output.info(f"{source.key}: already initialised; scanning normally")

            mapping = {
                **source.record,
                "id": source.source_id,
                "game": source.game.id,
                "checkpoint": checkpoints.to_json(before) if before is not None else None,
            }
            try:
                outcome = _observe_one(
                    source,
                    mapping,
                    before,
                    args=args,
                    publish=publish,
                    client=client,
                    cache=cache,
                    now=now,
                    report=report,
                    entry=entry,
                )
            except TrackerError:
                raise
            except MaintError as exc:
                entry["status"] = "failed"
                entry["error"] = exc.message
                output.error(f"{source.key}: {exc.message}")
                outcomes[source.key] = SourceOutcome(
                    status="failed",
                    history=str(entry["history"] or "heads-only"),
                    heads=dict(entry["heads"]),
                    checkpoint=None,
                    error=exc.message,
                    work=[],
                )
                continue
            outcomes[source.key] = outcome
            entry["checkpoint"]["after"] = outcome.checkpoint.summary() if outcome.checkpoint else None

        failed = sorted(key for key, outcome in outcomes.items() if outcome.status == "failed")

        if publish:
            for key, outcome in outcomes.items():
                board.apply_source(
                    key,
                    status=outcome.status,
                    history=outcome.history,
                    heads=outcome.heads,
                    checkpoint=outcome.checkpoint,
                    last_error=outcome.error,
                    at=now,
                )
                for work_identity, number in outcome.work:
                    board.set_work(work_identity, issue=number, state="detected", at=now)
            for game_id in sorted({source.game.id for source in watched}):
                board.set_targets(game_id, _targets_snapshot(catalog.game(game_id)))
            board.data["updatedAt"] = now
            if not failed and not uninitialized:
                board.data["lastSuccess"] = now

            existing_issue = board.issue
            number, created = dashboard.save(client, board)
            report["dashboard"] = {"issue": number, "created": created}
            if existing_issue is not None:
                report["plan"].append({"action": "update-dashboard", "issue": existing_issue})
            report["applied"].append({"action": "create-dashboard" if created else "update-dashboard", "issue": number})
            for key, outcome in outcomes.items():
                after = board.checkpoint(key)
                if outcome.checkpoint is not None and after is not None:
                    report["sources"][key]["checkpoint"]["after"] = after.summary()
        elif board.issue is not None and not blocked:
            report["plan"].append({"action": "update-dashboard", "issue": board.issue})
    except TrackerError as exc:
        report["error"] = exc.message
        output.error(exc.message)
        return _emit(report, code=exit_codes.TRACKER, out=args.out)

    # A failed source outranks an uninitialised one: the run has to be repeated either
    # way, and "something upstream is broken" is the more urgent of the two to report.
    reasons = []
    if failed:
        reasons.append("these sources failed: " + ", ".join(failed))
    if blocked:
        reasons.append("these sources need --bootstrap first: " + ", ".join(uninitialized))
    if reasons:
        report["error"] = "; ".join(reasons)
        return _emit(
            report,
            code=exit_codes.UPSTREAM if failed else exit_codes.USAGE,
            out=args.out,
        )
    return _emit(report, code=exit_codes.OK, out=args.out)
