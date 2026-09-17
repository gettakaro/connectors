"""NeoForged: the Maven metadata that says which Minecraft versions NeoForge builds for.

NeoForge encodes the game version in its own version number, in two grammars that have to
be read differently:

``21.11.45``      three segments, the Minecraft 1.x line: ``1.<major>.<minor>`` — so this is
                  NeoForge for Minecraft ``1.21.11``, and ``21.0.x`` would be ``1.21``.
``26.2.0.88``     four segments, the modern line: ``<major>.<minor>[.<patch>]`` — so this is
                  NeoForge for ``26.2``, while ``26.1.2.109`` is NeoForge for ``26.1.2``.

A trailing ``-beta`` or ``-alpha…`` is the branch label; an empty suffix is a release.
Versions that fit neither grammar exist (``0.25w14craftmine.3-beta``, an April Fools line)
and are reported for review rather than guessed at.

The metadata's ``<latest>`` and ``<release>`` pointers are never read: upstream currently
points ``<release>`` at ``26.3.0.3-beta``. Publish order in ``<versions>`` is the only
ordering this provider trusts.

This module owns observation only. Acquiring a NeoForge installer as a build input is a
separate concern and is not implemented here.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import replace
from typing import Any

from .. import channels, observations, readiness
from ..exit_codes import MaintError
from ..tracker import identity
from .base import Observation, Provider, ProviderResult

#: ``<core>`` is three or four dot-separated numbers; everything from the first dash on is
#: the label. Split first, classify second — that way an unparseable core is reported as
#: unparseable instead of falling into whichever grammar happens to match part of it.
_VERSION_RE = re.compile(r"^(?P<core>\d+(?:\.\d+){2,3})(?P<label>-.*)?$")


def game_version_of(version: str) -> tuple[str, str] | None:
    """``(game version, upstream label)`` for a NeoForge version, or ``None`` when unparseable."""
    match = _VERSION_RE.match(version or "")
    if match is None:
        return None
    parts = [int(part) for part in match.group("core").split(".")]
    label = match.group("label") or ""
    if len(parts) == 3:
        major, minor, _ = parts
        game = f"1.{major}" if minor == 0 else f"1.{major}.{minor}"
    else:
        major, minor, patch, _ = parts
        game = f"{major}.{minor}" if patch == 0 else f"{major}.{minor}.{patch}"
    return game, label


def _normalise(label: str) -> str:
    """``-alpha.1+snapshot-1`` and ``-alpha`` are the same channel to the catalog."""
    if label.startswith("-alpha"):
        return "-alpha"
    if label.startswith("-beta"):
        return "-beta"
    return label


class NeoForgeProvider(Provider):
    id = "neoforge"

    def observe(self, source: dict[str, Any]) -> ProviderResult:
        """The newest NeoForge version per (game version, branch), in publish order."""
        watch = source.get("watch") or {}
        base_url = str(source["baseUrl"]).rstrip("/")
        metadata_url = base_url + str(watch["metadataPath"])
        versions = readiness.fetch_xml_versions(metadata_url)

        enabled = channels.enabled_channels(watch)
        declared = channels.declared_labels(watch)
        by_label = {channels.channel_label(channel, key): channel["branch"] for key, channel in enabled.items()}

        component = str(watch["component"])
        checkpoint = source.get("checkpoint")
        now = observations.utcnow()
        window = int(watch.get("window") or 12)

        # Publish order, so "the newest" is "the last one listed" and never a comparison.
        order: list[str] = []
        latest: dict[tuple[str, str], str] = {}
        reviews: list[tuple[str, str]] = []
        for version in versions:
            parsed = game_version_of(version)
            if parsed is None:
                reviews.append((channels.UNKNOWN_BRANCH, version))
                continue
            game_version, label = parsed[0], _normalise(parsed[1])
            if label not in declared:
                reviews.append((channels.branch_name(label), version))
                continue
            # Last occurrence wins: NeoForge does publish for an older Minecraft line
            # after moving on, and the window is "the game versions most recently built
            # for", not "the ones that appeared last in a decade of history".
            if game_version in order:
                order.remove(game_version)
            order.append(game_version)
            branch = by_label.get(label)
            if branch is not None:
                latest[(game_version, branch)] = version

        window_versions = order[-window:]
        seen: list[Observation] = []
        for (game_version, branch), version in latest.items():
            if game_version not in window_versions:
                continue
            seen.append(
                self._framework(
                    watch, base_url, component, branch, game_version, version, metadata_url, checkpoint, now
                )
            )
        for branch, version in reviews:
            seen.append(self._review(watch, base_url, component, branch, version, metadata_url, checkpoint, now))

        heads: dict[str, str] = {}
        for branch in sorted({branch for _, branch in latest}):
            for game_version in reversed(window_versions):
                if (game_version, branch) in latest:
                    heads[branch] = latest[(game_version, branch)]
                    break

        result = ProviderResult(
            source_id=str(source.get("id") or self.id),
            status="ok",
            heads=heads,
            observations=seen,
            history="full",
        )
        readiness.registry().record(watch=watch, result=result)
        return result

    # -- observation shapes ---------------------------------------------------
    @staticmethod
    def _artifact(watch: dict[str, Any], base_url: str, version: str) -> dict[str, Any]:
        path = str(watch["installerPath"]).format(version=version)
        return readiness.artifact(f"neoforge-{version}-installer.jar", base_url + path)

    def _framework(
        self,
        watch: dict[str, Any],
        base_url: str,
        component: str,
        branch: str,
        game_version: str,
        version: str,
        metadata_url: str,
        checkpoint: dict[str, Any] | None,
        now: str,
    ) -> Observation:
        rollback_from = channels.head_event(
            checkpoint, (game_version, branch), version, _revs_of(game_version, branch, watch)
        )
        rev = channels.rollback_rev(version, rollback_from) if rollback_from else version
        facts: dict[str, Any] = {
            "gameVersion": game_version,
            # NeoForge publishes no per-version timestamp, so first-seen it is.
            "releaseTime": channels.first_seen(checkpoint, rev, now),
            "artifact": self._artifact(watch, base_url, version),
            "listing": {"url": metadata_url},
            "channel": _normalise((game_version_of(version) or ("", ""))[1]),
            "neoforgeVersion": version,
        }
        if rollback_from:
            facts["rollbackFrom"] = rollback_from
        return Observation(
            provider=self.id,
            component=component,
            branch=branch,
            rev=rev,
            kind="framework",
            identity=identity.canonical(self.id, component, branch, rev),
            facts=facts,
            observed_at=now,
        )

    def _review(
        self,
        watch: dict[str, Any],
        base_url: str,
        component: str,
        branch: str,
        version: str,
        metadata_url: str,
        checkpoint: dict[str, Any] | None,
        now: str,
    ) -> Observation:
        parsed = game_version_of(version)
        return Observation(
            provider=self.id,
            component=component,
            branch=branch,
            rev=version,
            kind="branch-review",
            identity=identity.canonical(self.id, component, branch, version),
            facts={
                "gameVersion": parsed[0] if parsed else "",
                "releaseTime": channels.first_seen(checkpoint, version, now),
                "artifact": self._artifact(watch, base_url, version),
                "listing": {"url": metadata_url},
                "channel": parsed[1] if parsed else version,
                "reason": "undeclared-channel" if parsed else "unparseable-version",
            },
            observed_at=now,
        )

    def enrich(self, observation: Observation, source: dict[str, Any]) -> Observation:
        """Add the installer's published sha256 from its ``.sha256`` sidecar."""
        asset = dict(observation.facts.get("artifact") or {})
        url = str(asset.get("url") or "")
        if not url or asset.get("sha256"):
            return observation
        try:
            asset["sha256"] = readiness.fetch_sha256(url + ".sha256")
        except MaintError:
            # The scan is about to mark this whole source failed; a row built from the
            # listing it already recorded would be a claim on a source known to be broken.
            readiness.registry().forget(observation.component)
            raise
        enriched = replace(observation, facts={**observation.facts, "artifact": asset})
        readiness.registry().observed(observation.component, enriched)
        return enriched


def _revs_of(game_version: str, branch: str, watch: dict[str, Any]) -> Callable[[str], bool]:
    """Remembered NeoForge revisions of one ``(game version, branch)``, read back by grammar."""
    labels = channels.declared_labels(watch)

    def predicate(rev: str) -> bool:
        head, _ = channels.split_rollback_rev(rev)
        parsed = game_version_of(head)
        if parsed is None or parsed[0] != game_version:
            return False
        return labels.get(_normalise(parsed[1])) == branch

    return predicate


PROVIDER = NeoForgeProvider()
