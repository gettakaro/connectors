"""GitHub releases, as a provider any game can point at without new Python.

Several of the remaining connectors are built against a framework that ships as a GitHub
release — Carbon for Rust, BepInEx builds, loader jars — and each of them would otherwise
arrive with its own little fetcher. This provider is the shared one: a game declares a
source and a watch block in its catalog record, and gets acquisition (``fetch_input``),
addressing (``input_url``) and discovery (``observe``) from here.

The one thing it refuses to assume is that a tag is a version. Carbon's tags
(``production_build``, ``edge_build``) are *rolling*: the same tag is re-uploaded in place
whenever a build is cut, so a scan that keyed on the tag would file one issue in the
project's lifetime and then go quiet forever. A channel that says ``mutable: true`` folds
the asset's published digest into the revision, which makes every re-upload a new
revision — and says so by reporting ``heads-only`` history, because the uploads between
two scans were never seen and are not claimed.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from .. import channels, net, observations, readiness
from ..exit_codes import UpstreamUnavailable, UsageError
from ..tracker import identity
from .base import Observation, Provider, ProviderResult

#: A GitHub asset digest is published as ``sha256:<64 hex>``; the catalog stores plain hex.
_DIGEST_RE = re.compile(r"^sha256:(?P<hex>[0-9a-f]{64})$")

OBSERVATION_LIMIT = (
    "heads-only: this release tag is re-uploaded in place, so only the asset currently "
    "published under it is observed; builds replaced between two scans are not observed "
    "and are not claimed."
)


def _compile(pattern: str, where: str) -> re.Pattern[str]:
    """One catalog-supplied regular expression, or a usage error naming the key it came from.

    Watch blocks are not schema-validated, and ``scan`` isolates only its own errors — so a
    raw ``re.error`` here would abort the entire run instead of failing this one source.
    """
    try:
        return re.compile(pattern)
    except re.error as exc:
        raise UsageError(f"{where} is not a valid regular expression: {pattern!r} ({exc})") from exc


def _selector(pattern: str, where: str) -> re.Pattern[str]:
    """``regex:<pattern>`` or an exact string, the grammar ``build.references`` already uses."""
    if pattern.startswith("regex:"):
        return _compile(pattern[len("regex:") :], where)
    return re.compile(f"^{re.escape(pattern)}$")


def _digest(asset: dict[str, Any]) -> str | None:
    match = _DIGEST_RE.match(str(asset.get("digest") or ""))
    return match.group("hex") if match else None


class GitHubReleaseProvider(Provider):
    id = "github-release"

    # -- acquisition -----------------------------------------------------------
    def _asset_url(self, input_spec: dict[str, Any], source: dict[str, Any]) -> str:
        """The asset-id URL when the input records one, else the tag's download URL.

        An input with ``assetId`` names an API source (``https://api.github.com``): GitHub
        never re-points an asset id, so a re-upload of the tag makes this URL 404 rather
        than serve other bytes. ``net`` adds the ``Accept: application/octet-stream`` header
        such a URL needs to answer with the file instead of its JSON description.
        """
        base = str(source["baseUrl"]).rstrip("/")
        if input_spec.get("assetId") is not None:
            return f"{base}/repos/{input_spec['repo']}/releases/assets/{int(input_spec['assetId'])}"
        return f"{base}/{input_spec['repo']}/releases/download/{input_spec['tag']}/{input_spec['asset']}"

    def _source_url(self, input_spec: dict[str, Any], source: dict[str, Any]) -> str:
        base = str(source["baseUrl"]).rstrip("/")
        return f"{base}/{input_spec['repo']}/archive/{input_spec['commit']}.{input_spec['archive']}"

    def fetch_input(
        self,
        input_spec: dict[str, Any],
        source: dict[str, Any],
        dest: Path,
        cache: Path,
    ) -> Path:
        if input_spec.get("kind") != "github-release-asset":
            return super().fetch_input(input_spec, source, dest, cache)
        url = self._asset_url(input_spec, source)
        expect = net.Expectation(sha256=input_spec.get("sha256"), size=input_spec.get("size"))
        blob = net.download(url, expect, cache)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(blob.read_bytes())
        return dest

    def input_url(self, input_spec: dict[str, Any], source: dict[str, Any]) -> str | None:
        """The URL an input names, including the source archive nothing fetches yet.

        A ``github-source`` input is a build input whose acquisition belongs to the game
        that needs it; naming it here is still worth doing, because ``targets resolve``,
        the compat record and the install ledger all want one string that identifies the
        bytes, and a commit archive has a perfectly good one.
        """
        kind = input_spec.get("kind")
        if kind == "github-release-asset":
            return str(input_spec.get("resolvedCoordinate") or self._asset_url(input_spec, source))
        if kind == "github-source":
            return self._source_url(input_spec, source)
        return None

    # -- discovery -------------------------------------------------------------
    def _listing(self, source: dict[str, Any], watch: dict[str, Any]) -> tuple[list[dict[str, Any]], str]:
        base = str(source["baseUrl"]).rstrip("/")
        window = int(watch.get("window") or 10)
        url = f"{base}/repos/{watch['repo']}/releases?per_page={window}"
        listed = readiness.fetch_json(url, what="the release listing")
        if not isinstance(listed, list):
            raise UpstreamUnavailable(f"{url}: the release listing is not a list", url=url)
        return [entry for entry in listed if isinstance(entry, dict) and not entry.get("draft")], url

    def _matches(self, channel: dict[str, Any], release: dict[str, Any], key: str) -> bool:
        tag = str(release.get("tag_name") or "")
        wanted = channel.get("tag")
        if wanted is not None and not _selector(str(wanted), f"watch.channels.{key}.tag").match(tag):
            return False
        prerelease = channel.get("prerelease")
        return not (prerelease is not None and bool(release.get("prerelease")) is not bool(prerelease))

    def _asset(self, channel: dict[str, Any], release: dict[str, Any], key: str) -> dict[str, Any]:
        """The one asset a channel means, or a failure that lists what it could have meant.

        A release carries several files — Carbon publishes a Release, a Minimal and a Debug
        build plus a ``.info`` sidecar for each — so the pattern is required and is always a
        regular expression over the asset names. Matching two is an error rather than "take
        the first": which of two builds a connector links against is not a detail to guess.
        """
        pattern = channel.get("asset")
        if not pattern:
            raise UsageError(f"watch.channels.{key} declares no 'asset' pattern; a release carries several files")
        selector = _compile(str(pattern), f"watch.channels.{key}.asset")
        found = [asset for asset in release.get("assets") or [] if selector.match(str(asset.get("name") or ""))]
        if len(found) != 1:
            names = ", ".join(sorted(str(asset.get("name")) for asset in release.get("assets") or [])) or "<none>"
            raise UpstreamUnavailable(
                f"release '{release.get('tag_name')}' has {len(found)} assets matching "
                f"watch.channels.{key}.asset = {pattern!r}; exactly one is required. Assets: {names}"
            )
        return found[0]

    def _rev(self, channel: dict[str, Any], release: dict[str, Any], asset: dict[str, Any], key: str) -> str:
        """The revision: a version out of the release name when there is one, else the tag.

        A mutable tag gets the asset digest appended, so re-uploading ``production_build``
        is a new revision rather than a silent replacement of the old one. Without a digest
        the fallback used to be the upload *date*, so two re-uploads on one day were one
        revision and the second one was never filed; the timestamp and the asset size are
        what tell them apart.

        Everything here comes out of a tag or a release name, which GitHub lets carry
        characters the observation schema's ``rev`` does not -- ``carbon@2.0`` is a valid
        tag -- so the result goes through :func:`observations.safe_rev`. The raw tag stays
        in the facts.
        """
        tag = str(release.get("tag_name") or "")
        rev = tag
        pattern = channel.get("versionPattern")
        if pattern:
            version = _compile(str(pattern), f"watch.channels.{key}.versionPattern")
            match = version.search(str(release.get("name") or ""))
            if match and match.groups():
                rev = match.group(1)
        if channel.get("mutable"):
            digest = _digest(asset)
            if digest:
                rev = f"{rev}.{digest[:8]}"
            else:
                stamp = re.sub(r"[^0-9TZ]", "", str(asset.get("updated_at") or ""))
                size = asset.get("size")
                rev = f"{rev}.{stamp}" + (f".{size}" if size else "")
        return observations.safe_rev(rev)

    def observe(self, source: dict[str, Any]) -> ProviderResult:
        """One observation per (enabled channel, matching release) the listing carries."""
        watch = source.get("watch") or {}
        for key in ("component", "repo"):
            if not watch.get(key):
                raise UsageError(f"the '{self.id}' watch block of source '{source.get('id')}' declares no {key!r}")

        releases, listing_url = self._listing(source, watch)
        enabled = channels.enabled_channels(watch)
        component = str(watch["component"])
        kind = str(watch.get("kind") or "framework")
        checkpoint = source.get("checkpoint")
        now = observations.utcnow()

        seen: list[Observation] = []
        heads: dict[str, str] = {}
        mutable = False
        for key, channel in sorted(enabled.items()):
            branch = str(channel["branch"])
            for release in releases:
                if not self._matches(channel, release, key):
                    continue
                asset = self._asset(channel, release, key)
                rev = self._rev(channel, release, asset, key)
                mutable = mutable or bool(channel.get("mutable"))
                published = str(release.get("published_at") or "")
                facts: dict[str, Any] = {
                    "tag": str(release.get("tag_name") or ""),
                    "releaseName": str(release.get("name") or ""),
                    "prerelease": bool(release.get("prerelease")),
                    "publishedAt": published,
                    "updatedAt": str(asset.get("updated_at") or ""),
                    "artifact": readiness.artifact(
                        str(asset.get("name") or ""),
                        str(asset.get("browser_download_url") or ""),
                        sha256=_digest(asset),
                        size=int(asset["size"]) if asset.get("size") is not None else None,
                    ),
                    "listing": {"url": listing_url},
                    "channel": key,
                    "mutable": bool(channel.get("mutable")),
                    "releaseTime": (str(asset.get("updated_at") or published) if channel.get("mutable") else published)
                    or channels.first_seen(checkpoint, rev, now),
                }
                if channel.get("mutable"):
                    facts["observationLimit"] = OBSERVATION_LIMIT
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
            history="heads-only" if mutable else "full",
        )
        if kind == "framework":
            readiness.registry().record(watch=watch, result=result)
        return result


PROVIDER = GitHubReleaseProvider()
