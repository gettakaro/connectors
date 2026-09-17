"""Bootstrap has one job: start tracking without filing fifteen years of history.

It records everything already published as seen, then files an issue only for a current
head that no catalog target covers.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

import test_scan_support as support


def test_bootstrap_seeds_heads_and_files_only_the_uncovered_head(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    support.frozen_clock(monkeypatch)
    with support.rig(catalog_copy, monkeypatch) as harness:
        code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        assert payload["mode"] == "publish"
        applied = [entry["action"] for entry in payload["applied"]]
        assert applied == ["create-issue", "create-dashboard"]

        support_issues = harness.support_issues()
        assert len(support_issues) == 1
        assert len(harness.fake.issues) == 2  # the support issue and the dashboard
        filed = support_issues[0]
        assert filed["title"] == "Minecraft 26.3: new stable release needs a target"
        assert filed["labels"] == ["connector-maintenance"]
        assert str(filed["body"]).splitlines()[0] == (
            "<!-- takaro-maint: kind=support provider=mojang component=minecraft branch=release rev=26.3 -->"
        )

        state = harness.dashboard_state()
        source = state["sources"][support.SOURCE_KEY]
        assert source["status"] == "ok"
        assert source["heads"] == {"release": "26.3"}
        assert sorted(harness.checkpoint_ids()) == sorted(["26.3", "26.2", "26.1.2", "26.1.1", "26.1", "1.21.11"])
        assert source["checkpoint"]["floor"] is None
        assert state["targets"]["minecraft"] == support.catalog_target_rows(catalog_copy)
        assert {"id": "fabric-26.2", "platform": "fabric", "revision": "26.2", "status": "maintained"} in (
            state["targets"]["minecraft"]
        )
        work = state["work"]["provider=mojang component=minecraft branch=release rev=26.3"]
        assert work == {"issue": filed["number"], "state": "detected", "since": support.FROZEN_NOW}
        assert state["lastSuccess"] == support.FROZEN_NOW

        # Only the head's per-version document was ever fetched.
        assert harness.requested(harness.served["26.3"]) == 1
        assert harness.requested(harness.served["26.2"]) == 0


def test_the_filed_issue_carries_no_catalog_change_and_no_readiness_claim(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    before = sorted(path.name for path in (catalog_copy / "catalog/minecraft/targets").glob("*.json"))
    with support.rig(catalog_copy, monkeypatch) as harness:
        code, _, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        body = str(harness.support_issues()[0]["body"])
        assert "Framework readiness is not assessed by this scan" in body
        assert "<!-- takaro-maint:state=detected -->" in body
        after = sorted(path.name for path in (catalog_copy / "catalog/minecraft/targets").glob("*.json"))
        assert after == before
        assert '"status": "maintained"' in (catalog_copy / "catalog/minecraft/targets/fabric-26.2.json").read_text()


def test_a_covered_head_files_nothing(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    support.add_candidate_target(catalog_copy, "26.3")
    with support.rig(catalog_copy, monkeypatch) as harness:
        code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        assert harness.support_issues() == []
        assert [entry["action"] for entry in payload["applied"]] == ["create-dashboard"]
        assert payload["plan"][1]["seeded"] == 6  # every release is seeded; none needs an issue
        assert payload["observations"] == []
        assert sorted(harness.checkpoint_ids()) == sorted(["26.3", "26.2", "26.1.2", "26.1.1", "26.1", "1.21.11"])


def test_publish_without_bootstrap_on_an_uninitialised_source_exits_two(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    with support.rig(catalog_copy, monkeypatch) as harness:
        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 2, stderr
        assert payload["ok"] is False
        assert "--bootstrap" in payload["error"]
        assert harness.fake.writes == 0
        assert harness.fake.issues == []


def test_a_blocked_publish_writes_nothing_even_when_another_source_has_work(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The stop is decided before the first issue is filed, not after the loop.

    One initialised source with a brand new release, one source that has never been
    initialised: the run has to refuse as a whole, because a half-published run whose
    checkpoint was never written is the worst of both outcomes.
    """
    with support.rig(catalog_copy, monkeypatch) as harness:
        code, _, stderr = harness.scan(run, "--bootstrap", "--publish")
        assert code == 0, stderr
        issues_before = len(harness.fake.issues)
        writes_before = harness.fake.writes
        seen_before = sorted(harness.checkpoint_ids())

        second = support.add_second_watch_source(catalog_copy, harness.upstream)
        support.add_release(harness.upstream, "26.4", release_time="2026-12-01T10:00:00+00:00")
        support.mirror_manifest(harness.upstream)

        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 2, stderr
        assert "nothing was written" in stderr
        assert harness.fake.writes == writes_before
        assert len(harness.fake.issues) == issues_before
        assert sorted(harness.checkpoint_ids()) == seen_before
        assert "26.4" not in seen_before
        assert payload["sources"][second]["status"] == "uninitialized"
        # The report still says what is out there — it just carries out none of it.
        assert [entry["rev"] for entry in payload["observations"]] == ["26.4"]
        assert payload["applied"] == []
        assert [entry["action"] for entry in payload["plan"]] == ["bootstrap-required", "create-issue"]


def test_bootstrap_twice_is_idempotent(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    with support.rig(catalog_copy, monkeypatch) as harness:
        first_code, _, _ = harness.scan(run, "--bootstrap", "--publish")
        seen_after_first = sorted(harness.checkpoint_ids())
        issues_after_first = len(harness.fake.issues)

        second_code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert (first_code, second_code) == (0, 0), stderr
        assert len(harness.fake.issues) == issues_after_first
        assert sorted(harness.checkpoint_ids()) == seen_after_first
        assert "already initialised" in stderr
        actions = [entry["action"] for entry in payload["applied"]]
        assert actions == ["update-dashboard"]
        assert payload["observations"] == []


def test_unseen_history_is_processed_once(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    with support.rig(catalog_copy, monkeypatch) as harness:
        code, _, stderr = harness.scan(run, "--bootstrap", "--publish")
        assert code == 0, stderr
        path = support.add_release(harness.upstream, "26.4", release_time="2026-12-01T10:00:00+00:00")

        second_code, second, stderr = harness.scan(run, "--publish")

        assert second_code == 0, stderr
        assert second["sources"][support.SOURCE_KEY]["heads"] == {"release": "26.4"}
        assert [entry["action"] for entry in second["applied"]] == ["create-issue", "update-dashboard"]
        assert len(harness.support_issues()) == 2
        assert harness.requested(path) == 1
        assert "26.4" in harness.checkpoint_ids()

        third_code, third, stderr = harness.scan(run, "--publish")

        assert third_code == 0, stderr
        assert [entry["action"] for entry in third["applied"]] == ["update-dashboard"]
        assert len(harness.support_issues()) == 2
        assert harness.requested(path) == 1  # never fetched again
