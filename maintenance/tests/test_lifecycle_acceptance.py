"""Acceptance for the rollout: bootstrap bounds, failure visibility, recovery and release safety.

These are the scenarios the operator is promised, driven through the real commands on controlled
inputs -- the same rigs the unit suites use, never a second implementation of them:

* a first publishing run files the *current uncovered* heads and nothing else, and seeds the rest;
* one provider down is a nonzero run that keeps its checkpoint, shows as degraded and recovers on
  a manual rerun;
* a dashboard that moves under a run is a compare-and-swap conflict (exit 9) the rerun repairs;
* two providers' issues reconcile independently in one tracker;
* a required target without passing evidence blocks a stable assembly and nothing is uploaded;
* a tag-based recovery never replaces conflicting bytes, and an interrupted one resumes.

The failure modes of exact inputs (unavailable, altered, preserved installs, artifact/target
mismatch) are already proven by the modules named in ``REUSED``; this module pins those names so
the reuse claim cannot rot, rather than proving them a second time.
"""

from __future__ import annotations

import importlib
import json
import shutil
from pathlib import Path
from typing import Any

import pytest

import fake_steamcmd
import test_lifecycle as lc
import test_readiness_transitions as transitions
import test_scan_support as support
from conftest import REPO_ROOT
from fake_github_releases import FakeReleases, serving
from takaro_maint.tracker import issues
from test_dashboard_show import show
from test_release_assemble import CONNECTOR, Inputs, assemble, assemble_default, build_inputs
from test_release_publish import TAG, publish, reassemble, stable_draft

CONAN = "conan-exiles"
CONAN_APP = 443030
CONAN_CONTENT_DEPOT = "443032"
CONAN_MOVED_MANIFEST = "2600000000000000001"
CONAN_MOVED_BUILD = 25400000

MOJANG_HEADS = ("26.3", "26.2", "26.1.2", "26.1.1", "26.1", "1.21.11")


def _error_of(payload: Any) -> str:
    """The error a ``run`` document reports, wherever the failing half put it."""
    if not isinstance(payload, dict):
        return str(payload)
    parts = [payload.get("error")]
    for half in ("scan", "reconcile"):
        section = payload.get(half)
        if isinstance(section, dict):
            parts.append(section.get("error"))
    return " ".join(str(part) for part in parts if part)


def _actions(section: Any) -> list[str]:
    return [str(entry["action"]) for entry in (section or {}).get("applied", [])]


# =============================================================================
# the first publishing run
# =============================================================================


def test_bootstrap_files_only_current_uncovered_heads_and_seeds_history(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The first publishing run files the uncovered head and records the rest as seen."""
    with lc.lifecycle_rig(catalog_copy, monkeypatch) as harness:
        code, payload, stderr = harness.run_command(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        assert payload["exitCodes"] == {"scan": 0, "reconcile": 0}
        # One issue and the board; every other observation is a recorded no-op, never a file.
        applied = payload["scan"]["applied"]
        assert [entry["action"] for entry in applied if entry["action"] != "noop"] == [
            "create-issue",
            "create-dashboard",
        ], applied
        assert {entry["reason"] for entry in applied if entry["action"] == "noop"} <= {"no-support-issue"}, applied
        assert "close-issue" not in _actions(payload["reconcile"])

        filed = harness.support_issues()
        assert len(filed) == 1
        assert filed[0]["title"] == "Minecraft 26.3: new stable release needs a target"

        # Everything already published is seen; only the head that no target covers was filed.
        assert sorted(harness.checkpoint_ids()) == sorted(MOJANG_HEADS)
        titles = [str(issue["title"]) for issue in filed]
        for old in MOJANG_HEADS[1:]:
            assert not any(f"Minecraft {old}:" in title for title in titles), old

        # A second publishing run over the same tracker files nothing again.
        code, payload, stderr = harness.run_command(run, "--publish")

        assert code == 0, stderr
        assert "create-issue" not in _actions(payload["scan"])
        assert len(harness.support_issues()) == 1


# =============================================================================
# failure reporting, checkpoint retention, manual recovery
# =============================================================================


def test_a_failed_source_reports_nonzero_keeps_its_checkpoint_and_a_manual_rerun_recovers(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Paper is down: the run is red, its checkpoint stands, the board degrades, a rerun repairs."""
    with lc.lifecycle_rig(catalog_copy, monkeypatch) as harness:
        assert harness.run_command(run, "--bootstrap", "--publish")[0] == 0
        bootstrap = show(run, harness)[1]
        paper_before = harness.checkpoint(transitions.PAPER_KEY)
        fabric_before = harness.checkpoint(transitions.FABRIC_KEY)

        transitions.add_fabric_api(harness.upstream, "0.161.0+26.3")
        harness.upstream.status_overrides[transitions.PAPER_PROJECT_PATH] = 503

        code, payload, stderr = harness.run_command(run, "--publish")

        assert code == 4, stderr
        assert payload["exitCodes"] == {"scan": 4, "reconcile": 0}
        sources = payload["scan"]["sources"]
        assert sources[transitions.PAPER_KEY]["status"] == "failed"
        assert sources[transitions.PAPER_KEY]["checkpoint"]["after"] is None
        assert sources[transitions.PAPER_KEY]["checkpoint"]["before"] is not None
        for key in (support.SOURCE_KEY, transitions.FABRIC_KEY, transitions.NEOFORGE_KEY):
            assert sources[key]["status"] == "ok", key

        # The failed source's checkpoint is exactly what it was; the healthy ones moved on.
        assert harness.checkpoint(transitions.PAPER_KEY) == paper_before
        assert harness.checkpoint(transitions.FABRIC_KEY) != fabric_before

        code, degraded, stderr = show(run, harness)
        assert code == 0, stderr
        assert degraded["health"] == "degraded"
        assert degraded["counts"]["failed"] == 1
        assert degraded["lastSuccess"] == bootstrap["lastSuccess"]

        # The manual rerun an operator is told to make.
        del harness.upstream.status_overrides[transitions.PAPER_PROJECT_PATH]
        code, payload, stderr = harness.run_command(run, "--publish")

        assert code == 0, stderr
        assert "create-issue" not in _actions(payload["scan"])
        code, recovered, stderr = show(run, harness)
        assert code == 0, stderr
        assert recovered["health"] == "ok"
        assert recovered["counts"]["failed"] == 0
        assert len(harness.support_issues()) == 1


def test_a_dashboard_that_moves_under_a_run_exits_nine_and_the_rerun_repairs_it(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Compare-and-swap at the tracker: a moved board is exit 9 and never a duplicate."""
    from takaro_maint.github import GitHub

    with lc.lifecycle_rig(catalog_copy, monkeypatch) as harness:
        assert harness.run_command(run, "--bootstrap", "--publish")[0] == 0
        before = len(harness.support_issues())
        # Work the repaired run has to record: the target landed on main between the two runs.
        harness.put_target_on_main(lc.target_for(catalog_copy, "26.3", status="maintained"))
        dashboard_number = int(harness.dashboard_issue()["number"])  # type: ignore[index]
        original = GitHub.issue_get

        def mutating(client: GitHub, number: int) -> Any:
            """Someone edits the dashboard between the run's read and its write."""
            if number == dashboard_number:
                for issue in harness.front.github.issues:
                    if issue["number"] == number:
                        issue["body"] = str(issue["body"]) + "\nsomeone else typed here\n"
            return original(client, number)

        monkeypatch.setattr(GitHub, "issue_get", mutating)
        code, payload, stderr = harness.run_command(run, "--publish")

        assert code == 9, stderr
        assert payload["exitCodes"] == {"scan": 9, "reconcile": None}
        assert "changed during the scan" in _error_of(payload), payload
        assert len(harness.support_issues()) == before

        monkeypatch.setattr(GitHub, "issue_get", original)
        code, payload, stderr = harness.run_command(run, "--publish")

        assert code == 0, stderr
        assert payload["reconcile"]["dashboard"]["updated"] is True
        assert harness.state_of() == "awaiting-release"
        assert len(harness.support_issues()) == before


# =============================================================================
# two providers in one tracker
# =============================================================================


@pytest.fixture
def steam(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Any:
    """The fake steamcmd serving a mutable copy of the recorded Conan Exiles document.

    Copied from ``test_game_conan_exiles`` rather than imported: a pytest fixture is bound to
    the module that defines it, and thirty lines here is cheaper than a shared conftest entry
    two suites would have to agree on.
    """

    class Rig:
        def __init__(self) -> None:
            self.root = tmp_path / "steam-root"
            self.log = tmp_path / "steamcmd-argv.jsonl"
            self.document = fake_steamcmd.recorded(CONAN_APP)
            self.root.mkdir(parents=True, exist_ok=True)
            self.serve()

        def serve(self) -> None:
            fake_steamcmd.serve(self.root, CONAN_APP, self.document)
            for name, value in fake_steamcmd.environment(self.root, self.log).items():
                monkeypatch.setenv(name, value)

    return Rig()


def _restore_conan_watch(root: Path) -> None:
    """Put back the watch the scan rig prunes: this scenario does serve Steam."""
    shipped = json.loads((REPO_ROOT / "catalog" / CONAN / "game.json").read_text(encoding="utf-8"))
    path = root / "catalog" / CONAN / "game.json"
    game = json.loads(path.read_text(encoding="utf-8"))
    game["sources"]["steam"]["watch"] = shipped["sources"]["steam"]["watch"]
    path.write_text(json.dumps(game, indent=2) + "\n", encoding="utf-8")


def _conan_target(root: Path) -> dict[str, Any]:
    """The shipped Conan record, re-pinned at the moved public head and marked maintained."""
    record = json.loads((root / "catalog" / CONAN / "targets" / "linux-25639945.json").read_text(encoding="utf-8"))
    record["id"] = f"linux-{CONAN_MOVED_BUILD}"
    record["revision"] = str(CONAN_MOVED_BUILD)
    record["support"] = {"status": "maintained", "since": "2026-09-17", "evidence": [], "notes": "test fixture"}
    server = record["inputs"]["server"]
    server["buildid"] = CONAN_MOVED_BUILD
    server["depots"][CONAN_CONTENT_DEPOT]["manifest"] = CONAN_MOVED_MANIFEST
    return record


def _conan_issue(harness: lc.Rig) -> dict[str, Any]:
    return harness.issue_with(kind="support", provider="steam", component=CONAN)


def test_cross_provider_issues_reconcile_independently(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch, steam: Any
) -> None:
    """A Mojang issue and a Steam issue share a tracker and move on their own evidence."""
    fake_steamcmd.move_head(
        steam.document,
        "public",
        CONAN_MOVED_BUILD,
        {CONAN_CONTENT_DEPOT: CONAN_MOVED_MANIFEST},
        timeupdated=1789900000,
    )
    steam.serve()

    with lc.lifecycle_rig(catalog_copy, monkeypatch) as harness:
        _restore_conan_watch(catalog_copy)

        code, payload, stderr = harness.run_command(run, "--bootstrap", "--publish")

        assert code == 0, stderr
        titles = sorted(str(issue["title"]) for issue in harness.support_issues())
        assert titles == [
            f"Conan Exiles public: build {CONAN_MOVED_BUILD} needs a target",
            "Minecraft 26.3: new stable release needs a target",
        ]
        work = harness.dashboard_state()["work"]
        assert any("provider=mojang" in key for key in work), work
        assert any("provider=steam" in key and CONAN in key for key in work), work

        conan_number = int(_conan_issue(harness)["number"])
        minecraft_number = int(harness.issue_with(kind="support", rev="26.3")["number"])

        def conan_state() -> str | None:
            return issues.existing_state(str(_conan_issue(harness)["body"]))

        def conan_is_untouched(payload: Any) -> None:
            assert str(_conan_issue(harness)["state"]) == "open"
            assert conan_state() == "detected"
            for reason in lc.reasons_of(payload, conan_number):
                assert CONAN in reason, reason

        # Walk only Minecraft from ready to released; Conan must not move with it.
        transitions.add_fabric_game(harness.upstream, "26.3")
        transitions.add_fabric_api(harness.upstream, "0.161.0+26.3")
        assert harness.scan(run, "--publish")[0] == 0
        assert harness.state_of() == "ready-for-agent"

        harness.open_pr(12, f"Refs #{minecraft_number}")
        code, payload, stderr = harness.reconcile(run, "--publish")
        assert code == 0, stderr
        assert harness.state_of() == "implementation-pr"
        conan_is_untouched(payload)

        harness.close_pr(12, merged_at="2026-09-18T11:00:00Z")
        record = lc.target_for(catalog_copy, "26.3", status="maintained")
        harness.put_target_on_main(record)
        code, payload, stderr = harness.reconcile(run, "--publish")
        assert code == 0, stderr
        assert harness.state_of() == "awaiting-release"
        conan_is_untouched(payload)

        lc.stable_release(harness, "minecraft-v0.2.0", "0.2.0", {"fabric-26.3": record})
        code, payload, stderr = harness.reconcile(run, "--publish")
        assert code == 0, stderr
        assert harness.state_of() == "released"
        minecraft = harness.issue_with(kind="support", rev="26.3")
        assert (minecraft["state"], minecraft["state_reason"]) == ("closed", "completed")
        conan_is_untouched(payload)

        # Now Conan's own evidence lands: a maintained target on main for the observed build.
        harness.put_target_on_main(_conan_target(catalog_copy))
        code, payload, stderr = harness.reconcile(run, "--publish")
        assert code == 0, stderr

        assert conan_state() == "awaiting-release"


# =============================================================================
# release and recovery
# =============================================================================


@pytest.fixture
def fake(monkeypatch: pytest.MonkeyPatch) -> Any:
    from test_release_assemble import REPO

    with serving(monkeypatch, REPO) as server:
        yield server


@pytest.fixture
def inputs(run: Any, catalog_copy: Path, tmp_path: Path) -> Inputs:
    return build_inputs(run, catalog_copy, tmp_path)


def test_required_target_evidence_blocks_stable_assembly_and_nothing_is_published(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    """Every Minecraft target requires ``protocol``: no evidence, no stable release."""
    stable_draft(fake, inputs.commit)
    target = "paper-1.21.11"
    reports = inputs.reports / target
    pristine = tmp_path / "pristine-report"
    shutil.copytree(reports, pristine)

    def drop() -> None:
        shutil.rmtree(reports)

    def failed() -> None:
        inputs.rewrite_report(target, outcome="fail")

    def under_level() -> None:
        inputs.rewrite_report(target, level="startup")

    for name, damage in (("missing", drop), ("failed", failed), ("under-level", under_level)):
        damage()
        out = tmp_path / f"assembled-{name}"

        code, payload, stderr = assemble(run, inputs, out)

        assert code == 8, f"{name}: {stderr}{payload}"
        assert payload["target"] == target, name
        assert not out.exists() or list(out.iterdir()) == [], name
        shutil.rmtree(reports, ignore_errors=True)
        shutil.copytree(pristine, reports)

    # The publish step is never reached, so the draft is still empty and nothing was uploaded.
    assert fake.asset_names(TAG) == []
    assert [path for method, path in fake.requests if "/uploads/" in path] == []


def test_tag_based_recovery_never_replaces_conflicting_bytes(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    """A recovery run re-uploads what is missing, skips what matches and refuses what differs."""
    directory, _, built = assemble_default(run, inputs, tmp_path)
    stable_draft(fake, built.commit)

    code, payload, stderr = publish(run, fake, directory, built.root, "--target-commit", built.commit)
    assert code == 0, stderr
    assert fake.release_for(TAG)["draft"] is False

    # The same commit, assembled again: byte-identical, so the recovery is a no-op.
    again = reassemble(run, built, tmp_path / "again", channel="stable", tag=TAG)
    before = len(fake.requests)
    code, payload, stderr = publish(run, fake, again, built.root, "--target-commit", built.commit)

    assert code == 0, stderr
    assert {asset["action"] for asset in payload["assets"]} == {"skipped-identical"}
    assert [path for method, path in fake.requests[before:] if "/uploads/" in path] == []

    # Someone replaced one published asset with other bytes.
    release = fake.release_for(TAG)
    names = sorted(path.name for path in directory.iterdir())
    victim = names[-1]
    release["assets"] = [asset for asset in release["assets"] if asset["name"] != victim]
    fake.add_asset(release, victim, b"someone else's bytes")

    before = len(fake.requests)
    code, payload, stderr = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7, stderr
    assert payload["conflicts"] == [victim]
    assert [path for method, path in fake.requests[before:] if "/uploads/" in path] == []
    assert [method for method, _ in fake.requests[before:] if method in ("POST", "PATCH", "DELETE")] == []
    assert fake.asset_names(TAG) == names
    assert fake.release_for(TAG)["draft"] is False

    code, payload, stderr = run(
        "release",
        "verify",
        "--tag",
        TAG,
        "--connector",
        CONNECTOR,
        "--repo",
        "o/r",
        "--api-url",
        fake.api_url,
        repo=built.root,
    )
    assert code == 7, stderr
    assert payload["asset"] == victim


def test_an_interrupted_stable_publication_resumes_to_completion(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    """The upload dies half way; the rerun uploads only what is missing and finalises."""
    directory, _, built = assemble_default(run, inputs, tmp_path)
    stable_draft(fake, built.commit)
    fake.fail_uploads_after(2)

    code, payload, stderr = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 9, stderr
    assert fake.release_for(TAG)["draft"] is True
    landed = fake.asset_names(TAG)
    assert len(landed) <= 2, landed

    fake.fail_uploads_after(10_000)
    code, payload, stderr = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 0, stderr
    actions = {asset["name"]: asset["action"] for asset in payload["assets"]}
    assert {name: actions[name] for name in landed} == dict.fromkeys(landed, "skipped-identical")
    assert sorted(name for name, action in actions.items() if action == "uploaded") == sorted(
        name for name in actions if name not in landed
    )
    assert fake.asset_names(TAG) == sorted(path.name for path in directory.iterdir())
    assert fake.release_for(TAG)["draft"] is False


# =============================================================================
# the evidence this module deliberately does not repeat
# =============================================================================

#: Exact inputs failing visibly, preserved installs and artifact/target mismatches are proven
#: here. This module runs them by name in CI rather than proving them again, so a rename has to
#: be noticed.
REUSED: dict[str, tuple[str, ...]] = {
    "test_cli_install": (
        "test_a_wrong_hash_exits_five_and_changes_nothing",
        "test_an_unavailable_input_exits_four_without_a_fallback_url",
        "test_a_failed_install_leaves_a_working_install_byte_identical",
        "test_protected_paths_survive_a_reinstall",
    ),
    "test_cli_artifact": (
        "test_a_jar_stamped_with_another_target_is_a_conflict",
        "test_a_jar_with_a_stale_fingerprint_is_a_conflict",
    ),
    "test_cli_deploy": (
        "test_a_manifest_for_another_target_is_a_conflict",
        "test_a_ledger_for_another_target_is_a_conflict",
        "test_an_interrupted_copy_leaves_the_previous_connector_in_place",
    ),
    "test_game_7d2d": (
        "test_steam_pin_reports_an_unavailable_manifest_as_upstream",
        "test_an_altered_reference_assembly_is_refused",
    ),
    "test_game_rust": (
        "test_an_altered_carbon_asset_is_refused_and_the_install_is_untouched",
        "test_an_unavailable_depot_manifest_exits_four_and_never_falls_back",
        "test_a_reinstall_preserves_server_data_plugins_and_configs_and_keeps_previous",
    ),
    "test_game_zomboid": (
        "test_a_corrupt_depot_file_leaves_the_install_untouched",
        "test_rollback_restores_the_previous_install",
        "test_deploy_into_a_directory_holding_another_fingerprint_is_refused",
    ),
    "test_game_valheim": (
        "test_install_refuses_a_pack_with_the_wrong_hash",
        "test_an_unavailable_pack_download_is_upstream",
        "test_an_upgrade_preserves_config_and_plugins_and_can_be_rolled_back",
    ),
    "test_game_conan_exiles": (
        "test_a_wrong_hash_or_missing_manifest_leaves_the_install_untouched",
        "test_preserve_keeps_saved_data_and_the_bridge_across_a_repin_and_rollback_restores",
        "test_a_manifest_built_for_another_target_is_refused",
    ),
    "test_game_terraria": (
        "test_install_places_the_pinned_distribution_and_refuses_altered_or_missing_bytes",
        "test_deploy_refuses_a_build_that_is_not_this_targets",
        "test_a_stable_image_tag_is_refused",
    ),
    "test_game_enshrouded": (
        "test_an_unavailable_manifest_exits_four_with_no_fallback",
        "test_a_wrong_declared_hash_exits_five_and_leaves_the_install_untouched",
        "test_preserve_keeps_config_saves_and_plugin_state_across_an_upgrade_and_rollback_restores",
        "test_every_container_selector_is_pinned_and_never_schedules_updates",
    ),
    "test_release_assemble": ("test_an_incomplete_set_exits_seven_and_names_the_missing_target_and_role",),
    "test_release_publish": ("test_a_conflict_on_a_published_release_leaves_it_untouched",),
    "test_scan_dedup": ("test_a_declined_issue_stays_declined",),
    "test_partial_provider_failure": ("test_a_failing_paper_source_keeps_its_checkpoint_and_exits_four",),
}


def test_the_reused_evidence_still_exists() -> None:
    """Every test this rollout relies on instead of repeating is still there under that name."""
    missing: list[str] = []
    for module_name, names in REUSED.items():
        module = importlib.import_module(module_name)
        missing += [f"{module_name}::{name}" for name in names if not callable(getattr(module, name, None))]
    assert missing == [], "reused evidence has been renamed or removed:\n" + "\n".join(missing)
