"""Framework readiness, end to end through ``cli.main``: the rig, and the transitions.

The rig is ``test_scan_support.rig`` with the three framework listings served on the same
in-process upstream, at exactly the paths the catalog's watch blocks name. Nothing is
stubbed inside the tool: every scenario runs the real command against real HTTP and a fake
tracker, and asserts what ends up in an issue body and on stdout.

The helpers live here so the two sibling modules (``test_channels_promotion_rollback`` and
``test_partial_provider_failure``) share one rig, and this module holds tests of its own so
the helpers cannot rot unnoticed.
"""

from __future__ import annotations

import hashlib
import http.server
import json
import threading
import xml.etree.ElementTree as ET
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pytest

import test_scan_support as support
from fake_github import FakeGitHub
from fake_upstream import FakeUpstream
from takaro_maint import net, readiness
from takaro_maint.tracker import identity, issues

FIXTURES = Path(__file__).parent / "fixtures" / "providers"
FABRIC = FIXTURES / "fabric"
PAPER = FIXTURES / "paper"
NEOFORGE = FIXTURES / "neoforge"

FABRIC_GAME_PATH = "/v2/versions/game"
FABRIC_LOADER_PATH = "/v2/versions/loader"
FABRIC_API_PATH = "/net/fabricmc/fabric-api/fabric-api/maven-metadata.xml"
PAPER_PROJECT_PATH = "/v3/projects/paper"
NEOFORGE_META_PATH = "/releases/net/neoforged/neoforge/maven-metadata.xml"

FABRIC_KEY = "minecraft/fabric-meta"
PAPER_KEY = "minecraft/paper-fill"
NEOFORGE_KEY = "minecraft/neoforge-maven"


def fabric_api_path(version: str) -> str:
    return f"/net/fabricmc/fabric-api/fabric-api/{version}/fabric-api-{version}.jar"


def paper_builds_path(version: str) -> str:
    return f"/v3/projects/paper/versions/{version}/builds"


def neoforge_installer_path(version: str) -> str:
    return f"/releases/net/neoforged/neoforge/{version}/neoforge-{version}-installer.jar"


def synthetic_sha256(name: str) -> str:
    """A stand-in digest for an artifact the fixtures did not record.

    The recorded sidecars carry the real upstream digests; a version a test invents has no
    real digest to serve, so this makes one that is deterministic and obviously derived.
    """
    return hashlib.sha256(name.encode("utf-8")).hexdigest()


# -- serving the framework listings -------------------------------------------
def _maven_document(group: str, artifact: str, versions: list[str], last_updated: str) -> bytes:
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        "<metadata>",
        f"  <groupId>{group}</groupId>",
        f"  <artifactId>{artifact}</artifactId>",
        "  <versioning>",
        f"    <latest>{versions[-1] if versions else ''}</latest>",
        f"    <release>{versions[-1] if versions else ''}</release>",
        "    <versions>",
        *(f"      <version>{version}</version>" for version in versions),
        "    </versions>",
        f"    <lastUpdated>{last_updated}</lastUpdated>",
        "  </versioning>",
        "</metadata>",
        "",
    ]
    return "\n".join(lines).encode("utf-8")


def maven_versions(payload: bytes) -> list[str]:
    root = ET.fromstring(payload.decode("utf-8"))
    return [(element.text or "") for element in root.findall("versioning/versions/version")]


def _serve_maven(upstream: Any, path: str, group: str, artifact: str, versions: list[str]) -> None:
    upstream.add(path, _maven_document(group, artifact, versions, "20260917081004"))


def frameworks_upstream(
    upstream: Any,
    *,
    fabric: bool = True,
    paper: bool = True,
    neoforge: bool = True,
) -> None:
    """Serve the recorded Fabric, Paper and NeoForge listings at their catalog paths."""
    if fabric:
        upstream.add(FABRIC_GAME_PATH, (FABRIC / "versions_game.json").read_bytes())
        upstream.add(FABRIC_LOADER_PATH, (FABRIC / "versions_loader.json").read_bytes())
        metadata = (FABRIC / "fabric-api-maven-metadata.xml").read_bytes()
        upstream.add(FABRIC_API_PATH, metadata)
        for version in maven_versions(metadata):
            _serve_fabric_sidecar(upstream, version)
    if paper:
        upstream.add(PAPER_PROJECT_PATH, (PAPER / "projects_paper.json").read_bytes())
        for builds_file in sorted((PAPER / "builds").glob("*.json")):
            upstream.add(paper_builds_path(builds_file.stem), builds_file.read_bytes())
    if neoforge:
        metadata = (NEOFORGE / "maven-metadata.xml").read_bytes()
        upstream.add(NEOFORGE_META_PATH, metadata)
        for version in maven_versions(metadata):
            _serve_neoforge_sidecar(upstream, version)


def _serve_fabric_sidecar(upstream: Any, version: str) -> None:
    recorded = FABRIC / "sha256" / f"{version}.sha256"
    digest = recorded.read_text().strip() if recorded.is_file() else synthetic_sha256(f"fabric-api-{version}")
    upstream.add(fabric_api_path(version) + ".sha256", (digest + "\n").encode("utf-8"))


def _serve_neoforge_sidecar(upstream: Any, version: str) -> None:
    recorded = NEOFORGE / "sha256" / f"{version}.sha256"
    digest = recorded.read_text().strip() if recorded.is_file() else synthetic_sha256(f"neoforge-{version}")
    upstream.add(neoforge_installer_path(version) + ".sha256", (digest + "\n").encode("utf-8"))


# -- moving the upstreams ------------------------------------------------------
def add_fabric_api(upstream: Any, version: str) -> str:
    """Publish one more fabric-api build; the last listed member of a bucket is its head."""
    versions = maven_versions(upstream.files[FABRIC_API_PATH])
    versions.append(version)
    _serve_maven(upstream, FABRIC_API_PATH, "net.fabricmc.fabric-api", "fabric-api", versions)
    _serve_fabric_sidecar(upstream, version)
    return version


def withdraw_fabric_api(upstream: Any, version: str) -> None:
    versions = [item for item in maven_versions(upstream.files[FABRIC_API_PATH]) if item != version]
    _serve_maven(upstream, FABRIC_API_PATH, "net.fabricmc.fabric-api", "fabric-api", versions)


def add_fabric_game(upstream: Any, version: str, *, stable: bool = True) -> None:
    listed = json.loads(upstream.files[FABRIC_GAME_PATH])
    listed.insert(0, {"version": version, "stable": stable})
    upstream.add_json(FABRIC_GAME_PATH, listed)


def withdraw_fabric_game(upstream: Any, *versions: str) -> None:
    listed = json.loads(upstream.files[FABRIC_GAME_PATH])
    upstream.add_json(FABRIC_GAME_PATH, [entry for entry in listed if entry["version"] not in versions])


def add_paper_version(upstream: Any, version: str, *, family: str) -> None:
    document = json.loads(upstream.files[PAPER_PROJECT_PATH])
    document["versions"].setdefault(family, [])
    document["versions"][family].insert(0, version)
    document["versions"] = {family: document["versions"][family], **document["versions"]}
    upstream.add_json(PAPER_PROJECT_PATH, document)
    upstream.add_json(paper_builds_path(version), [])


def withdraw_paper_version(upstream: Any, *versions: str) -> None:
    document = json.loads(upstream.files[PAPER_PROJECT_PATH])
    document["versions"] = {
        family: [version for version in listed if version not in versions]
        for family, listed in document["versions"].items()
    }
    document["versions"] = {family: listed for family, listed in document["versions"].items() if listed}
    upstream.add_json(PAPER_PROJECT_PATH, document)


def add_paper_build(
    upstream: Any,
    version: str,
    build: int,
    channel: str,
    *,
    sha256: str | None = None,
    time: str = "2026-09-17T12:00:00Z",
) -> dict[str, Any]:
    """Publish one more Paper build at the head of a version's listing (newest first)."""
    builds = json.loads(upstream.files.get(paper_builds_path(version), b"[]"))
    name = f"paper-{version}-{build}.jar"
    digest = sha256 or synthetic_sha256(name)
    entry = {
        "id": build,
        "time": time,
        "channel": channel,
        "downloads": {
            "server:default": {
                "name": name,
                "checksums": {"sha256": digest},
                "size": 1024,
                "url": f"https://fill-data.papermc.io/v1/objects/{digest}/{name}",
            }
        },
    }
    builds.insert(0, entry)
    upstream.add_json(paper_builds_path(version), builds)
    return entry


def withdraw_paper_build(upstream: Any, version: str, build: int) -> None:
    builds = json.loads(upstream.files[paper_builds_path(version)])
    upstream.add_json(paper_builds_path(version), [entry for entry in builds if entry["id"] != build])


def add_neoforge_version(upstream: Any, version: str) -> None:
    versions = maven_versions(upstream.files[NEOFORGE_META_PATH])
    versions.append(version)
    _serve_maven(upstream, NEOFORGE_META_PATH, "net.neoforged", "neoforge", versions)
    _serve_neoforge_sidecar(upstream, version)


def withdraw_neoforge_version(upstream: Any, *versions: str) -> None:
    kept = [item for item in maven_versions(upstream.files[NEOFORGE_META_PATH]) if item not in versions]
    _serve_maven(upstream, NEOFORGE_META_PATH, "net.neoforged", "neoforge", kept)


def add_snapshot(upstream: Any, rev: str, release_time: str, *, based_on: str = "26.3") -> str:
    """Publish one more Mojang snapshot and make it the snapshot head."""
    manifest = json.loads(upstream.files[support.MANIFEST_PATH])
    document = support.version_document(based_on)
    document["id"] = rev
    document["time"] = release_time
    document["releaseTime"] = release_time
    payload = json.dumps(document, indent=1).encode("utf-8")
    digest = hashlib.sha1(payload).hexdigest()
    path = f"/v1/packages/{digest}/{rev}.json"
    upstream.add(path, payload)
    manifest["versions"].insert(
        0,
        {
            "id": rev,
            "type": "snapshot",
            "url": upstream.base_url + path,
            "time": release_time,
            "releaseTime": release_time,
            "sha1": digest,
            "complianceLevel": 1,
        },
    )
    manifest["latest"] = {**manifest["latest"], "snapshot": rev}
    upstream.add(support.MANIFEST_PATH, json.dumps(manifest).encode("utf-8"))
    return path


# -- editing the catalog copy --------------------------------------------------
def _edit_game(root: Path, mutate: Any) -> None:
    game_file = root / "catalog/minecraft/game.json"
    game = json.loads(game_file.read_text(encoding="utf-8"))
    mutate(game)
    game_file.write_text(json.dumps(game, indent=2) + "\n", encoding="utf-8")


def enable_snapshot(root: Path) -> None:
    """Turn on Mojang's snapshot channel in this copy of the catalog."""
    _edit_game(root, lambda game: game["sources"]["mojang-meta"]["watch"]["channels"]["snapshot"].pop("enabled", None))


def enable_channel(root: Path, source: str, key: str) -> None:
    _edit_game(root, lambda game: game["sources"][source]["watch"]["channels"][key].pop("enabled", None))


def remove_channel(root: Path, source: str, key: str) -> None:
    _edit_game(root, lambda game: game["sources"][source]["watch"]["channels"].pop(key, None))


def catalog_digest(root: Path) -> str:
    """One digest over every file under ``catalog/``, so "nothing changed" is checkable."""
    digest = hashlib.sha256()
    for path in sorted((root / "catalog").rglob("*")):
        if path.is_file():
            digest.update(str(path.relative_to(root)).encode("utf-8"))
            digest.update(path.read_bytes())
    return digest.hexdigest()


# -- reading the tracker back --------------------------------------------------
def readiness_rows(body: str) -> dict[str, Any]:
    rows = readiness.parse_rows(body)
    return {platform: row for platform, row in (rows or {}).items()}


def state_of(body: str) -> str | None:
    return issues.existing_state(body)


def table_statuses(body: str) -> dict[str, str]:
    """The readiness the rendered table shows, platform by platform.

    The hidden line records only the platforms a framework source actually reported; the
    table is what a reader sees, and it says ``missing`` for every other platform.
    """
    statuses: dict[str, str] = {}
    for line in body.splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) == 6 and cells[0] not in ("Platform", "---") and cells[1] not in ("Readiness", "---"):
            statuses[cells[0]] = cells[1]
    return statuses


@dataclass
class Rig(support.Rig):
    """``test_scan_support.Rig`` scanning every watched source, not only Mojang's."""

    def scan(self, run: Any, *flags: str) -> tuple[int, Any, str]:
        return run("scan", "--repo", support.REPO, "--api-url", self.fake.api_url, *flags, repo=self.root)

    def issue_with(self, **marker: str) -> dict[str, Any]:
        for issue in self.fake.issues:
            fields = identity.parse_marker(str(issue.get("body") or "")) or {}
            if all(fields.get(key) == value for key, value in marker.items()):
                return issue
        raise AssertionError(f"no issue with marker {marker}; markers: {self.markers()}")

    def maybe_issue_with(self, **marker: str) -> dict[str, Any] | None:
        try:
            return self.issue_with(**marker)
        except AssertionError:
            return None

    def issues_with(self, **marker: str) -> list[dict[str, Any]]:
        found = []
        for issue in self.fake.issues:
            fields = identity.parse_marker(str(issue.get("body") or "")) or {}
            if all(fields.get(key) == value for key, value in marker.items()):
                found.append(issue)
        return found

    def rows(self, **marker: str) -> dict[str, Any]:
        return readiness_rows(str(self.issue_with(**marker)["body"]))

    def patches(self, number: int) -> int:
        return sum(1 for method, path in self.fake.requests if method == "PATCH" and path.endswith(f"/issues/{number}"))

    def checkpoint(self, source_key: str) -> dict[str, Any] | None:
        entry = self.dashboard_state()["sources"].get(source_key) or {}
        return entry.get("checkpoint")


@contextmanager
def rig(
    catalog_copy: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    releases: tuple[str, ...] = support.RECORDED,
    fabric: bool = True,
    paper: bool = True,
    neoforge: bool = True,
) -> Iterator[Rig]:
    """The whole rig: Mojang and the three frameworks on one fake upstream, a fake tracker.

    The clock is pinned, as in the #153 rig: Fabric and NeoForge publish no per-version
    timestamp, so their observations carry the run's clock reading, and an unpinned clock
    would make "the same head, observed again" render a different ``Observed`` cell
    whenever two scans happened to straddle a second.
    """
    readiness.reset_registry()
    support.frozen_clock(monkeypatch)
    with FakeUpstream() as upstream, FakeGitHub() as fake:
        served = support.mojang_upstream(upstream, releases)
        frameworks_upstream(upstream, fabric=fabric, paper=paper, neoforge=neoforge)
        support.scan_repo(catalog_copy, upstream)
        monkeypatch.setenv("GH_TOKEN", support.TOKEN)
        try:
            yield Rig(root=catalog_copy, upstream=upstream, fake=fake, served=served)
        finally:
            readiness.reset_registry()


# -- a server that records what the tool sent ----------------------------------
@dataclass
class RecordingUpstream:
    """Like ``FakeUpstream``, but it keeps every request's headers."""

    files: dict[str, bytes] = field(default_factory=dict)
    seen: list[tuple[str, str]] = field(default_factory=list)
    _server: http.server.ThreadingHTTPServer | None = field(default=None, init=False)
    _thread: threading.Thread | None = field(default=None, init=False)

    def add(self, path: str, payload: bytes) -> None:
        self.files[path] = payload

    def add_json(self, path: str, document: object) -> None:
        self.add(path, json.dumps(document).encode("utf-8"))

    @property
    def base_url(self) -> str:
        assert self._server is not None
        host, port = self._server.server_address[:2]
        return f"http://{host}:{port}"

    def __enter__(self) -> RecordingUpstream:
        recorder = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args: object) -> None:
                pass

            def do_GET(self) -> None:  # noqa: N802 - http.server's naming
                recorder.seen.append((self.path, self.headers.get("User-Agent", "")))
                payload = recorder.files.get(self.path)
                if payload is None:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        self._server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc: object) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=5)


# =============================================================================
# tests
# =============================================================================
def test_the_rig_serves_every_framework_listing(catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The helpers themselves: every path a watch block names answers, with usable bytes."""
    with rig(catalog_copy, monkeypatch) as harness:
        game = json.loads(harness.upstream.files[FABRIC_GAME_PATH])
        assert {"26.3", "26.2", "26.1.2"} <= {entry["version"] for entry in game}
        assert any(entry["version"] == "26.3-rc-3" and not entry["stable"] for entry in game)

        buckets = {version.partition("+")[2] for version in maven_versions(harness.upstream.files[FABRIC_API_PATH])}
        assert {"26.3", "26.2", "26.1.2"} <= buckets

        project = json.loads(harness.upstream.files[PAPER_PROJECT_PATH])
        assert project["versions"]["26.3"] == ["26.3", "26.3-rc-3"]
        assert [build["channel"] for build in json.loads(harness.upstream.files[paper_builds_path("26.3")])] == [
            "ALPHA",
            "ALPHA",
            "ALPHA",
        ]

        neoforge = maven_versions(harness.upstream.files[NEOFORGE_META_PATH])
        assert "21.11.45" in neoforge and "26.2.0.88" in neoforge and "0.25w14craftmine.3-beta" in neoforge

        sidecar = harness.upstream.files[fabric_api_path("0.160.7+26.3") + ".sha256"].decode().strip()
        assert sidecar == "1720e31ab65c62d4de6e963606d74db4d6063bf58b065f40296cdaa68b25759d"
        assert len(sidecar) == 64


def test_a_release_without_any_framework_is_filed_blocked_upstream(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC4, first half: nothing can build 26.3 yet, so the issue says so and blocks."""
    with rig(catalog_copy, monkeypatch) as harness:
        withdraw_fabric_game(harness.upstream, "26.3")
        withdraw_paper_version(harness.upstream, "26.3", "26.3-rc-3")
        withdraw_neoforge_version(harness.upstream, "26.3.0.0-beta", "26.3.0.1-beta", "26.3.0.2-beta", "26.3.0.3-beta")

        code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        assert [entry["action"] for entry in payload["applied"] if entry["action"] == "create-issue"] == [
            "create-issue"
        ]
        support_issues = harness.issues_with(kind="support")
        assert len(support_issues) == 1
        body = str(support_issues[0]["body"])
        assert table_statuses(body) == {"fabric": "missing", "neoforge": "missing", "paper": "missing"}
        assert readiness_rows(body)["fabric"].status == "missing"
        assert state_of(body) == "blocked-upstream"
        assert "| fabric | missing | — | — | — | — |" in body


def test_framework_availability_reconciles_to_ready_without_a_new_issue(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC4, second half: the same issue moves to ready-for-agent; no second issue appears."""
    with rig(catalog_copy, monkeypatch) as harness:
        withdraw_fabric_game(harness.upstream, "26.3")
        withdraw_paper_version(harness.upstream, "26.3", "26.3-rc-3")
        withdraw_neoforge_version(harness.upstream, "26.3.0.0-beta", "26.3.0.1-beta", "26.3.0.2-beta", "26.3.0.3-beta")
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        issue_number = int(harness.issue_with(kind="support", rev="26.3")["number"])

        add_fabric_game(harness.upstream, "26.3")
        add_fabric_api(harness.upstream, "0.161.0+26.3")
        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        assert len(harness.issues_with(kind="support")) == 1
        assert not [entry for entry in payload["applied"] if entry["action"] == "create-issue"]
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        row = readiness_rows(body)["fabric"]
        assert row.status == "ready"
        assert row.rev == "0.161.0+26.3"
        assert row.sha256 == synthetic_sha256("fabric-api-0.161.0+26.3")
        assert row.artifact == "fabric-api-0.161.0+26.3.jar"
        assert state_of(body) == "ready-for-agent"
        assert table_statuses(body)["paper"] == "missing"

        # Nothing new upstream: the next run re-reads the same head and writes nothing.
        before = harness.patches(issue_number)
        harness.forget("0.161.0+26.3", FABRIC_KEY)
        code, payload, stderr = harness.scan(run, "--publish")
        assert code == 0, stderr
        assert harness.patches(issue_number) == before
        assert [entry.get("reason") for entry in payload["applied"] if entry["action"] == "noop"] == [
            "readiness-current"
        ]


def test_fabric_readiness_ignores_the_loader_listing(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC3: the loader list is identical for every version, so it can never be evidence."""
    with rig(catalog_copy, monkeypatch) as harness:
        harness.upstream.status_overrides[FABRIC_LOADER_PATH] = 404
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        rows_without_loader = harness.rows(kind="support", rev="26.3")
        assert harness.requested(FABRIC_LOADER_PATH) >= 1
        assert harness.dashboard_state()["sources"][FABRIC_KEY]["status"] == "ok"

        del harness.upstream.status_overrides[FABRIC_LOADER_PATH]
        assert harness.scan(run, "--publish")[0] == 0
        rows_with_loader = harness.rows(kind="support", rev="26.3")

        assert {platform: row.status for platform, row in rows_with_loader.items()} == {
            platform: row.status for platform, row in rows_without_loader.items()
        }


def test_fabric_readiness_needs_the_game_listed_and_the_bucket(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC3: both halves are required, and the head is the last listed, not the largest."""
    with rig(catalog_copy, monkeypatch) as harness:
        # A bucket for a version Fabric does not list: no row.
        add_fabric_api(harness.upstream, "0.10.0+26.4")
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        support.add_release(harness.upstream, "26.4", release_time="2026-09-18T08:00:00Z")
        assert harness.scan(run, "--publish")[0] == 0
        assert harness.rows(kind="support", rev="26.4")["fabric"].status == "missing"

        # Fabric lists it, and the *last* listed bucket member wins even when it is the
        # numerically smaller one.
        add_fabric_game(harness.upstream, "26.4")
        add_fabric_api(harness.upstream, "0.9.0+26.4")
        assert harness.scan(run, "--publish")[0] == 0
        row = harness.rows(kind="support", rev="26.4")["fabric"]
        assert row.status == "ready"
        assert row.rev == "0.9.0+26.4"


def test_paper_stable_makes_ready_and_alpha_only_is_preview_only(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC7 and the preview rule: an alpha build is reported, never counted as ready."""
    enable_channel(catalog_copy, "paper-fill", "alpha")
    with rig(catalog_copy, monkeypatch) as harness:
        withdraw_fabric_game(harness.upstream, "26.3")  # so the state is Paper's answer alone
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        row = readiness_rows(body)["paper"]
        assert row.status == "preview-only (alpha build 16)"
        assert row.rev == "26.3-16"
        assert row.sha256 == "131a04201abb4627d97e296f42e41a2e8238e836ece27c9a5457cf0fe58512ff"
        assert state_of(body) == "blocked-upstream"

        add_paper_build(harness.upstream, "26.3", 17, "STABLE", sha256="bb" * 32)
        assert harness.scan(run, "--publish")[0] == 0
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        row = readiness_rows(body)["paper"]
        assert row.status == "ready"
        assert row.rev == "26.3-17"
        assert row.artifact == "paper-26.3-17.jar"
        assert row.sha256 == "bb" * 32
        assert state_of(body) == "ready-for-agent"


def test_paper_requests_carry_the_identifying_user_agent(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC3: PaperMC's policy asks for an identifying UA with a contact URL; `net` sends it."""
    with RecordingUpstream() as recorder, FakeGitHub() as fake:
        frameworks_upstream(recorder, fabric=False, neoforge=False)
        support.scan_repo(catalog_copy, recorder)
        monkeypatch.setenv("GH_TOKEN", support.TOKEN)
        readiness.reset_registry()

        code, _, stderr = run(
            "scan",
            "--repo",
            support.REPO,
            "--api-url",
            fake.api_url,
            "--source",
            "paper-fill",
            "--bootstrap",
            "--publish",
            repo=catalog_copy,
        )

        assert code == 0, stderr
        assert any(path == PAPER_PROJECT_PATH for path, _ in recorder.seen)
        assert {agent for _, agent in recorder.seen} == {net.USER_AGENT}
        assert "takaro-connectors-maint/" in net.USER_AGENT
        assert "+https://github.com/gettakaro/connectors" in net.USER_AGENT


def test_neoforge_handles_both_version_grammars(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """AC3: ``21.11.45`` is Minecraft 1.21.11 and ``26.2.0.88`` is 26.2; betas stay off."""
    with rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        # Nothing filed the older releases (they are behind the head), so seed them by hand
        # and let a second run reconcile the framework rows onto them.
        harness.forget("26.2.0.88", NEOFORGE_KEY)
        harness.forget("21.11.45", NEOFORGE_KEY)
        harness.forget("26.1.2.109", NEOFORGE_KEY)
        for rev in ("26.2", "1.21.11", "26.1.2"):
            _seed_support_issue(harness, rev)

        assert harness.scan(run, "--publish")[0] == 0

        assert harness.rows(kind="support", rev="1.21.11")["neoforge"].rev == "21.11.45"
        assert harness.rows(kind="support", rev="26.2")["neoforge"].rev == "26.2.0.88"
        assert harness.rows(kind="support", rev="26.1.2")["neoforge"].rev == "26.1.2.109"
        # 26.3 has betas only, and beta is switched off: no observation, so the table says
        # missing and the hidden line records nothing for NeoForge at all.
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        assert table_statuses(body)["neoforge"] == "missing"
        assert "neoforge" not in readiness_rows(body)


def test_neoforge_betas_are_reported_when_the_channel_is_enabled(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The other side of the same switch: with `beta` on, 26.3 gets a preview-only row."""
    enable_channel(catalog_copy, "neoforge-maven", "beta")
    with rig(catalog_copy, monkeypatch) as harness:
        withdraw_fabric_game(harness.upstream, "26.3")  # so the state is NeoForge's answer alone
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        row = harness.rows(kind="support", rev="26.3")["neoforge"]
        assert row.status == "preview-only (beta 26.3.0.3-beta)"
        assert row.rev == "26.3.0.3-beta"
        assert state_of(str(harness.issue_with(kind="support", rev="26.3")["body"])) == "blocked-upstream"


def test_a_readiness_update_never_touches_a_later_lifecycle_state(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC: a state a later stage wrote outranks readiness, which only ever adds rows."""
    with rig(catalog_copy, monkeypatch) as harness:
        withdraw_fabric_game(harness.upstream, "26.3")
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        issue = harness.issue_with(kind="support", rev="26.3")
        issue["body"] = str(issue["body"]).replace(
            "<!-- takaro-maint:state=blocked-upstream -->", "<!-- takaro-maint:state=implementation-pr -->"
        )

        add_fabric_game(harness.upstream, "26.3")
        add_fabric_api(harness.upstream, "0.161.0+26.3")
        assert harness.scan(run, "--publish")[0] == 0

        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        assert readiness_rows(body)["fabric"].status == "ready"
        assert state_of(body) == "implementation-pr"


def test_readiness_survives_a_partial_source_run(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A Mojang-only run files the sentence; a later full run replaces it with the table."""
    with rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--source", "mojang-meta", "--bootstrap", "--publish")[0] == 0
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        assert readiness.parse_rows(body) is None
        assert issues.READINESS_SENTENCE in body
        assert state_of(body) == "detected"

        # A run where a framework has something new re-renders the row it observed.
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        add_fabric_api(harness.upstream, "0.161.0+26.3")
        assert harness.scan(run, "--publish")[0] == 0
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        assert readiness_rows(body)["fabric"].rev == "0.161.0+26.3"
        assert issues.READINESS_SENTENCE not in body


def test_observations_never_edit_the_catalog(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """AC9: a scan reads the catalog and writes issues; it never writes a catalog file."""
    with rig(catalog_copy, monkeypatch) as harness:
        before = catalog_digest(harness.root)
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        assert catalog_digest(harness.root) == before

        add_fabric_api(harness.upstream, "0.161.0+26.3")
        add_paper_build(harness.upstream, "26.3", 17, "STABLE")
        add_neoforge_version(harness.upstream, "26.3.0.4")
        assert harness.scan(run, "--publish")[0] == 0
        assert catalog_digest(harness.root) == before

        snapshot = harness.dashboard_state()["targets"]["minecraft"]
        assert snapshot == [{"id": "fabric-26.2", "platform": "fabric", "revision": "26.2", "status": "maintained"}]


def test_the_readiness_table_rows_are_durable_in_the_hidden_line(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The rendered table and the hidden JSON say the same thing, and survive a round trip."""
    with rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        rows = readiness.parse_rows(body)
        assert rows is not None
        assert readiness.dump_rows(rows) in body
        rendered = readiness.render_rows(rows, ["fabric", "paper", "neoforge"])
        assert readiness.parse_rows("\n".join(rendered)) == rows
        for platform, row in rows.items():
            assert f"| {platform} | {row.status} |" in body


def _seed_support_issue(harness: Rig, rev: str, *, branch: str = "release") -> dict[str, Any]:
    """A support issue for ``rev`` as a previous run would have left it, rows not yet written."""
    from takaro_maint.providers.base import Observation

    observation = Observation(
        provider="mojang",
        component="minecraft",
        branch=branch,
        rev=rev,
        kind="game",
        identity=identity.canonical("mojang", "minecraft", branch, rev),
        facts={"releaseTime": "2026-01-01T00:00:00Z"},
        observed_at=support.FROZEN_NOW,
    )
    marker = identity.support_marker(observation)
    block = issues.render_owned_block(observation, support.golden_targets())
    return harness.seed_issue(issues.render_body(marker, block), title=f"Minecraft {rev}")
