"""Steam discovery: running ``app_info_print`` and reading what it published.

The tool is replaced by ``fake_steamcmd.py`` — a real subprocess, invoked through the real
``TAKARO_MAINT_STEAMCMD`` override — so what is exercised here is the command line, the
retry, the log and the parse, not a mocked return value.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

import pytest

import fake_steamcmd
from takaro_maint.exit_codes import UPSTREAM, MaintError
from takaro_maint.steam import steamcmd

APP = 294420


@pytest.fixture
def steam(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Any:
    """The fake steamcmd, serving a mutable copy of the recorded 294420 document."""

    class Rig:
        def __init__(self) -> None:
            self.root = tmp_path / "steam-root"
            self.log = tmp_path / "steamcmd-argv.jsonl"
            self.tool_log = tmp_path / "logs" / f"app_info-{APP}.log"
            self.document = fake_steamcmd.recorded(APP)
            self.root.mkdir(parents=True, exist_ok=True)
            self.serve()

        def serve(self) -> None:
            fake_steamcmd.serve(self.root, APP, self.document)

        def env(self, **extra: str) -> None:
            for name, value in fake_steamcmd.environment(self.root, self.log, **extra).items():
                monkeypatch.setenv(name, value)

        @property
        def calls(self) -> list[list[str]]:
            return fake_steamcmd.argv_log(self.log)

    rig = Rig()
    rig.env()
    return rig


def test_the_recorded_app_is_read_through_the_real_command_line(steam: Any) -> None:
    info = steamcmd.app_info(APP, log=steam.tool_log)

    assert info.app == APP
    assert info.name == "7 Days to Die Dedicated Server"
    assert info.change_number == 39026857
    assert info.private_branches is True
    assert info.branches["public"].buildid == 24994542
    assert info.branches["public"].timeupdated == 1788196235
    assert info.depots["294422"].manifests["public"].gid == "1633674551820196085"
    assert info.depots["294422"].oslist == "linux"

    assert steam.calls == [
        [
            str(Path(fake_steamcmd.__file__).resolve()),
            "+login",
            "anonymous",
            "+app_info_update",
            "1",
            "+app_info_print",
            str(APP),
            "+quit",
        ]
    ], "the argv is the documented read-only, anonymous invocation"


def test_only_pinnable_depots_are_reported(steam: Any) -> None:
    """Shared depots and the bare scalars beside them are not content this can pin."""
    info = steamcmd.app_info(APP, log=steam.tool_log)

    assert sorted(info.depots) == ["294421", "294422"]
    assert "branches" not in info.depots
    assert "overridescddb" not in info.depots


def test_a_branch_without_timeupdated_falls_back_to_the_build_time(steam: Any) -> None:
    info = steamcmd.app_info(APP, log=steam.tool_log)

    old = info.branches["alpha12.5"]
    assert old.timeupdated is None
    assert old.published_at == old.timebuildupdated == 1440323524
    assert steamcmd.iso(old.published_at or 0) == "2015-08-23T09:52:04Z"


def test_a_protected_branch_is_reported_as_one(steam: Any) -> None:
    fake_steamcmd.add_branch(
        steam.document, "latest_experimental", 25200000, {"294422": "3000000000000000001"}, pwdrequired=True
    )
    steam.serve()

    info = steamcmd.app_info(APP, log=steam.tool_log)

    branch = info.branches["latest_experimental"]
    assert branch.pwdrequired is True
    assert "latest_experimental" in info.depots["294422"].encrypted
    assert "latest_experimental" not in info.depots["294422"].manifests


# -- the documented non-TTY truncation -----------------------------------------
def test_a_truncated_first_answer_is_retried_exactly_once(steam: Any) -> None:
    steam.env(FAKE_STEAMCMD_TRUNCATE="1")

    info = steamcmd.app_info(APP, log=steam.tool_log)

    assert info.branches["public"].buildid == 24994542
    assert len(steam.calls) == 2


def test_two_truncated_answers_are_an_upstream_failure_and_not_a_third_attempt(steam: Any) -> None:
    steam.env(FAKE_STEAMCMD_TRUNCATE="5")

    with pytest.raises(MaintError) as raised:
        steamcmd.app_info(APP, log=steam.tool_log)

    assert raised.value.code == UPSTREAM
    assert "after 2 attempts" in raised.value.message
    assert len(steam.calls) == 2, "a broken upstream is reported, not hammered"


# -- the other ways the tool fails ---------------------------------------------
def test_a_failing_steamcmd_is_an_upstream_failure_with_its_log(steam: Any) -> None:
    steam.env(FAKE_STEAMCMD_FAIL="1")

    with pytest.raises(MaintError) as raised:
        steamcmd.app_info(APP, log=steam.tool_log)

    assert raised.value.code == UPSTREAM
    assert "exited 1" in raised.value.message
    assert steam.tool_log.name in raised.value.message
    assert "Connection failed" in steam.tool_log.read_text(encoding="utf-8")
    assert len(steam.calls) == 1, "a non-zero exit is not the retryable failure"


def test_a_missing_tool_names_the_override(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setenv("TAKARO_MAINT_STEAMCMD", str(tmp_path / "there-is-no-steamcmd-here"))

    with pytest.raises(MaintError) as raised:
        steamcmd.app_info(APP)

    assert raised.value.code == UPSTREAM
    assert "TAKARO_MAINT_STEAMCMD" in raised.value.message


def test_a_hanging_tool_times_out(steam: Any) -> None:
    steam.env(FAKE_STEAMCMD_HANG="5")

    with pytest.raises(MaintError) as raised:
        steamcmd.app_info(APP, timeout=0.5, log=steam.tool_log)

    assert raised.value.code == UPSTREAM
    assert "timed out" in raised.value.message


def test_an_unknown_app_prints_no_branch_data(steam: Any) -> None:
    with pytest.raises(MaintError) as raised:
        steamcmd.app_info(999999, log=steam.tool_log.parent / "app_info-999999.log")

    assert raised.value.code == UPSTREAM
    assert "999999" in raised.value.message


# -- what the run leaves behind ------------------------------------------------
def test_the_log_records_the_command_and_its_exit(steam: Any) -> None:
    steamcmd.app_info(APP, log=steam.tool_log)

    recorded = steam.tool_log.read_text(encoding="utf-8")
    assert "+app_info_print 294420" in recorded
    assert "-- exit 0" in recorded


def test_a_secret_in_the_environment_never_reaches_the_log(
    steam: Any, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """No credential is an argument of steamcmd, but the log is redacted regardless."""
    monkeypatch.setenv("TAKARO_MAINT_STEAM_BRANCH_PASSWORD__294420__X", "hunter2-secret-value")
    root = tmp_path / "leaky"
    document = fake_steamcmd.recorded(APP)
    fake_steamcmd.add_branch(document, "leak", 1, {"294422": "1"}, description="hunter2-secret-value")
    fake_steamcmd.serve(root, APP, document)
    monkeypatch.setenv(fake_steamcmd.ROOT_ENV, str(root))

    steamcmd.app_info(APP, log=steam.tool_log)

    assert "hunter2-secret-value" not in steam.tool_log.read_text(encoding="utf-8")


# -- the revision digest -------------------------------------------------------
def test_a_pathologically_nested_app_info_is_a_failed_source_not_a_traceback(steam: Any) -> None:
    """A `RecursionError` is not a `VdfError`; `app_info` maps it to a failed source."""
    from takaro_maint.exit_codes import UPSTREAM, MaintError

    deep = f'"{APP}"' + "\n{\n" + '"a"\n{\n' * 2000
    (steam.root / f"{APP}.vdf").write_text(deep, encoding="utf-8")

    with pytest.raises(MaintError) as caught:
        steamcmd.app_info(APP, log=steam.tool_log)

    assert caught.value.code == UPSTREAM
    assert "nesting deeper than" in caught.value.message


def test_the_manifest_digest_binds_the_whole_depot_set() -> None:
    one = steamcmd.manifest_digest({"294422": "1633674551820196085"})
    two = steamcmd.manifest_digest({"294422": "1633674551820196085", "294421": "1646211645575803407"})

    assert len(one) == 8 and one != two
    assert one == steamcmd.manifest_digest({"294422": "1633674551820196085"}), "stable across calls"
    assert two == steamcmd.manifest_digest({"294421": "1646211645575803407", "294422": "1633674551820196085"}), (
        "and independent of the order the depots were listed in"
    )
    assert steamcmd.manifest_digest({"294422": "2089214172134382404"}) != one


def test_the_fake_serves_a_document_a_test_wrote(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The fixture is bent in memory and written back through the real VDF writer."""
    root = tmp_path / "root"
    document = fake_steamcmd.recorded(APP)
    fake_steamcmd.move_head(document, "public", 25100000, {"294422": "2089214172134382404"}, timeupdated=1788300000)
    fake_steamcmd.set_private_branches(document, False)
    fake_steamcmd.remove_branch(document, "alpha12.5")
    fake_steamcmd.serve(root, APP, document)
    for name, value in fake_steamcmd.environment(root, tmp_path / "argv.jsonl").items():
        monkeypatch.setenv(name, value)

    info = steamcmd.app_info(APP)

    assert info.branches["public"].buildid == 25100000
    assert info.branches["public"].timeupdated == 1788300000
    assert info.depots["294422"].manifests["public"].gid == "2089214172134382404"
    assert info.private_branches is False
    assert "alpha12.5" not in info.branches
    assert json.dumps(sorted(info.branches)) == '["alpha21.2", "public", "v3.1.0", "v3.2.0"]'


# -- `steam branches` and `steam pin --metadata` -------------------------------
import fake_depotdownloader as fake_dd  # noqa: E402

GAME = "7d2d"


@pytest.fixture
def repo(tmp_path: Path, steam: Any, monkeypatch: pytest.MonkeyPatch) -> Path:
    """A repository copy with the DepotDownloader stand-in wired up beside the steamcmd one."""
    root = fake_dd.make_repo(tmp_path)
    for name, value in fake_dd.environment(tmp_path, tmp_path / "dd-argv.jsonl").items():
        monkeypatch.setenv(name, value)
    return root


def watch_7d2d(root: Path, watch: dict[str, Any]) -> None:
    """Give the copied 7 Days to Die record a Steam watch block."""
    path = root / "catalog" / GAME / "game.json"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["sources"]["steam"]["watch"] = watch
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def test_steam_branches_lists_every_branch_the_app_publishes(run: Any, steam: Any, repo: Path) -> None:
    fake_steamcmd.add_branch(steam.document, "beta_test", 25300000, {"294422": "3000000000000000001"})
    steam.serve()

    code, payload, err = run("steam", "branches", "--game", GAME, "--app", "294420", repo=repo)

    assert code == 0, err
    assert payload["app"] == 294420
    assert payload["name"] == "7 Days to Die Dedicated Server"
    assert payload["privateBranches"] is True
    assert payload["changeNumber"] == 39026857
    labels = [row["label"] for row in payload["branches"]]
    assert labels == sorted(labels), "sorted by label, so two runs are diffable"
    assert "beta_test" in labels

    public = next(row for row in payload["branches"] if row["label"] == "public")
    assert public["buildid"] == 24994542
    assert public["timeupdated"] == "2026-08-31T17:10:35Z"
    assert public["manifests"]["294422"]["gid"] == "1633674551820196085"
    assert public["pwdrequired"] is False
    assert [depot["id"] for depot in payload["depots"]] == ["294422"], (
        "the watch block says which depots make the identity, and those are the ones listed"
    )

    code, payload, err = run(
        "steam", "branches", "--game", GAME, "--app", "294420", "--depot", "294421", "--depot", "294422", repo=repo
    )

    assert code == 0, err
    assert [depot["id"] for depot in payload["depots"]] == ["294421", "294422"], "--depot overrides the watch block"


def test_steam_branches_classifies_against_the_watch_block(run: Any, steam: Any, repo: Path) -> None:
    fake_steamcmd.add_branch(
        steam.document, "latest_experimental", 25200000, {"294422": "3000000000000000001"}, pwdrequired=True
    )
    fake_steamcmd.add_branch(steam.document, "beta_test", 25300000, {"294422": "4000000000000000001"})
    steam.serve()
    watch_7d2d(
        repo,
        {
            "kind": "game",
            "component": GAME,
            "app": 294420,
            "os": "linux",
            "depots": ["294422"],
            "channels": {
                "public": {"branch": "public"},
                "latest_experimental": {"branch": "experimental", "enabled": False},
            },
            "knownBranches": ["regex:^v[0-9]+(\\.[0-9]+)*$", "regex:^alpha[0-9]+(\\.[0-9]+)*$"],
        },
    )

    code, payload, err = run("steam", "branches", "--game", GAME, repo=repo)

    assert code == 0, err
    by_label = {row["label"]: row for row in payload["branches"]}
    assert by_label["public"]["classification"] == "watched"
    assert by_label["public"]["branch"] == "public"
    assert by_label["latest_experimental"]["classification"] == "declared"
    assert by_label["latest_experimental"]["pwdrequired"] is True
    assert by_label["latest_experimental"]["manifests"] is None
    assert by_label["latest_experimental"]["encrypted"] == ["294422"]
    assert by_label["v3.2.0"]["classification"] == "known"
    assert by_label["alpha12.5"]["classification"] == "known"
    assert by_label["beta_test"]["classification"] == "unfamiliar"


def test_steam_branches_takes_the_app_from_the_watch_block(run: Any, steam: Any, repo: Path) -> None:
    code, payload, err = run("steam", "branches", "--game", GAME, repo=repo)

    assert code == 0, err
    assert payload["app"] == 294420


def test_steam_branches_without_an_app_anywhere_is_a_usage_error(run: Any, steam: Any, repo: Path) -> None:
    path = repo / "catalog" / GAME / "game.json"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["sources"]["steam"]["watch"].pop("app")
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    code, payload, _ = run("steam", "branches", "--game", GAME, repo=repo)

    assert code == 2
    assert "--app" in json.dumps(payload)


def test_steam_branches_reports_a_failing_tool_as_upstream(run: Any, steam: Any, repo: Path) -> None:
    steam.env(FAKE_STEAMCMD_FAIL="1")

    code, payload, _ = run("steam", "branches", "--game", GAME, "--app", "294420", repo=repo)

    assert code == 4
    assert "steamcmd" in json.dumps(payload)


def test_steam_pin_metadata_fills_the_buildid_from_app_info(run: Any, steam: Any, repo: Path) -> None:
    """The build id is in the metadata and nowhere else: DepotDownloader never sees one."""
    fake_steamcmd.set_manifest(steam.document, "294422", "public", fake_dd.HEAD_MANIFEST)
    steam.serve()

    code, payload, err = run("steam", "pin", "--game", GAME, "--target", fake_dd.TARGET_ID, "--metadata", repo=repo)

    assert code == 0, err
    assert payload["metadata"] == {
        "buildid": 24994542,
        "timeupdated": "2026-08-31T17:10:35Z",
        "description": None,
        "changeNumber": 39026857,
        "crossChecked": ["294422"],
    }
    assert payload["buildid"] == 24994542
    assert payload["snippet"]["buildid"] == 24994542
    assert payload["depots"]["294422"]["manifest"] == fake_dd.HEAD_MANIFEST


def test_a_metadata_pin_says_which_depots_it_could_cross_check(run: Any, steam: Any, repo: Path) -> None:
    """A protected branch publishes its manifest ids encrypted, so there is nothing to compare.

    That is not a disagreement and must not fail the pin -- but it is also not the
    cross-check, so the run says which depots it actually compared instead of leaving the
    reader to assume all of them were.
    """
    fake_steamcmd.set_manifest(steam.document, "294422", "public", fake_dd.HEAD_MANIFEST, encrypted=True)
    steam.serve()

    code, payload, err = run("steam", "pin", "--game", GAME, "--target", fake_dd.TARGET_ID, "--metadata", repo=repo)

    assert code == 0, err
    assert payload["metadata"]["buildid"] == 24994542
    assert payload["metadata"]["crossChecked"] == [], "an encrypted manifest cannot be compared"


def test_a_publish_in_flight_is_a_retry_not_a_record(run: Any, steam: Any, repo: Path) -> None:
    """The metadata and the depot are read a moment apart; disagreeing means retry."""
    code, payload, _ = run("steam", "pin", "--game", GAME, "--target", fake_dd.TARGET_ID, "--metadata", repo=repo)

    assert code == 4
    message = json.dumps(payload)
    assert "disagree" in message
    assert "a publish is in flight" in message


def test_metadata_and_an_explicit_buildid_are_a_usage_error(run: Any, steam: Any, repo: Path) -> None:
    code, payload, _ = run(
        "steam", "pin", "--game", GAME, "--target", fake_dd.TARGET_ID, "--metadata", "--buildid", "1", repo=repo
    )

    assert code == 2
    assert "--metadata" in json.dumps(payload)


def test_steam_pin_without_metadata_is_unchanged(run: Any, steam: Any, repo: Path) -> None:
    """No app_info call, no metadata block."""
    code, payload, err = run("steam", "pin", "--game", GAME, "--target", fake_dd.TARGET_ID, repo=repo)

    assert code == 0, err
    assert payload["metadata"] is None
    assert steam.calls == [], "steamcmd is not consulted unless --metadata asks for it"


# -- observing a branch head: `scan` end to end --------------------------------
# Everything below drives the real ``scan`` command against the real tracker fakes, with
# the real fake tool answering as steamcmd. What is asserted is what a maintainer sees:
# exit codes, the report on stdout, and the issues the tracker ends up holding.
import test_scan_support as scan_support  # noqa: E402
from conftest import REPO_ROOT  # noqa: E402
from fake_github import FakeGitHub  # noqa: E402
from takaro_maint import channels  # noqa: E402
from takaro_maint.providers import steam as steam_provider  # noqa: E402
from takaro_maint.tracker import identity  # noqa: E402

STEAM_KEY = f"{GAME}/steam"
PINNED_BUILD = 24994542
PINNED_MANIFEST = "1633674551820196085"
SECOND_MANIFEST = "2089214172134382404"


def rev_of(buildid: int, manifests: dict[str, str], branch: str = "public") -> str:
    """The revision the provider builds for one head, spelled out by the test."""
    return f"{buildid}.{steamcmd.manifest_digest(manifests)}+{branch}"


PINNED_REV = rev_of(PINNED_BUILD, {"294422": PINNED_MANIFEST})


class ScanRig(scan_support.Rig):
    """The shared scan rig with Steam in place of Mojang: no HTTP upstream, a real tool."""

    def scan(self, run: Any, *flags: str) -> tuple[int, Any, str]:
        return run(
            "scan",
            "--game",
            GAME,
            "--source",
            "steam",
            "--repo",
            scan_support.REPO,
            "--api-url",
            self.fake.api_url,
            *flags,
            repo=self.root,
        )

    def titles(self) -> list[str]:
        return [str(issue["title"]) for issue in self.support_issues()]

    def body(self, index: int = 0) -> str:
        return str(self.support_issues()[index]["body"])


@pytest.fixture
def tracker(steam: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> Any:
    """The catalog as it ships, a tracker stand-in, and the clock pinned."""
    scan_support.frozen_clock(monkeypatch)
    monkeypatch.setenv("GH_TOKEN", scan_support.TOKEN)
    monkeypatch.delenv(steam_provider.password_env(APP, "latest_experimental"), raising=False)
    with FakeGitHub() as fake:
        yield ScanRig(root=catalog_copy, upstream=None, fake=fake)  # type: ignore[arg-type]


def watch_of(root: Path) -> dict[str, Any]:
    record = json.loads((root / "catalog" / GAME / "game.json").read_text(encoding="utf-8"))
    return dict(record["sources"]["steam"]["watch"])


def set_watch(root: Path, watch: dict[str, Any]) -> None:
    path = root / "catalog" / GAME / "game.json"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["sources"]["steam"]["watch"] = watch
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def restore_watch(root: Path) -> None:
    """Put the shipped Steam watch block back into a catalog copy that dropped it.

    A rig built for another game's fixture is free to prune the watch blocks it cannot
    serve, and one of them is this game's. These scenarios are precisely about a Steam
    source sitting beside that fixture, so they ask for it back — from the record the
    repository actually ships, never from a literal, so the block under test is the one
    that ships.
    """
    shipped = json.loads((REPO_ROOT / "catalog" / GAME / "game.json").read_text(encoding="utf-8"))
    set_watch(root, dict(shipped["sources"]["steam"]["watch"]))


def enable_experimental(root: Path) -> None:
    """Start watching the opt-in channel the catalog ships disabled."""
    watch = watch_of(root)
    watch["channels"] = {
        **watch["channels"],
        "latest_experimental": {"branch": "experimental"},
    }
    set_watch(root, watch)


# -- T-S1/T-S9/T-S19 bootstrap -------------------------------------------------
def test_bootstrap_seeds_the_public_head_and_files_nothing_for_the_pinned_build(
    run: Any, tracker: Any, steam: Any
) -> None:
    """The catalog already pins exactly this app, branch, build and manifest."""
    code, payload, err = tracker.scan(run, "--bootstrap")

    assert code == 0, err
    assert payload["observations"] == []
    assert payload["sources"][STEAM_KEY]["status"] == "ok"
    assert payload["sources"][STEAM_KEY]["history"] == "heads-only"
    assert payload["sources"][STEAM_KEY]["heads"] == {"public": PINNED_REV}
    bootstrap = next(entry for entry in payload["plan"] if entry["action"] == "bootstrap-checkpoint")
    assert bootstrap["seeded"] == 1, "one head, not one manifest and not one branch label"
    assert [entry["action"] for entry in payload["plan"] if entry["action"] == "create-issue"] == []
    assert tracker.support_issues() == []
    assert tracker.fake.writes == 0


def test_a_read_only_scan_plans_the_issue_and_writes_nothing(run: Any, tracker: Any, steam: Any) -> None:
    fake_steamcmd.move_head(steam.document, "public", 25100000, {"294422": SECOND_MANIFEST})
    steam.serve()

    code, payload, err = tracker.scan(run, "--bootstrap")

    assert code == 0, err
    assert [entry["action"] for entry in payload["plan"]].count("create-issue") == 1
    assert payload["applied"] == []
    assert tracker.fake.writes == 0


def test_bootstrap_files_the_uncovered_public_head(run: Any, tracker: Any, steam: Any) -> None:
    fake_steamcmd.move_head(steam.document, "public", 25100000, {"294422": SECOND_MANIFEST}, timeupdated=1788300000)
    steam.serve()

    code, payload, err = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 0, err
    expected = rev_of(25100000, {"294422": SECOND_MANIFEST})
    assert [observation["rev"] for observation in payload["observations"]] == [expected]
    assert tracker.titles() == ["7 Days to Die public: build 25100000 needs a target"]

    body = tracker.body()
    assert body.splitlines()[0] == (
        "<!-- takaro-maint: kind=support provider=steam component=7d2d app=294420 "
        f"branch=public buildid=25100000 rev={expected} -->"
    )
    assert "| Build id | 25100000 |" in body
    assert f"| Depot 294422 manifest | `{SECOND_MANIFEST}`" in body
    assert "| App | `294420` (7 Days to Die Dedicated Server) |" in body
    assert steam_provider.OBSERVATION_LIMIT in body
    assert "| Credentials | anonymous |" in body
    assert "<!-- takaro-maint:state=detected -->" in body
    assert "steam pin --game 7d2d --branch public --metadata" in body
    assert "Steam moved the `public` branch of app 294420" in body
    # 7D2D declares no `watch.readinessNote`, so the generic sentence is what it gets.
    assert "no framework layer" in body
    assert [issue["labels"] for issue in tracker.support_issues()] == [["connector-maintenance"]]
    assert (
        tracker.dashboard_state()["work"][f"provider=steam component=7d2d branch=public rev={expected}"]["state"]
        == "detected"
    )


def test_a_rescan_files_nothing_twice(run: Any, tracker: Any, steam: Any) -> None:
    fake_steamcmd.move_head(steam.document, "public", 25100000, {"294422": SECOND_MANIFEST}, timeupdated=1788300000)
    steam.serve()
    tracker.scan(run, "--bootstrap", "--publish")
    before = len(tracker.fake.issues)

    code, payload, err = tracker.scan(run, "--publish")

    assert code == 0, err
    assert payload["observations"] == []
    assert [entry["action"] for entry in payload["applied"]] == ["update-dashboard"]
    assert len(tracker.fake.issues) == before


# -- T-S4 several branches -----------------------------------------------------
def test_multiple_branches_are_observed_independently(run: Any, tracker: Any, steam: Any) -> None:
    enable_experimental(tracker.root)
    fake_steamcmd.add_branch(steam.document, "latest_experimental", 25200000, {"294422": "3000000000000000001"})
    steam.serve()

    code, payload, err = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 0, err
    experimental = rev_of(25200000, {"294422": "3000000000000000001"}, "experimental")
    assert payload["sources"][STEAM_KEY]["heads"] == {"public": PINNED_REV, "experimental": experimental}
    assert [observation["branch"] for observation in payload["observations"]] == ["experimental"]
    assert tracker.titles() == ["7 Days to Die latest_experimental: build 25200000 needs a target"]

    # Closing the experimental issue leaves the (covered) public head alone.
    tracker.support_issues()[0]["state"] = "closed"
    tracker.support_issues()[0]["state_reason"] = "completed"
    code, payload, err = tracker.scan(run, "--publish")
    assert code == 0, err
    assert payload["observations"] == []


# -- T-S5/T-S6/T-S7/T-S8 protected and hidden branches -------------------------
def test_a_protected_branch_without_credentials_is_a_failed_observation(run: Any, tracker: Any, steam: Any) -> None:
    enable_experimental(tracker.root)
    fake_steamcmd.add_branch(
        steam.document, "latest_experimental", 25200000, {"294422": "3000000000000000001"}, pwdrequired=True
    )
    steam.serve()

    code, payload, err = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 4, err
    entry = payload["sources"][STEAM_KEY]
    assert entry["status"] == "failed"
    assert "TAKARO_MAINT_STEAM_BRANCH_PASSWORD__294420__LATEST_EXPERIMENTAL" in entry["error"]
    assert entry["checkpoint"]["after"] is None, "a failed source advances nothing"
    assert tracker.support_issues() == [], "a branch that could not be read files nothing"


def test_a_hidden_branch_is_a_failed_observation(run: Any, tracker: Any, steam: Any) -> None:
    """The experimental branch of 294420 is private today: it is simply not listed."""
    enable_experimental(tracker.root)

    code, payload, err = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 4, err
    error = payload["sources"][STEAM_KEY]["error"]
    assert "not listed by app_info" in error
    assert "privatebranches=1" in error


#: The head `latest_experimental` publishes, distinct from the public branch's.
EXPERIMENTAL_MANIFEST = "3000000000000000001"


def test_a_protected_branch_without_its_password_is_a_failed_source(
    run: Any, tracker: Any, steam: Any, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """No password, no observation -- not an observation of whatever the public head is."""
    for name, value in fake_dd.environment(tmp_path, tmp_path / "dd-argv.jsonl").items():
        monkeypatch.setenv(name, value)
    monkeypatch.setenv(
        "FAKE_DD_BRANCHES",
        json.dumps({"latest_experimental": {"depots": {"294422": EXPERIMENTAL_MANIFEST}, "protected": True}}),
    )
    enable_experimental(tracker.root)
    fake_steamcmd.add_branch(
        steam.document, "latest_experimental", 25200000, {"294422": EXPERIMENTAL_MANIFEST}, pwdrequired=True
    )
    steam.serve()
    monkeypatch.delenv(steam_provider.password_env(APP, "latest_experimental"), raising=False)

    code, payload, _ = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 4, payload
    source = payload["sources"][STEAM_KEY]
    assert source["status"] == "failed"
    assert steam_provider.password_env(APP, "latest_experimental") in source["error"]
    assert not [o for o in payload.get("observations", []) if o.get("branch") == "experimental"]


def test_credentials_resolve_a_protected_branch_and_never_appear_anywhere(
    run: Any, tracker: Any, steam: Any, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Both a long password and a four-character one: neither reaches any output."""
    for name, value in fake_dd.environment(tmp_path, tmp_path / "dd-argv.jsonl").items():
        monkeypatch.setenv(name, value)
    # The branch publishes its own head. Without this the tool answered with the *public*
    # head whatever `-branch` said, so the test could not tell a password that worked from
    # one that was ignored.
    monkeypatch.setenv(
        "FAKE_DD_BRANCHES",
        json.dumps({"latest_experimental": {"depots": {"294422": EXPERIMENTAL_MANIFEST}, "protected": True}}),
    )
    for password in ("hunter2-secret-value", "ab12"):
        with FakeGitHub() as fake:
            harness = ScanRig(root=tracker.root, upstream=None, fake=fake)  # type: ignore[arg-type]
            enable_experimental(harness.root)
            document = fake_steamcmd.recorded(APP)
            fake_steamcmd.add_branch(
                document, "latest_experimental", 25200000, {"294422": EXPERIMENTAL_MANIFEST}, pwdrequired=True
            )
            fake_steamcmd.serve(steam.root, APP, document)
            monkeypatch.setenv(steam_provider.password_env(APP, "latest_experimental"), password)
            out = tmp_path / f"scan-{len(password)}.json"

            code, payload, err = harness.scan(run, "--bootstrap", "--publish", "--out", str(out))

            assert code == 0, err
            observation = next(o for o in payload["observations"] if o["branch"] == "experimental")
            assert observation["facts"]["depots"]["294422"]["manifest"] == EXPERIMENTAL_MANIFEST
            assert observation["facts"]["credentialsEnv"] == (
                "TAKARO_MAINT_STEAM_BRANCH_PASSWORD__294420__LATEST_EXPERIMENTAL"
            )
            assert any("-branchpassword" in call for call in fake_dd.argv_log(tmp_path / "dd-argv.jsonl"))

            written = [json.dumps(payload), err, out.read_text(encoding="utf-8")]
            written += [str(issue["title"]) + str(issue["body"]) for issue in fake.issues]
            written += [
                path.read_text(encoding="utf-8")
                for path in (Path(os.environ["TAKARO_MAINT_CACHE"]) / "steam" / "logs").rglob("*")
                if path.is_file()
            ]
            for text in written:
                assert password not in text


def test_a_channel_may_name_the_variable_its_password_comes_from(
    run: Any, tracker: Any, steam: Any, monkeypatch: pytest.MonkeyPatch
) -> None:
    watch = watch_of(tracker.root)
    watch["channels"] = {
        **watch["channels"],
        "latest_experimental": {"branch": "experimental", "passwordEnv": "SEVEN_EXP_PW"},
    }
    set_watch(tracker.root, watch)
    fake_steamcmd.add_branch(
        steam.document, "latest_experimental", 25200000, {"294422": "3000000000000000001"}, pwdrequired=True
    )
    steam.serve()
    monkeypatch.delenv("SEVEN_EXP_PW", raising=False)

    code, payload, _ = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 4
    error = payload["sources"][STEAM_KEY]["error"]
    assert "SEVEN_EXP_PW" in error
    assert "TAKARO_MAINT_STEAM_BRANCH_PASSWORD" not in error


# -- T-S10/T-S11/T-S12 what a moving head means --------------------------------
def test_rollback_is_detected_by_identity_and_never_by_build_number(run: Any, tracker: Any, steam: Any) -> None:
    tracker.scan(run, "--bootstrap", "--publish")

    fake_steamcmd.move_head(steam.document, "public", 25100000, {"294422": SECOND_MANIFEST}, timeupdated=1788300000)
    steam.serve()
    code, _, err = tracker.scan(run, "--publish")
    assert code == 0, err
    moved = rev_of(25100000, {"294422": SECOND_MANIFEST})

    fake_steamcmd.move_head(steam.document, "public", PINNED_BUILD, {"294422": PINNED_MANIFEST}, timeupdated=1788196235)
    steam.serve()
    code, payload, err = tracker.scan(run, "--publish")

    assert code == 0, err
    assert [observation["rev"] for observation in payload["observations"]] == [channels.rollback_rev(PINNED_REV, moved)]
    assert tracker.titles()[-1] == f"7 Days to Die public: rolled back to build {PINNED_BUILD}"
    assert f"| Rolled back from | build `{moved}` |" in tracker.body(-1)

    # A build nobody has ever seen is new, however small its number is.
    fake_steamcmd.move_head(
        steam.document, "public", 24000001, {"294422": "5000000000000000001"}, timeupdated=1788400000
    )
    steam.serve()
    code, payload, err = tracker.scan(run, "--publish")

    assert code == 0, err
    assert tracker.titles()[-1] == "7 Days to Die public: build 24000001 needs a target"


def test_a_build_promoted_to_public_is_a_new_public_observation(run: Any, tracker: Any, steam: Any) -> None:
    enable_experimental(tracker.root)
    manifests = {"294422": "3000000000000000001"}
    fake_steamcmd.add_branch(steam.document, "latest_experimental", 25200000, manifests)
    steam.serve()
    code, _, err = tracker.scan(run, "--bootstrap", "--publish")
    assert code == 0, err
    experimental_issue = tracker.support_issues()[0]["number"]

    fake_steamcmd.move_head(steam.document, "public", 25200000, manifests, timeupdated=1788400000)
    steam.serve()
    code, payload, err = tracker.scan(run, "--publish")

    assert code == 0, err
    promoted = next(o for o in payload["observations"] if o["branch"] == "public")
    assert promoted["facts"]["promotedFrom"] == "experimental"
    assert tracker.titles()[-1] == "7 Days to Die public: build 25200000 promoted from experimental"
    assert [entry for entry in payload["applied"] if entry.get("issue") == experimental_issue] == []


def test_a_new_manifest_under_the_same_build_is_a_new_revision(run: Any, tracker: Any, steam: Any) -> None:
    """A publisher can replace a depot's content without moving the build id."""
    tracker.scan(run, "--bootstrap", "--publish")

    fake_steamcmd.set_manifest(steam.document, "294422", "public", "6000000000000000001")
    steam.serve()
    code, payload, err = tracker.scan(run, "--publish")

    assert code == 0, err
    expected = rev_of(PINNED_BUILD, {"294422": "6000000000000000001"})
    assert [observation["rev"] for observation in payload["observations"]] == [expected]
    assert expected != PINNED_REV
    assert "`6000000000000000001`" in tracker.body(-1)


# -- T-S13/T-S14 a branch nobody declared --------------------------------------
def test_an_unfamiliar_branch_becomes_a_review_candidate(run: Any, tracker: Any, steam: Any) -> None:
    fake_steamcmd.add_branch(steam.document, "beta_test", 25300000, {"294422": "7000000000000000001"})
    steam.serve()

    code, payload, err = tracker.scan(run, "--bootstrap", "--publish")

    assert code == 0, err
    review = next(o for o in payload["observations"] if o["kind"] == "branch-review")
    assert review["branch"] == "beta-test", "the marker alphabet has no underscore in it"
    assert review["rev"] == rev_of(25300000, {"294422": "7000000000000000001"}, "beta-test")
    assert review["facts"]["channel"] == "beta_test"
    assert tracker.fake.issues[0]["title"] == (
        f"7 Days to Die 7d2d: unrecognised upstream branch 'beta-test' ({review['rev']}) needs review"
    )
    assert "<!-- takaro-maint:state=review -->" in str(tracker.fake.issues[0]["body"])
    assert [o["branch"] for o in payload["observations"]] == ["beta-test"], (
        "the tagged version branches are knownBranches and are never reviewed"
    )


def test_a_declined_review_is_never_refiled_and_a_new_build_is_a_new_question(
    run: Any, tracker: Any, steam: Any
) -> None:
    fake_steamcmd.add_branch(steam.document, "beta_test", 25300000, {"294422": "7000000000000000001"})
    steam.serve()
    tracker.scan(run, "--bootstrap", "--publish")
    review = tracker.fake.issues[0]
    review["state"] = "closed"
    review["state_reason"] = "not_planned"
    tracker.forget(rev_of(25300000, {"294422": "7000000000000000001"}, "beta-test"), STEAM_KEY)
    writes = tracker.fake.writes

    code, payload, err = tracker.scan(run, "--publish")

    assert code == 0, err
    assert [entry["reason"] for entry in payload["applied"] if entry["action"] == "noop"] == ["declined"]

    fake_steamcmd.move_head(steam.document, "beta_test", 25400000, {"294422": "8000000000000000001"})
    steam.serve()
    code, _, err = tracker.scan(run, "--publish")

    assert code == 0, err
    assert len([issue for issue in tracker.fake.issues if "unrecognised upstream branch" in str(issue["title"])]) == 2
    assert tracker.fake.writes > writes


# -- T-S15 closed support issues ------------------------------------------------
def test_closed_support_issues_are_left_alone(run: Any, tracker: Any, steam: Any) -> None:
    fake_steamcmd.move_head(steam.document, "public", 25100000, {"294422": SECOND_MANIFEST}, timeupdated=1788300000)
    steam.serve()
    tracker.scan(run, "--bootstrap", "--publish")
    moved = rev_of(25100000, {"294422": SECOND_MANIFEST})

    for reason in ("completed", "not_planned"):
        issue = tracker.support_issues()[0]
        issue["state"] = "closed"
        issue["state_reason"] = reason
        tracker.forget(moved, STEAM_KEY)
        writes = tracker.fake.writes

        code, payload, err = tracker.scan(run, "--publish")

        assert code == 0, err
        actions = [entry["reason"] for entry in payload["applied"] if entry["action"] == "noop"]
        assert actions == ["closed-completed" if reason == "completed" else "declined"]
        assert tracker.fake.writes == writes + 1, "only the dashboard is written"


# -- T-S16/T-S20 the source failing ---------------------------------------------
def test_a_truncated_answer_fails_the_source_after_one_retry(run: Any, tracker: Any, steam: Any) -> None:
    steam.env(FAKE_STEAMCMD_TRUNCATE="5")

    code, payload, _ = tracker.scan(run, "--bootstrap")

    assert code == 4
    assert "after 2 attempts" in payload["sources"][STEAM_KEY]["error"]
    assert len(steam.calls) == 2


def test_a_misconfigured_watch_block_names_the_missing_key(run: Any, tracker: Any, steam: Any) -> None:
    watch = watch_of(tracker.root)
    watch.pop("depots")
    set_watch(tracker.root, watch)

    code, payload, _ = tracker.scan(run, "--bootstrap")

    assert code == 4
    assert "watch.depots" in payload["sources"][STEAM_KEY]["error"]
    assert payload["observations"] == []


def test_a_broken_known_branch_pattern_fails_only_its_own_source(run: Any, tracker: Any, steam: Any) -> None:
    """A pattern comes out of the catalog, so a broken one is a misconfiguration.

    The scan isolates a source that reports its own error; a raw ``re.error`` would escape
    that and take the whole run down, which is exactly what a bad regex must not do.
    """
    watch = watch_of(tracker.root)
    watch["knownBranches"] = ["regex:^v[0-9]+("]
    set_watch(tracker.root, watch)

    code, payload, _ = tracker.scan(run, "--bootstrap")

    assert code == 4
    error = payload["sources"][STEAM_KEY]["error"]
    assert "watch.knownBranches[0]" in error, error
    assert "not a valid pattern" in error
    assert payload["observations"] == []


# -- T-S17/T-S18 a Steam source beside another game's sources ------------------
# The one place the two halves of the catalog meet: one run, one tracker, one dashboard.
import test_readiness_transitions as readiness_rig  # noqa: E402


def test_a_failing_steam_source_keeps_the_other_sources_progressing(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch, steam: Any
) -> None:
    """A source that could not be read is retried next run, and quarantines nothing."""
    with readiness_rig.rig(catalog_copy, monkeypatch) as harness:
        restore_watch(harness.root)
        steam.env(FAKE_STEAMCMD_FAIL="1")

        code, payload, err = harness.scan(run, "--bootstrap", "--publish")

        assert code == 4, err
        assert payload["sources"][STEAM_KEY]["status"] == "failed"
        assert payload["sources"][STEAM_KEY]["checkpoint"]["after"] is None
        assert payload["sources"][scan_support.SOURCE_KEY]["checkpoint"]["after"]["seen"] > 0
        assert harness.dashboard_state()["sources"][STEAM_KEY]["checkpoint"] is None

        monkeypatch.delenv("FAKE_STEAMCMD_FAIL")
        code, payload, err = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, err
        assert payload["sources"][STEAM_KEY]["status"] == "ok"
        assert harness.checkpoint_ids(STEAM_KEY) == [PINNED_REV]


def test_a_steam_issue_carries_no_other_games_readiness_table(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch, steam: Any
) -> None:
    """The frameworks observed in the same run belong to Minecraft, not to this issue."""
    fake_steamcmd.move_head(steam.document, "public", 25100000, {"294422": SECOND_MANIFEST}, timeupdated=1788300000)
    steam.serve()
    with readiness_rig.rig(catalog_copy, monkeypatch) as harness:
        restore_watch(harness.root)
        code, _, err = harness.scan(run, "--bootstrap", "--publish")

        assert code == 0, err
        steam_issues = [
            issue
            for issue in harness.fake.issues
            if (identity.parse_marker(str(issue.get("body") or "")) or {}).get("provider") == "steam"
        ]
        assert [issue["title"] for issue in steam_issues] == ["7 Days to Die public: build 25100000 needs a target"]
        body = str(steam_issues[0]["body"])
        assert "| fabric |" not in body, "another game's platforms are not this issue's readiness"
        assert "takaro-maint:readiness=" not in body
        assert "<!-- takaro-maint:state=detected -->" in body
        assert "This game has no framework layer" in body
        minecraft = [
            issue
            for issue in harness.fake.issues
            if (identity.parse_marker(str(issue.get("body") or "")) or {}).get("provider") == "mojang"
        ]
        assert minecraft and "| fabric |" in str(minecraft[0]["body"]), "Minecraft's own issue is unchanged"


def test_vein_version_branches_are_known_without_suppressing_real_branch_reviews() -> None:
    """VEIN keeps old version labels on Steam; they are not active support channels."""
    from takaro_maint.providers.steam import _Watch

    record = json.loads((REPO_ROOT / "catalog/vein/game.json").read_text())
    watch = _Watch({"id": "steam", "game": "vein", **record["sources"]["steam"]})
    for branch in ("0.022h10", "0.022h18", "0.023", "0.024", "0.024h8"):
        assert watch.is_known(branch), branch
    for branch in ("public", "experimental", "0.024h8-test"):
        assert not watch.is_known(branch), branch
