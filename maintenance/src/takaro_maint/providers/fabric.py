"""FabricMC: the server launcher jar and the Maven artifacts a target builds against.

Observation answers one question — can a Fabric mod be built for this game version? — and
answers it from two documents: ``meta.fabricmc.net`` says which game versions Fabric knows
at all, and the ``fabric-api`` Maven metadata says whether an API jar exists for one. Both
are required: the game listing alone means Fabric parsed the version, not that anything is
publishable against it.

The loader listing is fetched but deliberately unused for readiness. It is identical for
every game version (253 loaders, the same 253 whatever you ask about), so treating it as
evidence would mark every version ready forever. It is recorded as a fact and its failure
is a warning, never a failed source.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import replace
from pathlib import Path
from typing import Any

from .. import channels, net, observations, output, readiness
from ..exit_codes import MaintError, UpstreamUnavailable
from ..tracker import identity
from .base import Observation, Provider, ProviderResult


def _bucket_of(game_version: str) -> str:
    """The fabric-api bucket a game version builds against.

    Mojang preview ids share the release's bucket — ``26.3-rc-3``, ``26.3-pre-1`` and
    ``26.3`` all build against ``…+26.3`` — so everything before the first dash names it.
    """
    return game_version.partition("-")[0]


def _buckets(versions: list[str]) -> dict[str, list[str]]:
    """Maven versions grouped by the game version after ``+``, in publish order.

    A family has one bucket per game version, not one per line: ``+26.1``, ``+26.1.1`` and
    ``+26.1.2`` are three separate buckets, and only an exact match counts.
    """
    grouped: dict[str, list[str]] = {}
    for version in versions:
        _, plus, bucket = version.partition("+")
        if plus and bucket:
            grouped.setdefault(bucket, []).append(version)
    return grouped


class FabricProvider(Provider):
    id = "fabric"

    def fetch_input(
        self,
        input_spec: dict[str, Any],
        source: dict[str, Any],
        dest: Path,
        cache: Path,
    ) -> Path:
        if input_spec["kind"] not in ("fabric-launcher", "maven-artifact", "http-file"):
            return super().fetch_input(input_spec, source, dest, cache)
        blob = net.download(source["url"], net.Expectation(sha256=input_spec["sha256"]), cache)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(blob.read_bytes())
        return dest

    def _maven_base(self, source: dict[str, Any], watch: dict[str, Any]) -> str:
        """The host the fabric-api listing lives on, resolved through the catalog.

        Fabric splits metadata (``meta.fabricmc.net``) from artifacts
        (``maven.fabricmc.net``), so the watch block names a *second* source id rather than
        a second URL: the catalog stays the only place a host is written down, and a test
        that rewrites every ``baseUrl`` moves this listing too.
        """
        from ..catalog import load

        api = watch.get("api") or {}
        name = str(api.get("source") or "")
        if not name:
            return str(source["baseUrl"]).rstrip("/")
        record = load().game(str(source["game"])).record["sources"][name]
        return str(record["baseUrl"]).rstrip("/")

    def observe(self, source: dict[str, Any]) -> ProviderResult:
        """One observation per (game version, branch) Fabric can actually build for."""
        watch = source.get("watch") or {}
        base_url = str(source["baseUrl"]).rstrip("/")
        api = watch.get("api") or {}
        maven_base = self._maven_base(source, watch)

        game_url = base_url + str(watch["gamePath"])
        listed = readiness.fetch_json(game_url, what="the game version listing")
        if not isinstance(listed, list):
            raise UpstreamUnavailable(f"{game_url}: the game version listing is not a list", url=game_url)

        metadata_url = maven_base + str(api["path"])
        api_versions = readiness.fetch_xml_versions(metadata_url)
        buckets = _buckets(api_versions)
        loader = self._loader(base_url, watch)

        enabled = channels.enabled_channels(watch)
        declared = channels.declared_labels(watch)
        by_label = {channels.channel_label(channel, key): channel["branch"] for key, channel in enabled.items()}

        component = str(watch["component"])
        checkpoint = source.get("checkpoint")
        now = observations.utcnow()
        window = int(watch.get("window") or 40)

        seen: list[Observation] = []
        heads: dict[str, str] = {}
        for entry in listed[:window]:
            if not isinstance(entry, dict) or not entry.get("version"):
                continue
            game_version = str(entry["version"])
            label = f"stable:{str(bool(entry.get('stable'))).lower()}"
            if label not in declared:  # Fabric declares both truth values; kept for symmetry
                continue
            branch = by_label.get(label)
            if branch is None:
                continue
            bucket = buckets.get(_bucket_of(game_version)) or []
            if not bucket:
                continue  # Fabric knows the version but nothing is publishable against it
            head = bucket[-1]  # publish order: the last listed member of a bucket is the head
            rollback_from = channels.head_event(
                checkpoint,
                (game_version, branch),
                head,
                self._revs_of(_bucket_of(game_version)),
            )
            rev = channels.rollback_rev(head, rollback_from) if rollback_from else head
            name = f"fabric-api-{head}.jar"
            url = maven_base + str(api["artifactPath"]).format(version=head)
            facts: dict[str, Any] = {
                "gameVersion": game_version,
                "releaseTime": channels.first_seen(checkpoint, rev, now),
                "artifact": readiness.artifact(name, url),
                "listing": {"url": metadata_url},
                "channel": label,
                "stable": bool(entry.get("stable")),
                "fabricApi": {"version": head, "bucket": list(bucket)},
                "loader": {"version": loader},
            }
            if rollback_from:
                facts["rollbackFrom"] = rollback_from
            seen.append(
                Observation(
                    provider=self.id,
                    component=component,
                    branch=branch,
                    rev=rev,
                    kind="framework",
                    identity=identity.canonical(self.id, component, branch, rev),
                    facts=facts,
                    observed_at=now,
                )
            )
            heads.setdefault(branch, head)

        result = ProviderResult(
            source_id=str(source.get("id") or self.id),
            status="ok",
            heads=heads,
            observations=seen,
            history="full",
        )
        readiness.registry().record(watch=watch, result=result)
        return result

    @staticmethod
    def _revs_of(bucket: str) -> Callable[[str], bool]:
        """ "Does this remembered revision belong to this bucket?" — suffix match, no ordering."""

        def predicate(rev: str) -> bool:
            head, _ = channels.split_rollback_rev(rev)
            return head.partition("+")[2] == bucket

        return predicate

    def _loader(self, base_url: str, watch: dict[str, Any]) -> str | None:
        """The newest stable loader, as an informational fact. Never a readiness input."""
        path = watch.get("loaderPath")
        if not path:
            return None
        url = base_url + str(path)
        try:
            listed = readiness.fetch_json(url, what="the loader listing")
        except MaintError as exc:  # informational only: a missing loader list never fails the source
            output.warn(f"{url}: loader listing unavailable ({exc.message}); readiness does not use it")
            return None
        if not isinstance(listed, list):
            return None
        for entry in listed:
            if isinstance(entry, dict) and entry.get("stable") and entry.get("version"):
                return str(entry["version"])
        return None

    def enrich(self, observation: Observation, source: dict[str, Any]) -> Observation:
        """Add the fabric-api jar's published sha256 from its ``.sha256`` sidecar."""
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


PROVIDER = FabricProvider()
