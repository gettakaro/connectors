"""A plain Maven repository, as a provider any game can point at without new Python.

Some game servers are published as one jar in a vendor's own Maven repository rather than
through Steam or a modding framework. This provider is the shared one for them: a game
declares a source whose ``baseUrl`` is the repository root and gets acquisition
(``fetch_input`` for ``maven-artifact`` inputs) and discovery (``observe``) from here.

Discovery reads one ``maven-metadata.xml`` per channel. Every ``<version>`` in it is kept,
in publish order, and the last one is the head; the ``<latest>`` and ``<release>`` pointers
are not trusted (see ``readiness.fetch_xml_versions``). A repository that drops old
versions from its metadata loses them from the listing too, so the history is
``heads-only``: a scan can say what the head is, never that it saw every version since.

A channel may live in a different repository on the same host (a ``pre-release``
repository beside ``release``), so each channel can name its own ``metadataPath``;
otherwise the watch block's ``metadataPath`` is used.
"""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from typing import Any

from .. import channels, net, observations, readiness
from ..exit_codes import MaintError, UpstreamUnavailable, UsageError
from ..tracker import identity
from .base import Observation, Provider, ProviderResult

OBSERVATION_LIMIT = (
    "heads-only: maven-metadata.xml lists the versions the repository still holds, so a "
    "version removed between two scans is not observed and is not claimed."
)


class MavenProvider(Provider):
    id = "maven"

    # -- acquisition -----------------------------------------------------------
    def fetch_input(
        self,
        input_spec: dict[str, Any],
        source: dict[str, Any],
        dest: Path,
        cache: Path,
    ) -> Path:
        if input_spec.get("kind") not in ("maven-artifact", "http-file"):
            return super().fetch_input(input_spec, source, dest, cache)
        expect = net.Expectation(sha256=input_spec.get("sha256"), size=input_spec.get("size"))
        blob = net.download(source["url"], expect, cache)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(blob.read_bytes())
        return dest

    # -- discovery -------------------------------------------------------------
    def observe(self, source: dict[str, Any]) -> ProviderResult:
        """One observation per enabled channel: the newest version its metadata lists."""
        watch = source.get("watch") or {}
        for key in ("component", "group", "artifact"):
            if not watch.get(key):
                raise UsageError(f"the '{self.id}' watch block of source '{source.get('id')}' declares no {key!r}")
        base = str(source["baseUrl"]).rstrip("/")
        group, artifact = str(watch["group"]), str(watch["artifact"])
        group_path = group.replace(".", "/")
        component = str(watch["component"])
        kind = str(watch.get("kind") or "framework")
        checkpoint = source.get("checkpoint")
        now = observations.utcnow()

        seen: list[Observation] = []
        heads: dict[str, str] = {}
        for key, channel in sorted(channels.enabled_channels(watch).items()):
            metadata_path = str(channel.get("metadataPath") or watch.get("metadataPath") or "")
            if not metadata_path.startswith("/"):
                raise UsageError(
                    f"the '{self.id}' watch block of source '{source.get('id')}' names no metadataPath "
                    f"for channel '{key}'"
                )
            url = base + metadata_path
            versions = readiness.fetch_xml_versions(url)
            if not versions:
                raise UpstreamUnavailable(f"{url}: the metadata lists no versions", url=url)
            rev = versions[-1]
            repository = metadata_path.rsplit("/" + group_path + "/", 1)[0]
            file_name = f"{artifact}-{rev}.jar"
            artifact_url = f"{base}{repository}/{group_path}/{artifact}/{rev}/{file_name}"
            branch = str(channel["branch"])
            facts: dict[str, Any] = {
                "artifact": readiness.artifact(file_name, artifact_url),
                "releaseTime": channels.first_seen(checkpoint, rev, now),
                "coordinate": f"{group}:{artifact}:{rev}",
                "listing": {"url": url, "versions": versions[-10:]},
                "channel": key,
                "observationLimit": OBSERVATION_LIMIT,
            }
            seen.append(
                Observation(
                    provider=self.id,
                    component=component,
                    branch=branch,
                    rev=rev,
                    kind=kind,
                    identity=identity.canonical(self.id, component, branch, rev),
                    facts=facts,
                    observed_at=now,
                )
            )
            heads.setdefault(branch, rev)

        result = ProviderResult(
            source_id=str(source.get("id") or self.id),
            status="ok",
            heads=heads,
            observations=seen,
            history="heads-only",
        )
        if kind == "framework":
            readiness.registry().record(watch=watch, result=result)
        return result

    def enrich(self, observation: Observation, source: dict[str, Any]) -> Observation:
        """Add the jar's published sha256 from its ``.sha256`` sidecar, when the repository has one."""
        del source
        asset = dict(observation.facts.get("artifact") or {})
        url = str(asset.get("url") or "")
        if not url or asset.get("sha256"):
            return observation
        try:
            asset["sha256"] = readiness.fetch_sha256(url + ".sha256")
        except MaintError:
            return observation  # not every repository publishes sidecars; the pin is then self-recorded
        return replace(observation, facts={**observation.facts, "artifact": asset})


PROVIDER = MavenProvider()
