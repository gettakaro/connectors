"""Marker identity and body rewriting: the two things that keep duplicates impossible.

These are the pure functions under the scan. The scenarios in ``test_scan_*.py`` drive
them through the real command; here they are pinned directly, because "a human reordered
the marker" and "a human deleted the block" are cheaper to state than to stage.
"""

from __future__ import annotations

import dataclasses

import test_scan_support as support
from takaro_maint.providers.base import Observation
from takaro_maint.tracker import identity, issues

MARKER = {"kind": "support", "provider": "mojang", "component": "minecraft", "branch": "release", "rev": "26.3"}


def test_marker_renders_in_canonical_order() -> None:
    assert identity.render_marker(MARKER) == (
        "<!-- takaro-maint: kind=support provider=mojang component=minecraft branch=release rev=26.3 -->"
    )
    assert identity.render_marker(identity.DASHBOARD_MARKER) == "<!-- takaro-maint: kind=dashboard v=1 -->"


def test_marker_parse_is_order_insensitive() -> None:
    reordered = "<!-- takaro-maint: rev=26.3 branch=release component=minecraft provider=mojang kind=support -->"

    parsed = identity.parse_marker(reordered)

    assert parsed == MARKER
    assert identity.render_marker(parsed or {}) == identity.render_marker(MARKER)


def test_marker_only_on_the_first_line_counts() -> None:
    quoted = "Some human wrote:\n\n" + identity.render_marker(MARKER) + "\n"

    assert identity.parse_marker(quoted) is None
    assert identity.parse_marker("\n\n" + identity.render_marker(MARKER)) == MARKER


def test_a_malformed_marker_is_not_an_identity() -> None:
    assert identity.parse_marker("<!-- takaro-maint: rev=26 3 -->") is None
    assert identity.parse_marker("<!-- takaro-maint: -->") is None
    assert identity.parse_marker("") is None


def test_search_terms_end_with_the_revision() -> None:
    assert identity.search_terms(MARKER).split(" ")[-1] == "26.3"
    assert identity.search_terms(identity.DASHBOARD_MARKER).split(" ")[-1] == "kind=dashboard"
    assert identity.search_terms(MARKER).startswith("label:connector-maintenance ")


def test_canonical_identity_leaves_the_kind_to_the_marker() -> None:
    observation = support.golden_observation()

    assert observation.identity == "provider=mojang component=minecraft branch=release rev=26.3"
    assert identity.support_marker(observation) == MARKER


def test_steam_support_marker_carries_catalog_join_keys() -> None:
    observation = Observation(
        provider="steam",
        component="enshrouded",
        branch="public",
        rev="23178631.abc123+public",
        kind="game",
        identity="steam-observation",
        facts={"app": 2278520, "buildid": 23178631},
    )

    marker = identity.support_marker(observation)

    assert marker["app"] == "2278520"
    assert marker["buildid"] == "23178631"
    assert identity.render_marker(marker) == (
        "<!-- takaro-maint: kind=support provider=steam component=enshrouded app=2278520 "
        "branch=public buildid=23178631 rev=23178631.abc123+public -->"
    )


def test_render_body_keeps_text_outside_the_owned_block() -> None:
    block = issues.render_owned_block(support.golden_observation(), support.golden_targets())
    first = issues.render_body(MARKER, block)
    edited = first.replace(issues.INTRO, "My own note. Niek is on this.") + "\nA trailing human paragraph.\n"

    rewritten = issues.render_body(MARKER, block, edited)

    assert rewritten == edited
    assert "My own note. Niek is on this." in rewritten
    assert rewritten.endswith("A trailing human paragraph.\n")
    assert rewritten.count(issues.OWNED_BEGIN) == 1


def test_missing_owned_markers_append_one_block() -> None:
    block = issues.render_owned_block(support.golden_observation(), support.golden_targets())
    human_only = identity.render_marker(MARKER) + "\n\nI deleted the generated part.\n"

    once = issues.render_body(MARKER, block, human_only)
    twice = issues.render_body(MARKER, block, once)

    assert once.count(issues.OWNED_BEGIN) == 1
    assert twice == once
    assert "I deleted the generated part." in twice


def test_existing_state_is_preserved() -> None:
    observation = support.golden_observation()
    targets = support.golden_targets()
    body = issues.render_body(MARKER, issues.render_owned_block(observation, targets))
    reviewed = body.replace("<!-- takaro-maint:state=detected -->", "<!-- takaro-maint:state=in-progress -->")

    assert issues.existing_state(reviewed) == "in-progress"
    rewritten = issues.render_body(
        MARKER,
        issues.render_owned_block(observation, targets, state=issues.existing_state(reviewed) or "detected"),
        reviewed,
    )
    assert "<!-- takaro-maint:state=in-progress -->" in rewritten
    assert "state=detected" not in rewritten


def test_the_owned_block_matches_the_golden_file() -> None:
    block = issues.render_owned_block(support.golden_observation(), support.golden_targets())

    assert block + "\n" == (support.GITHUB_FIXTURES / "support-issue-26.3.md").read_text(encoding="utf-8")


def test_the_owned_block_carries_every_source_link_and_no_readiness_claim() -> None:
    block = issues.render_owned_block(support.golden_observation(), support.golden_targets())

    for url in (
        support.GOLDEN_FACTS["manifestList"]["url"],
        support.GOLDEN_FACTS["manifest"]["url"],
        support.GOLDEN_FACTS["server"]["url"],
    ):
        assert url in block
    assert "62294556 bytes" in block
    assert "| Java | 25 |" in block
    assert issues.READINESS_SENTENCE in block
    assert "| paper | — | — | — | no |" in block


def test_a_readiness_table_replaces_the_sentence() -> None:
    block = issues.render_owned_block(
        support.golden_observation(),
        support.golden_targets(),
        readiness=["| Framework | Ready |", "| --- | --- |", "| fabric | no |"],
    )

    assert issues.READINESS_SENTENCE not in block
    assert "| fabric | no |" in block


def test_the_title_names_the_game_and_the_revision() -> None:
    assert issues.title_for(support.golden_observation(), "Minecraft") == (
        "Minecraft 26.3: new stable release needs a target"
    )


def test_deleting_only_the_end_marker_replaces_the_rest_of_the_body() -> None:
    """Documented, not discovered: half a block is treated as a whole one."""
    block = issues.render_owned_block(support.golden_observation(), support.golden_targets())
    half = issues.render_body(MARKER, block).replace(issues.OWNED_END, "")

    rewritten = issues.render_body(MARKER, block, half)

    assert rewritten.count(issues.OWNED_BEGIN) == 1
    assert rewritten.count(issues.OWNED_END) == 1
    assert rewritten.startswith(identity.render_marker(MARKER))


def test_an_icon_pack_adds_one_line_and_moves_nothing_else() -> None:
    plain = issues.render_owned_block(support.golden_observation(), support.golden_targets())
    pack = issues.IconPack("g", "p", True)
    targets = dataclasses.replace(support.golden_targets(), icons=pack)
    routed = issues.render_owned_block(support.golden_observation(), targets)

    assert routed == plain.replace(issues.OWNED_END, issues.icon_rebuild_line(pack) + "\n" + issues.OWNED_END)
