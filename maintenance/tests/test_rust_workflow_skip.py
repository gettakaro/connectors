"""The Rust workflow only ever skips its reference checks for a pull request."""

from __future__ import annotations

import re

from conftest import REPO_ROOT

RUST = REPO_ROOT / ".github/workflows/rust.yml"
RELEASE = REPO_ROOT / ".github/workflows/connector-release.yml"
NEEDS_REFERENCES = (
    "Compile-check the plugin against the pinned target",
    "Check the config resolution",
    "Check the world map tiles",
    "Check the protocol handshake",
)


def _step(text: str, name: str) -> str:
    """One step's block: from its ``- name:`` line to the next step or comment-led step."""
    start = text.index(f"      - name: {name}\n")
    following = re.search(r"^      - |^      #", text[start + 1 :], re.MULTILINE)
    return text[start : start + 1 + following.start()] if following else text[start:]


def test_every_step_that_needs_the_references_honours_the_skip() -> None:
    text = RUST.read_text()
    for name in NEEDS_REFERENCES:
        assert "if: steps.fetch.outputs.skip != 'true'" in _step(text, name), name
    assert "if:" not in _step(text, "Check the entity display names"), "the name catalogue needs no assemblies"


def test_only_a_pull_request_reaches_the_tolerant_fetch() -> None:
    block = _step(RUST.read_text(), "Fetch the pinned reference assemblies and Carbon")
    condition, otherwise = block.split("else", 1)
    assert 'if [ "$EVENT" = "pull_request" ]' in condition
    assert "ci-fetch-references.sh" in condition
    assert "setup-environment.sh" in otherwise
    assert "ci-fetch-references.sh" not in otherwise


def test_a_skipped_release_build_is_a_pull_request_decision() -> None:
    block = _step(RELEASE.read_text(), "Does this pull request change this connector's build inputs")
    assert block.index('"$EVENT" != "pull_request"') < block.index('"$IN_SKIP_PR_BUILD" = "true"')


def test_a_file_renamed_out_of_the_rust_tree_counts_as_a_rust_change() -> None:
    script = (REPO_ROOT / "games/rust/scripts/ci-fetch-references.sh").read_text()
    assert "diff --name-only --no-renames HEAD^1 HEAD" in script
