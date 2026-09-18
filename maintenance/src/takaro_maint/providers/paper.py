"""PaperMC: the server jar for one build, and the Fill v3 listings that say a build exists.

*Acquiring.* Fill v3 serves every build by a content-addressed URL whose path segment *is*
the jar's sha256, so the pin and the address are the same fact. Acquisition consults no
build listing: the build number was resolved once, at record time, and a listing lookup
would silently follow upstream to a different jar.

*Observing.* Readiness for Paper is a *build*, not a version: ``/v3/projects/paper`` lists
the game versions Paper knows, and only ``/v3/projects/paper/versions/<v>/builds`` says
whether any of them has a build and on which channel (``STABLE``, ``BETA``, ``ALPHA``). The
per-version endpoint returns build ids without a channel or a time, which is not enough to
choose one, so it is never read.

PaperMC's download policy requires every request to carry a User-Agent that identifies the
software and gives a contact URL. Nothing here sets one: ``net`` already sends
``takaro-connectors-maint/<version> (+https://github.com/gettakaro/connectors)`` on every
request, and a second constant would be a second thing to forget to update.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .. import channels, net, observations, readiness
from ..exit_codes import UpstreamUnavailable, UsageError
from ..tracker import identity
from .base import Observation, Provider, ProviderResult

_OBJECT_PATH = re.compile(r"^/v1/objects/(?P<sha256>[0-9a-f]{64})/")


def _versions(document: Any, url: str) -> list[str]:
    """Every listed game version, families newest first and versions newest first inside one."""
    families = (document or {}).get("versions") if isinstance(document, dict) else None
    if not isinstance(families, dict):
        raise UpstreamUnavailable(f"{url}: the project listing has no 'versions' mapping", url=url)
    flattened: list[str] = []
    for versions in families.values():
        if not isinstance(versions, list):
            raise UpstreamUnavailable(f"{url}: a version family is not a list", url=url)
        flattened += [str(version) for version in versions]
    return flattened


class PaperProvider(Provider):
    id = "paper"

    def fetch_input(
        self,
        input_spec: dict[str, Any],
        source: dict[str, Any],
        dest: Path,
        cache: Path,
    ) -> Path:
        if input_spec["kind"] != "paper-build":
            return super().fetch_input(input_spec, source, dest, cache)
        # The URL carries the hash. A record whose two halves disagree is a broken record,
        # not a broken download, so it is refused before anything is requested.
        match = _OBJECT_PATH.match(str(input_spec["path"]))
        addressed = match.group("sha256") if match else None
        if addressed != input_spec["sha256"]:
            raise UsageError(
                f"the Paper input contradicts itself: the content-addressed path names {addressed} "
                f"but sha256 is {input_spec['sha256']}; nothing was downloaded",
                path=input_spec["path"],
                sha256=input_spec["sha256"],
            )
        blob = net.download(
            source["url"],
            net.Expectation(sha256=input_spec["sha256"], size=input_spec.get("size")),
            cache,
        )
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(blob.read_bytes())
        return dest

    def observe(self, source: dict[str, Any]) -> ProviderResult:
        """One observation per (game version, declared channel), newest build wins."""
        watch = source.get("watch") or {}
        base_url = str(source["baseUrl"]).rstrip("/")
        project_url = base_url + str(watch["projectPath"])
        versions = _versions(readiness.fetch_json(project_url, what="the project listing"), project_url)

        enabled = channels.enabled_channels(watch)
        declared = channels.declared_labels(watch)
        by_label = {channels.channel_label(channel, key): channel["branch"] for key, channel in enabled.items()}

        component = str(watch["component"])
        checkpoint = source.get("checkpoint")
        now = observations.utcnow()
        window = int(watch.get("window") or 8)

        seen: list[Observation] = []
        heads: dict[str, str] = {}
        for game_version in versions[:window]:
            builds_url = base_url + str(watch["buildsPath"]).format(version=game_version)
            builds = readiness.fetch_json(builds_url, what="the build listing")
            if not isinstance(builds, list):
                raise UpstreamUnavailable(f"{builds_url}: the build listing is not a list", url=builds_url)

            newest: dict[str, dict[str, Any]] = {}
            channel_of: dict[str, str] = {}
            for build in builds:  # listing order is newest first; the first of a label is its head
                if not isinstance(build, dict) or build.get("id") is None:
                    continue
                label = str(build.get("channel") or "")
                # Upstream schema drift is this source failing, not this process crashing:
                # an unexpected type here must still leave the other sources to finish.
                try:
                    channel_of.setdefault(str(int(build["id"])), label)
                except (TypeError, ValueError) as exc:
                    raise UpstreamUnavailable(
                        f"{builds_url}: build id {build['id']!r} is not a number", url=builds_url
                    ) from exc
                newest.setdefault(label, build)

            for label, build in newest.items():
                branch = by_label.get(label)
                if branch is None:
                    if label in declared:
                        continue  # declared and switched off: known, deliberately not watched
                    seen.append(self._review(component, label, game_version, build, builds_url, checkpoint, now))
                    continue
                observation = self._framework(
                    component, branch, label, game_version, build, builds_url, checkpoint, now, channel_of, by_label
                )
                seen.append(observation)
                heads.setdefault(branch, channels.split_rollback_rev(observation.rev)[0])

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
    def _download(build: dict[str, Any]) -> dict[str, Any]:
        download = ((build.get("downloads") or {}).get("server:default")) or {}
        return {
            "name": str(download.get("name") or ""),
            "url": str(download.get("url") or ""),
            "sha256": str(((download.get("checksums") or {}).get("sha256")) or "") or None,
            "size": int(download["size"]) if download.get("size") is not None else None,
        }

    def _framework(
        self,
        component: str,
        branch: str,
        label: str,
        game_version: str,
        build: dict[str, Any],
        builds_url: str,
        checkpoint: dict[str, Any] | None,
        now: str,
        channel_of: dict[str, str],
        by_label: dict[str, str],
    ) -> Observation:
        head = f"{game_version}-{build['id']}"
        rollback_from = channels.head_event(
            checkpoint, (game_version, branch), head, _revs_of(game_version, branch, channel_of, by_label)
        )
        rev = channels.rollback_rev(head, rollback_from) if rollback_from else head
        download = self._download(build)
        facts: dict[str, Any] = {
            "gameVersion": game_version,
            # Paper is the one framework here that timestamps its builds, so this is a real
            # upstream time rather than a first-seen stand-in.
            "releaseTime": str(build.get("time") or channels.first_seen(checkpoint, rev, now)),
            "artifact": readiness.artifact(
                download["name"], download["url"], sha256=download["sha256"], size=download["size"]
            ),
            "listing": {"url": builds_url},
            "channel": label,
            "build": int(build["id"]),
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
        component: str,
        label: str,
        game_version: str,
        build: dict[str, Any],
        builds_url: str,
        checkpoint: dict[str, Any] | None,
        now: str,
    ) -> Observation:
        branch = channels.branch_name(label)
        rev = f"{game_version}-{build['id']}"
        download = self._download(build)
        return Observation(
            provider=self.id,
            component=component,
            branch=branch,
            rev=rev,
            kind="branch-review",
            identity=identity.canonical(self.id, component, branch, rev),
            facts={
                "gameVersion": game_version,
                "releaseTime": str(build.get("time") or channels.first_seen(checkpoint, rev, now)),
                "artifact": readiness.artifact(
                    download["name"], download["url"], sha256=download["sha256"], size=download["size"]
                ),
                "listing": {"url": builds_url},
                "channel": label,
                "build": int(build["id"]),
                "reason": "undeclared-channel",
            },
            observed_at=now,
        )


def _revs_of(
    game_version: str,
    branch: str,
    channel_of: dict[str, str],
    by_label: dict[str, str],
) -> Callable[[str], bool]:
    """Remembered Paper revisions of one ``(game version, branch)``.

    A revision is ``<version>-<build>`` and does not carry its channel, so the channel is
    read back out of the *current* listing: a build still published on another channel
    belongs to that channel's key, not this one. Without that, a new stable build would
    make an unchanged alpha head look as though it had rolled back.

    A build the listing no longer mentions has no channel to read — and a withdrawn build
    is exactly what a rollback rolls back *from*, so it counts for every branch of its game
    version. With one channel enabled (the shipped configuration) that is exact; with
    several it can only ever widen the key, never narrow it.
    """

    def predicate(rev: str) -> bool:
        head, _ = channels.split_rollback_rev(rev)
        version, _, build = head.rpartition("-")
        if not build or version != game_version:
            return False
        label = channel_of.get(build)
        return label is None or by_label.get(label) == branch

    return predicate


PROVIDER = PaperProvider()
