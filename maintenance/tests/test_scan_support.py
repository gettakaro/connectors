"""Helpers and fixtures the ``scan`` scenarios share.

The scan is exercised through ``cli.main`` against two in-process fakes: a Mojang stand-in
serving the recorded manifest and per-version documents, and a GitHub stand-in whose
issues, request log and write count are inspectable. Nothing here asserts; the scenarios
do. The module is named ``test_scan_support`` so it sits inside this issue's owned test
glob, and it holds one test of its own so the helpers cannot rot unnoticed.
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pytest

from conftest import point_at
from fake_github import FakeGitHub
from fake_upstream import FakeUpstream
from takaro_maint import observations
from takaro_maint.tracker import dashboard, identity

FIXTURES = Path(__file__).parent / "fixtures"
MOJANG = FIXTURES / "providers" / "mojang"
GITHUB_FIXTURES = FIXTURES / "github"

MANIFEST_PATH = "/mc/game/version_manifest_v2.json"
SOURCE_KEY = "minecraft/mojang-meta"
REPO = "gettakaro/connectors"
TOKEN = "token-for-tests"
FROZEN_NOW = "2026-09-17T12:00:00Z"

#: The per-version documents recorded next to the manifest fixture.
RECORDED = ("26.3", "26.2", "26.1.2")


def version_document(revision: str) -> dict[str, Any]:
    return json.loads((MOJANG / f"{revision}.json").read_text(encoding="utf-8"))


def _serve_version(upstream: FakeUpstream, revision: str, payload: bytes) -> tuple[str, str]:
    """Serve one per-version document and return ``(path, sha1)``."""
    digest = hashlib.sha1(payload).hexdigest()
    path = f"/v1/packages/{digest}/{revision}.json"
    upstream.add(path, payload)
    return path, digest


def mojang_upstream(upstream: FakeUpstream, releases: tuple[str, ...] = RECORDED) -> dict[str, str]:
    """Serve the recorded manifest, re-pinned to the trimmed per-version documents.

    Trimming the recorded documents changed their bytes, so each served entry's ``url``
    path and ``sha1`` are recomputed here — which is exactly what keeps the provider's
    integrity check honest under test.
    """
    manifest = json.loads((MOJANG / "version_manifest_v2.json").read_text(encoding="utf-8"))
    served: dict[str, str] = {}
    for entry in manifest["versions"]:
        revision = str(entry["id"])
        if revision not in releases:
            continue
        path, digest = _serve_version(upstream, revision, (MOJANG / f"{revision}.json").read_bytes())
        entry["url"] = upstream.base_url + path
        entry["sha1"] = digest
        served[revision] = path
    upstream.add(MANIFEST_PATH, json.dumps(manifest).encode("utf-8"))
    return served


def add_release(
    upstream: FakeUpstream,
    revision: str,
    *,
    release_time: str,
    based_on: str = "26.3",
) -> str:
    """Publish one more stable release on the fake upstream and make it the head."""
    manifest = json.loads(upstream.files[MANIFEST_PATH])
    document = version_document(based_on)
    document["id"] = revision
    document["time"] = release_time
    document["releaseTime"] = release_time
    path, digest = _serve_version(upstream, revision, json.dumps(document, indent=1).encode("utf-8"))
    manifest["versions"].insert(
        0,
        {
            "id": revision,
            "type": "release",
            "url": upstream.base_url + path,
            "time": release_time,
            "releaseTime": release_time,
            "sha1": digest,
            "complianceLevel": 1,
        },
    )
    manifest["latest"] = {"release": revision, "snapshot": revision}
    upstream.add(MANIFEST_PATH, json.dumps(manifest).encode("utf-8"))
    return path


def scan_repo(catalog_copy: Path, upstream: FakeUpstream) -> Path:
    """Point the copied catalog at the fake upstream.

    Most legacy scan scenarios serve only the recorded Mojang manifest. Keep framework
    watches enabled only when the test upstream actually serves their listing endpoint;
    otherwise those unrelated Fabric/Paper/NeoForge sources would 404 against the
    Mojang-only fixture. Framework readiness tests populate those endpoints first, so
    they still exercise the full multi-source path.
    """
    point_at(catalog_copy, upstream.base_url)
    game_file = catalog_copy / "catalog/minecraft/game.json"
    game = json.loads(game_file.read_text(encoding="utf-8"))
    served_paths = set(getattr(upstream, "files", {}))
    for source in game["sources"].values():
        watch = source.get("watch")
        if not watch or watch.get("kind") != "framework":
            continue
        sentinel = watch.get("projectPath") or watch.get("metadataPath") or watch.get("gamePath")
        if sentinel not in served_paths:
            source.pop("watch", None)
    game_file.write_text(json.dumps(game, indent=2) + "\n", encoding="utf-8")
    return catalog_copy


def frozen_clock(monkeypatch: pytest.MonkeyPatch, at: str = FROZEN_NOW) -> str:
    """Pin the one clock observations and checkpoints read."""
    monkeypatch.setattr(observations, "utcnow", lambda: at)
    return at


@dataclass
class Rig:
    """A catalog copy wired to a fake Mojang and a fake tracker."""

    root: Path
    upstream: FakeUpstream
    fake: FakeGitHub
    served: dict[str, str] = field(default_factory=dict)

    def scan(self, run: Any, *flags: str) -> tuple[int, Any, str]:
        return run("scan", "--repo", REPO, "--api-url", self.fake.api_url, *flags, repo=self.root)

    # -- inspecting the tracker ----------------------------------------------
    def markers(self) -> list[dict[str, str]]:
        parsed = [identity.parse_marker(str(issue.get("body") or "")) for issue in self.fake.issues]
        return [fields for fields in parsed if fields]

    def support_issues(self) -> list[dict[str, Any]]:
        return [
            issue
            for issue in self.fake.issues
            if (identity.parse_marker(str(issue.get("body") or "")) or {}).get("kind") == "support"
        ]

    def dashboard_issue(self) -> dict[str, Any] | None:
        for issue in self.fake.issues:
            if identity.parse_marker(str(issue.get("body") or "")) == identity.DASHBOARD_MARKER:
                return issue
        return None

    def dashboard_state(self) -> dict[str, Any]:
        issue = self.dashboard_issue()
        assert issue is not None, "the tracker holds no dashboard issue"
        body = str(issue["body"])
        block = body.partition(dashboard.BEGIN)[2].partition(dashboard.END)[0]
        return json.loads(block.partition("```json\n")[2].partition("\n```")[0])  # type: ignore[no-any-return]

    def checkpoint_ids(self, source_key: str = SOURCE_KEY) -> list[str]:
        checkpoint = self.dashboard_state()["sources"][source_key]["checkpoint"]
        return [str(entry[0]) for entry in checkpoint["seen"]]

    def seed_issue(self, body: str, *, title: str = "seeded", **fields: Any) -> dict[str, Any]:
        issue = {
            "number": len(self.fake.issues) + 1,
            "state": "open",
            "title": title,
            "body": body,
            **fields,
        }
        self.fake.issues.append(issue)
        return issue

    def forget(self, revision: str, source_key: str = SOURCE_KEY) -> None:
        """Drop one revision from the stored checkpoint, as an interrupted run would leave it.

        The issue stays in the tracker while the dashboard no longer knows about it, which
        is exactly the state a duplicate would be created from.
        """
        issue = self.dashboard_issue()
        assert issue is not None
        head, fence, rest = str(issue["body"]).partition("```json\n")
        raw, end, tail = rest.partition("\n```")
        state = json.loads(raw)
        checkpoint = state["sources"][source_key]["checkpoint"]
        checkpoint["seen"] = [entry for entry in checkpoint["seen"] if entry[0] != revision]
        issue["body"] = head + fence + json.dumps(state, sort_keys=True, indent=1) + end + tail

    def requested(self, path: str) -> int:
        return sum(1 for asked in self.upstream.requested if asked == path)

    def listing_requests(self) -> list[str]:
        return [
            path
            for method, path in self.fake.requests
            if method == "GET" and "/issues?" in path and not path.startswith("/search/")
        ]


@contextmanager
def rig(
    catalog_copy: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    releases: tuple[str, ...] = RECORDED,
) -> Iterator[Rig]:
    """The whole rig: fake upstream, fake tracker, a token in the environment."""
    with FakeUpstream() as upstream, FakeGitHub() as fake:
        served = mojang_upstream(upstream, releases)
        scan_repo(catalog_copy, upstream)
        monkeypatch.setenv("GH_TOKEN", TOKEN)
        yield Rig(root=catalog_copy, upstream=upstream, fake=fake, served=served)


def test_the_rig_serves_the_recorded_manifest(catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The helpers themselves: the manifest is served with recomputed, verifiable hashes."""
    with rig(catalog_copy, monkeypatch) as harness:
        manifest = json.loads(harness.upstream.files[MANIFEST_PATH])
        releases = [entry for entry in manifest["versions"] if entry["type"] == "release"]

        assert manifest["latest"]["release"] == "26.3"
        assert [entry["id"] for entry in releases] == ["26.3", "26.2", "26.1.2", "26.1.1", "26.1", "1.21.11"]
        for revision, path in harness.served.items():
            entry = next(item for item in releases if item["id"] == revision)
            assert entry["url"].endswith(path)
            assert entry["sha1"] == hashlib.sha1(harness.upstream.files[path]).hexdigest()


# -- golden renderings --------------------------------------------------------
# The two golden files are pinned to these literals rather than to the live catalog, so a
# new target record can never silently rewrite what a maintenance issue says, and merging
# another catalog change does not move this issue's expectations.
GOLDEN_FACTS: dict[str, Any] = {
    "id": "26.3",
    "type": "release",
    "releaseTime": "2026-09-15T11:23:02+00:00",
    "manifestList": {"url": "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"},
    "manifest": {
        "url": "https://piston-meta.mojang.com/v1/packages/96c00d95a31328714d3811cfade2804bb050e455/26.3.json",
        "sha1": "96c00d95a31328714d3811cfade2804bb050e455",
    },
    "server": {
        "url": "https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar",
        "sha1": "33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c",
        "size": 62294556,
    },
    "javaMajor": 25,
}


def golden_observation() -> Any:
    from takaro_maint.providers.base import Observation

    return Observation(
        provider="mojang",
        component="minecraft",
        branch="release",
        rev="26.3",
        kind="game",
        identity=identity.canonical("mojang", "minecraft", "release", "26.3"),
        facts=GOLDEN_FACTS,
        observed_at=FROZEN_NOW,
    )


def golden_targets() -> Any:
    from takaro_maint.tracker import issues

    return issues.GameTargets(
        name="Minecraft",
        platforms=["fabric", "paper", "neoforge"],
        rows=[issues.TargetRow(platform="fabric", id="fabric-26.2", revision="26.2", status="maintained")],
    )


def golden_dashboard() -> Any:
    """A dashboard holding one bootstrapped source and one filed piece of work."""
    work_identity = identity.canonical("mojang", "minecraft", "release", "26.3")
    board = dashboard.Dashboard.empty()
    board.apply_source(
        SOURCE_KEY,
        status="ok",
        history="full",
        heads={"release": "26.3"},
        checkpoint=None,
        last_error=None,
        at=FROZEN_NOW,
    )
    board.source(SOURCE_KEY)["checkpoint"] = {
        "seen": [["26.3", "2026-09-15T11:23:02+00:00"], ["26.2", "2026-06-16T12:03:33+00:00"]],
        "floor": None,
        "at": FROZEN_NOW,
    }
    board.set_work(work_identity, issue=187, state="detected", at=FROZEN_NOW)
    board.set_targets(
        "minecraft",
        [{"id": "fabric-26.2", "platform": "fabric", "revision": "26.2", "status": "maintained"}],
    )
    board.data["updatedAt"] = FROZEN_NOW
    board.data["lastSuccess"] = FROZEN_NOW
    return board


def catalog_target_rows(root: Path, game: str = "minecraft") -> list[dict[str, Any]]:
    """The dashboard's target rows as the catalog tree on disk spells them.

    Read straight from the records rather than through the loader, so the expectation stays
    independent of the code under test, and so a scenario can say "the dashboard mirrors the
    catalog" without naming the records -- adding a target never edits the assertion.
    """
    rows = [
        {
            "id": str(record["id"]),
            "platform": str(record["platform"]),
            "revision": str(record["revision"]),
            "status": str(record["support"]["status"]),
        }
        for record in (
            json.loads(path.read_text(encoding="utf-8"))
            for path in (root / "catalog" / game / "targets").glob("*.json")
        )
        if record["support"]["status"] != "retired"
    ]
    return sorted(rows, key=lambda row: row["id"])


def add_candidate_target(root: Path, revision: str, *, platform: str = "fabric") -> Path:
    """Clone the maintained Fabric record into a candidate target for ``revision``."""
    from conftest import read_target, write_target

    record = read_target(root)
    record["id"] = f"{platform}-{revision}"
    record["platform"] = platform
    record["revision"] = revision
    record["default"] = False
    record["support"] = {"status": "candidate", "since": "2026-09-17", "evidence": [], "notes": "test fixture"}
    return write_target(root, record, f"{platform}-{revision}")


SECOND_MANIFEST_PATH = "/mc/game/version_manifest_v2b.json"


def add_second_watch_source(
    root: Path,
    upstream: FakeUpstream,
    *,
    source_id: str = "mojang-meta-2",
    manifest_path: str = SECOND_MANIFEST_PATH,
) -> str:
    """A second watched source on its own manifest path, so one can fail alone."""
    game_file = root / "catalog/minecraft/game.json"
    game = json.loads(game_file.read_text(encoding="utf-8"))
    original = game["sources"]["mojang-meta"]
    game["sources"][source_id] = {
        "provider": original["provider"],
        "baseUrl": original["baseUrl"],
        "watch": {**original["watch"], "manifestPath": manifest_path},
    }
    game_file.write_text(json.dumps(game, indent=2) + "\n", encoding="utf-8")
    mirror_manifest(upstream, manifest_path)
    return f"minecraft/{source_id}"


def mirror_manifest(upstream: FakeUpstream, path: str = SECOND_MANIFEST_PATH) -> None:
    """Serve the current manifest bytes at a second path as well."""
    upstream.add(path, upstream.files[MANIFEST_PATH])


def drop_watch(root: Path) -> None:
    """Remove every watch block, leaving a game the scan has nothing to do with."""
    game_file = root / "catalog/minecraft/game.json"
    game = json.loads(game_file.read_text(encoding="utf-8"))
    for source in game["sources"].values():
        source.pop("watch", None)
    game_file.write_text(json.dumps(game, indent=2) + "\n", encoding="utf-8")
