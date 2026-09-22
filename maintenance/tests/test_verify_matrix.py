"""The release workflow's `verify` job: what it needs, what it runs, what it keeps.

These are text assertions on the workflow file, because the workflow is the artifact under
review here. A YAML parser would let a job be renamed or a `needs` edge be dropped and still
pass, which is exactly the regression this file exists to catch.
"""

from __future__ import annotations

import json
import re
from typing import Any

from conftest import REPO_ROOT

WORKFLOW = REPO_ROOT / ".github/workflows/connector-release.yml"
JOB = re.compile(r"^  (?P<name>[A-Za-z0-9_-]+):$", re.MULTILINE)


def step(block: str, marker: str) -> str:
    """The one step of a job block that contains ``marker``, up to the next step."""
    steps = re.split(r"\n(?=      - )", block)
    found = [entry for entry in steps if marker in entry]
    assert len(found) == 1, f"expected exactly one step mentioning '{marker}', found {len(found)}"
    return found[0]


def job(name: str) -> str:
    """One job's block, from its key to the next one at the same indent."""
    text = WORKFLOW.read_text()
    starts = {match.group("name"): match.start() for match in JOB.finditer(text)}
    assert name in starts, f"the workflow has no job '{name}' (has: {', '.join(starts)})"
    later = [start for start in starts.values() if start > starts[name]]
    return text[starts[name] : min(later)] if later else text[starts[name] :]


def test_the_release_workflow_verifies_between_build_and_publish() -> None:
    verify = job("verify")

    assert "needs: [plan, build]" in verify
    assert "fail-fast: false" in verify
    assert "target: ${{ fromJson(needs.plan.outputs.targets) }}" in verify

    publish = job("publish")
    assert "needs: [build, legacy, verify]" in publish
    assert "needs.verify.result == 'success'" in publish


def test_the_verify_job_consumes_the_build_artifact_by_target_name() -> None:
    build, verify = job("build"), job("verify")
    artifact = "name: dist-${{ inputs.connector }}-${{ matrix.target.id }}"

    assert artifact in build, "the build leg no longer uploads under the name verify downloads"
    assert artifact in verify
    assert '--game "$IN_CONNECTOR"' in verify
    assert "IN_CONNECTOR: ${{ inputs.connector }}" in verify
    assert '--target "${{ matrix.target.id }}"' in verify
    assert "--artifacts dist" in verify
    assert "--out reports" in verify


def test_the_verify_job_holds_no_secrets_and_no_hosted_mode() -> None:
    verify = job("verify")

    assert "secrets." not in verify
    assert "--takaro hosted" not in verify
    assert "--negative" not in verify
    assert verify.count("permissions:") == 1
    assert "contents: read" in verify
    assert "contents: write" not in verify


def test_the_verify_job_keeps_its_report_even_when_it_fails() -> None:
    verify = job("verify")

    upload = step(verify, "upload-artifact")
    assert "if: always()" in upload
    assert "name: verify-${{ inputs.connector }}-${{ matrix.target.id }}" in upload
    assert "if-no-files-found: warn" in upload

    summary = step(verify, "GITHUB_STEP_SUMMARY")
    assert "if: always()" in summary

    assert "if: always()" in step(verify, "docker rm -f"), "a cancelled leg still removes its container"


def test_every_action_in_the_verify_job_is_sha_pinned() -> None:
    uses = re.findall(r"uses: (\S+)", job("verify"))

    assert uses, "the verify job pulls in no actions at all"
    for entry in uses:
        assert re.search(r"@[0-9a-f]{40}$", entry), entry


def test_the_build_leg_builds_twice_from_scratch_and_compares_every_artifact() -> None:
    """Without --rerun-tasks the second build is UP-TO-DATE and the comparison proves nothing."""
    build = job("build")
    again = step(build, "dist-again")

    assert "--gradle-args --rerun-tasks" in again
    assert "--out dist-again" in again
    assert "jq -r '.artifacts[].file' dist/build-manifest.json" in again
    assert 'cmp "dist/$file" "dist-again/$file"' in again
    assert build.index("artifact validate") < build.index("dist-again") < build.index("upload-artifact")
    assert "path: dist/" in step(build, "upload-artifact")


def test_the_plan_matrix_lists_the_minecraft_targets(run: Any) -> None:
    listing = ("targets", "list", "--game", "minecraft", "--status", "candidate,maintained", "--format", "gha")
    code, payload, _ = run(*listing)

    assert code == 0
    line = payload if isinstance(payload, str) else json.dumps(payload)
    rows = json.loads(line.split("targets=", 1)[1])
    assert {row["id"] for row in rows} == {
        "fabric-26.1.2",
        "fabric-26.2",
        "fabric-26.3",
        "neoforge-1.21.11",
        "paper-1.21.11",
    }
    for row in rows:
        assert row["id"] and row["fp16"] and row["status"]
        assert len(row["fp16"]) == 16


def test_every_job_checks_out_the_commit_that_started_the_run() -> None:
    """A branch tip can move while earlier jobs run; `github.sha` cannot."""
    text = WORKFLOW.read_text()
    expected = "ref: ${{ inputs.publish == 'stable' && inputs.tag || github.sha }}"

    names = [match.group("name") for match in JOB.finditer(text)]
    assert names, "the workflow has no jobs"
    for name in names:
        block = job(name)
        if "actions/checkout" not in block:
            continue
        assert expected in step(block, "actions/checkout"), name

    assert "github.ref" not in text, "a release job would check out a moving branch tip"


CONNECTOR_WORKFLOWS = (
    "7d2d",
    "conan-exiles",
    "dragonwilds",
    "enshrouded",
    "minecraft",
    "rust",
    "terraria",
    "valheim",
    "zomboid",
)


def test_every_connector_workflow_keeps_release_runs_out_of_the_ci_group() -> None:
    """A recovery dispatch runs on the same ref and workflow as a push; only the tag separates them."""
    for name in CONNECTOR_WORKFLOWS:
        text = (REPO_ROOT / ".github/workflows" / f"{name}.yml").read_text()
        assert "-${{ github.ref }}-${{ inputs.tag || 'ci' }}" in text, name
        assert "cancel-in-progress: ${{ inputs.tag == '' }}" in text, name


def run_bodies(text: str) -> list[str]:
    """Every `run:` value in a workflow, block scalars included, so a shell body can be inspected."""
    bodies: list[str] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)(?:-\s+)?run:\s*(.*)$", line)
        if not match:
            continue
        indent, rest = len(match.group(1)), match.group(2)
        if rest.strip() in ("|", ">", "|-", ">-"):
            body = []
            for following in lines[index + 1 :]:
                if following.strip() and (len(following) - len(following.lstrip())) <= indent:
                    break
                body.append(following)
            bodies.append("\n".join(body))
        else:
            bodies.append(rest)
    return bodies


def test_dispatch_inputs_reach_the_params_script_only_through_env() -> None:
    for name in CONNECTOR_WORKFLOWS:
        text = (REPO_ROOT / ".github/workflows" / f"{name}.yml").read_text()
        params = step(text, "release-params.sh")
        assert "IN_VERSION: ${{ inputs.version }}" in params, name
        assert "IN_TAG: ${{ inputs.tag }}" in params, name
        assert all("${{" not in body for body in run_bodies(params)), name


def test_the_minecraft_workflow_expands_no_input_inside_a_shell_body() -> None:
    """The callers are where dispatch free text enters; the reusable workflow's own expansions are separate."""
    text = (REPO_ROOT / ".github/workflows/minecraft.yml").read_text()
    for body in run_bodies(text):
        assert "${{ inputs." not in body, body
        assert "github.event.inputs" not in body, body


def test_no_dispatch_input_is_expanded_inside_a_shell_body() -> None:
    """``${{ inputs.x }}`` in a ``run:`` body is free text pasted into a shell on a token-holding runner.

    The safe form is an ``env:`` entry the shell reads as ``$VAR``, which release-params.sh
    already uses. This covers the reusable workflow as well as its callers, because the callers'
    dispatch values arrive here as workflow inputs.
    """
    for name in (*CONNECTOR_WORKFLOWS, "connector-release"):
        text = (REPO_ROOT / ".github/workflows" / f"{name}.yml").read_text()
        for body in run_bodies(text):
            assert "${{ inputs." not in body, f"{name}: {body}"
            assert "github.event.inputs" not in body, f"{name}: {body}"
