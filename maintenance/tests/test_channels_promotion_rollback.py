"""Channels: the opt-in snapshot stream, promotion, rollback and undeclared branches.

Everything here runs the real ``scan`` against the shared rig in
``test_readiness_transitions``. The point of most of these scenarios is a *negative*: a
preview that must not be filed, an issue that must not be edited, a rollback that must not
be decided by comparing version numbers.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

import test_readiness_transitions as rt
import test_scan_support as support

PAPER_KEY = rt.PAPER_KEY
NEOFORGE_KEY = rt.NEOFORGE_KEY


def test_the_snapshot_channel_is_off_by_default(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """AC1: the recorded manifest holds 11 snapshots and the scan files none of them."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        manifest = json.loads(harness.upstream.files[support.MANIFEST_PATH])
        assert len([entry for entry in manifest["versions"] if entry["type"] == "snapshot"]) == 11

        code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        assert not harness.issues_with(kind="support", branch="snapshot")
        assert payload["sources"][support.SOURCE_KEY]["heads"] == {"release": "26.3"}
        assert all(document["branch"] != "snapshot" for document in payload["observations"])


def test_enabling_the_snapshot_channel_files_previews_independently(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC1: turning the channel on files previews under their own branch and title."""
    rt.enable_snapshot(catalog_copy)
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        releases_before = {issue["number"] for issue in harness.issues_with(kind="support", branch="release")}

        rt.add_snapshot(harness.upstream, "26.4-snapshot-1", "2026-09-18T09:00:00Z")
        code, _, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        preview = harness.issue_with(kind="support", branch="snapshot", rev="26.4-snapshot-1")
        assert preview["title"] == "Minecraft 26.4-snapshot-1: new snapshot preview"
        assert "Mojang published a new preview (snapshot channel)." in str(preview["body"])
        assert {issue["number"] for issue in harness.issues_with(kind="support", branch="release")} == releases_before
        # Connector distribution channels live in release-please plumbing, not in the catalog:
        # this copy has no such file and the scan never reaches for one.
        assert not list(harness.root.glob(".github/**/*"))
        assert not list(harness.root.glob("scripts/**/*"))


def test_equal_release_and_snapshot_emit_no_preview(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC5: Mojang points both heads at 26.3 today; that is one release, not two things."""
    rt.enable_snapshot(catalog_copy)
    with rt.rig(catalog_copy, monkeypatch) as harness:
        manifest = json.loads(harness.upstream.files[support.MANIFEST_PATH])
        assert manifest["latest"]["release"] == manifest["latest"]["snapshot"] == "26.3"

        code, _, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        assert len(harness.issues_with(kind="support", rev="26.3")) == 1
        assert harness.issue_with(kind="support", rev="26.3")["body"].startswith(
            "<!-- takaro-maint: kind=support provider=mojang component=minecraft branch=release rev=26.3 -->"
        )
        assert not harness.issues_with(kind="support", branch="snapshot", rev="26.3")


def test_promotion_creates_the_release_issue_and_supersedes_the_preview(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC5: the release arrives, its previews are marked superseded and left otherwise alone."""
    rt.enable_snapshot(catalog_copy)
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        rt.add_snapshot(harness.upstream, "26.4-pre-1", "2026-09-18T09:00:00Z")
        rt.add_snapshot(harness.upstream, "26.4-rc-1", "2026-09-19T09:00:00Z")
        assert harness.scan(run, "--publish")[0] == 0
        previews = {
            rev: harness.issue_with(kind="support", branch="snapshot", rev=rev) for rev in ("26.4-pre-1", "26.4-rc-1")
        }
        titles = {rev: issue["title"] for rev, issue in previews.items()}
        # A preview of a different base: the promotion of 26.4 must not touch it.
        other = rt._seed_support_issue(harness, "26.3-rc-3", branch="snapshot")

        support.add_release(harness.upstream, "26.4", release_time="2026-09-20T09:00:00Z")
        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        release = harness.issue_with(kind="support", branch="release", rev="26.4")
        for rev in previews:
            body = str(harness.issue_with(kind="support", branch="snapshot", rev=rev)["body"])
            assert rt.state_of(body) == "superseded"
            assert f"Superseded by #{release['number']}" in body
            assert harness.issue_with(kind="support", branch="snapshot", rev=rev)["title"] == titles[rev]
        assert rt.state_of(str(other["body"])) != "superseded"
        actions = [entry["action"] for entry in payload["applied"]]
        assert actions.count("create-issue") == 1  # the release, and nothing else
        # The previews were written to, the unrelated one was not.
        assert harness.patches(int(previews["26.4-pre-1"]["number"])) == 1
        assert harness.patches(int(previews["26.4-rc-1"]["number"])) == 1
        assert harness.patches(int(other["number"])) == 0


def test_promotion_requires_destination_branch_evidence(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC5: a framework listing the version, or a newer preview, is not the release."""
    rt.enable_snapshot(catalog_copy)
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        rt.add_snapshot(harness.upstream, "26.4-rc-1", "2026-09-18T09:00:00Z")
        assert harness.scan(run, "--publish")[0] == 0
        preview = harness.issue_with(kind="support", branch="snapshot", rev="26.4-rc-1")
        before = str(preview["body"])

        rt.add_fabric_game(harness.upstream, "26.4")
        rt.add_fabric_api(harness.upstream, "0.162.0+26.4")
        rt.add_paper_version(harness.upstream, "26.4", family="26.4")
        rt.add_paper_build(harness.upstream, "26.4", 1, "STABLE")
        rt.add_snapshot(harness.upstream, "26.4-rc-2", "2026-09-19T09:00:00Z")
        code, _, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        after = str(harness.issue_with(kind="support", branch="snapshot", rev="26.4-rc-1")["body"])
        assert rt.state_of(after) != "superseded"
        assert "Superseded by" not in after
        assert rt.state_of(after) == rt.state_of(before)
        assert not harness.issues_with(kind="support", branch="release", rev="26.4")


def test_a_declined_preview_is_not_superseded_and_stays_declined(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC8: a human closed it as not planned; the promotion walks straight past it."""
    rt.enable_snapshot(catalog_copy)
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        rt.add_snapshot(harness.upstream, "26.4-rc-1", "2026-09-18T09:00:00Z")
        assert harness.scan(run, "--publish")[0] == 0
        preview = harness.issue_with(kind="support", branch="snapshot", rev="26.4-rc-1")
        number = int(preview["number"])
        preview["state"] = "closed"
        preview["state_reason"] = "not_planned"
        frozen = str(preview["body"])
        harness.fake.requests.clear()

        support.add_release(harness.upstream, "26.4", release_time="2026-09-20T09:00:00Z")
        code, _, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        assert harness.issues_with(kind="support", branch="release", rev="26.4")
        assert str(preview["body"]) == frozen
        assert preview["state"] == "closed"
        assert harness.patches(number) == 0


def test_framework_rollback_is_annotated_by_set_membership(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC6: a withdrawn build is a rollback, and nothing here compares build numbers."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        issue = rt._seed_support_issue(harness, "1.21.11")
        number = int(issue["number"])

        rt.add_paper_build(harness.upstream, "1.21.11", 133, "STABLE", time="2026-09-18T09:00:00Z")
        assert harness.scan(run, "--publish")[0] == 0
        row = harness.rows(kind="support", rev="1.21.11")["paper"]
        assert row.rev == "1.21.11-133"
        assert row.rollback_from is None

        rt.withdraw_paper_build(harness.upstream, "1.21.11", 133)
        assert harness.scan(run, "--publish")[0] == 0
        body = str(harness.issue_with(kind="support", rev="1.21.11")["body"])
        row = rt.readiness_rows(body)["paper"]
        assert row.rev == "1.21.11-132"
        assert row.rollback_from == "1.21.11-133"
        assert "ready (rolled back from 1.21.11-133)" in body

        patches = harness.patches(number)
        assert harness.scan(run, "--publish")[0] == 0
        assert harness.patches(number) == patches  # the same rollback is never filed twice

        # A *lower* build number published later is still the newer head: listing order
        # decides, and no code anywhere compares 99 with 132.
        rt.add_paper_build(harness.upstream, "1.21.11", 99, "STABLE", time="2026-09-19T09:00:00Z")
        assert harness.scan(run, "--publish")[0] == 0
        row = harness.rows(kind="support", rev="1.21.11")["paper"]
        assert row.rev == "1.21.11-99"
        assert row.rollback_from is None


def test_a_mutable_framework_release_tracks_its_digest(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC7: a new build of the same game version replaces the row, digest and all."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        rt._seed_support_issue(harness, "26.2")
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        row = harness.rows(kind="support", rev="26.2")["paper"]
        assert row.rev == "26.2-124"
        assert row.sha256 == "274bbcb9807ad79a479617ce695a7c0686e652a8d1cabfd0e288d92a3ea29593"

        rt.add_paper_build(harness.upstream, "26.2", 125, "STABLE", sha256="bb" * 32)
        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        row = harness.rows(kind="support", rev="26.2")["paper"]
        assert row.rev == "26.2-125"
        assert row.artifact == "paper-26.2-125.jar"
        assert row.sha256 == "bb" * 32
        assert any(entry["action"] == "update-issue" for entry in payload["applied"])
        seen = [entry[0] for entry in (harness.checkpoint(PAPER_KEY) or {})["seen"]]
        assert "26.2-125" in seen


def test_an_undeclared_branch_becomes_a_review_candidate(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC2: an undeclared or unparseable branch is a question, never a commitment."""
    rt.remove_channel(catalog_copy, "neoforge-maven", "alpha")
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        rows_before = harness.rows(kind="support", rev="26.3")

        rt.add_neoforge_version(harness.upstream, "26.4.0.0-alpha.1+snapshot-1")
        rt.add_neoforge_version(harness.upstream, "0.26w03craftmine.1-beta")
        rt.add_paper_build(harness.upstream, "26.2", 126, "EXPERIMENTAL")
        code, _, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        reviews = harness.issues_with(kind="branch-review")
        by_rev = {str(issue["title"]): issue for issue in reviews}
        assert len(reviews) == 3
        assert (
            "Minecraft neoforge: unrecognised upstream branch 'alpha' (26.4.0.0-alpha.1+snapshot-1) needs review"
            in by_rev
        )
        assert "Minecraft neoforge: unrecognised upstream branch 'unknown' (0.26w03craftmine.1-beta) needs review" in (
            by_rev
        )
        assert "Minecraft paper: unrecognised upstream branch 'experimental' (26.2-126) needs review" in by_rev
        for issue in reviews:
            assert rt.state_of(str(issue["body"])) == "review"
            assert "- [ ] decided: watch it (channel added) / ignore it (closed as not planned)" in str(issue["body"])
        assert harness.rows(kind="support", rev="26.3") == rows_before


def test_a_declined_branch_review_stays_declined(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """AC8: declining a branch sticks, but a new revision on it is a new question."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        rt.add_neoforge_version(harness.upstream, "0.26w03craftmine.1-beta")
        assert harness.scan(run, "--publish")[0] == 0
        review = harness.issue_with(kind="branch-review", rev="0.26w03craftmine.1-beta")
        number = int(review["number"])
        review["state"] = "closed"
        review["state_reason"] = "not_planned"
        frozen = str(review["body"])

        harness.forget("0.26w03craftmine.1-beta", NEOFORGE_KEY)
        harness.fake.requests.clear()
        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        assert len(harness.issues_with(kind="branch-review", rev="0.26w03craftmine.1-beta")) == 1
        assert str(review["body"]) == frozen
        assert harness.patches(number) == 0
        assert any(entry.get("reason") == "declined" for entry in payload["applied"])

        rt.add_neoforge_version(harness.upstream, "0.26w04craftmine.1-beta")
        assert harness.scan(run, "--publish")[0] == 0
        assert harness.issue_with(kind="branch-review", rev="0.26w04craftmine.1-beta")
        assert str(review["body"]) == frozen


def test_disabled_channels_are_neither_observed_nor_reviewed(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC2: a channel switched off is known and unwatched; it is not an open question."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        assert not harness.issues_with(kind="branch-review")
        rt.add_neoforge_version(harness.upstream, "26.3.0.4-beta")
        rows_before = harness.rows(kind="support", rev="26.3")
        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        assert all(document["branch"] != "beta" for document in payload["observations"])
        assert not harness.issues_with(kind="branch-review")
        assert harness.rows(kind="support", rev="26.3") == rows_before
        assert "beta" not in payload["sources"][NEOFORGE_KEY]["heads"]
