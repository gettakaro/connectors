"""`release publish`: the set reaches the release page whole, or the old one stays untouched.

Every test drives the real command against an in-process GitHub whose request log and stored
bytes are inspectable, so what is asserted is the protocol the publisher actually performed:
which requests, in which order, and what each release was left holding.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import pytest

from fake_github_releases import TOKEN, FakeReleases, serving
from test_release_assemble import CONNECTOR, REPO, VERSION, Inputs, assemble_default, build_inputs

TAG = f"{CONNECTOR}-v{VERSION}"
DEV_TAG = f"{CONNECTOR}-dev"


@pytest.fixture
def fake(monkeypatch: pytest.MonkeyPatch) -> Any:
    with serving(monkeypatch, REPO) as server:
        yield server


@pytest.fixture
def inputs(run: Any, catalog_copy: Path, tmp_path: Path) -> Inputs:
    return build_inputs(run, catalog_copy, tmp_path)


@pytest.fixture
def assembled(run: Any, inputs: Inputs, tmp_path: Path) -> tuple[Path, dict[str, Any], Inputs]:
    return assemble_default(run, inputs, tmp_path)


def publish(
    run: Any, fake: FakeReleases, directory: Path, root: Path, *extra: str, channel: str = "stable", tag: str = TAG
) -> tuple[int, Any, str]:
    return run(
        "release",
        "publish",
        "--connector",
        CONNECTOR,
        "--channel",
        channel,
        "--tag",
        tag,
        "--assembled",
        str(directory),
        "--repo",
        REPO,
        "--api-url",
        fake.api_url,
        *extra,
        repo=root,
    )


def stable_draft(fake: FakeReleases, commit: str, *, body: str = "") -> dict[str, Any]:
    """What release-please leaves behind with `draft: true` and `force-tag-creation: true`."""
    release = fake.add_release(TAG, draft=True, body=body, name=f"minecraft: v{VERSION}")
    fake.add_tag(TAG, commit)
    return release


def reassemble(run: Any, built: Inputs, out: Path, *, channel: str, tag: str) -> Path:
    code, _, err = run(
        "release",
        "assemble",
        "--connector",
        CONNECTOR,
        "--version",
        VERSION,
        "--channel",
        channel,
        "--tag",
        tag,
        "--dist",
        str(built.dist),
        "--reports",
        str(built.reports),
        "--out",
        str(out),
        "--repo",
        REPO,
        repo=built.root,
    )
    assert code == 0, err
    return out


# -- stable ------------------------------------------------------------------------------


def test_stable_uploads_every_asset_to_the_draft_and_undrafts_after_verifying(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    stable_draft(fake, built.commit)

    code, payload, err = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 0, err
    local = sorted(path.name for path in directory.iterdir())
    assert {asset["action"] for asset in payload["assets"]} == {"uploaded"}
    assert fake.asset_names(TAG) == local
    for name in local:
        assert fake.asset_bytes(TAG, name) == (directory / name).read_bytes()

    assert payload["verified"] is True
    assert payload["finalized"] is True
    assert fake.release_for(TAG)["draft"] is False
    assert fake.release_for(TAG)["prerelease"] is False

    uploads = [i for i, (method, path) in enumerate(fake.requests) if method == "POST" and "/uploads/" in path]
    downloads = [i for i, (method, path) in enumerate(fake.requests) if "/releases/assets/" in path]
    undraft = [i for i, (method, path) in enumerate(fake.requests) if method == "PATCH"]
    assert max(uploads) < min(downloads) < max(undraft)


@pytest.mark.parametrize("digests", [True, False])
def test_stable_retry_with_identical_bytes_is_a_no_op(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs], digests: bool
) -> None:
    directory, _, built = assembled
    fake.digests = digests
    stable_draft(fake, built.commit)
    first, _, err = publish(run, fake, directory, built.root, "--target-commit", built.commit)
    assert first == 0, err

    before = len(fake.requests)
    code, payload, err = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 0, err
    assert {asset["action"] for asset in payload["assets"]} == {"skipped-identical"}
    assert payload["alreadyPublished"] is True
    assert payload["finalized"] is False
    retried = fake.requests[before:]
    assert [path for method, path in retried if method == "POST"] == []
    assert [path for method, path in retried if method == "DELETE"] == []


def test_stable_conflicting_bytes_exit_seven_and_upload_nothing(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    """The conflict is on the second name in sort order, so an asset that sorts before it is missing."""
    directory, _, built = assembled
    release = stable_draft(fake, built.commit)
    names = sorted(path.name for path in directory.iterdir())
    fake.add_asset(release, names[1], b"someone else's bytes")

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7
    actions = {asset["name"]: asset["action"] for asset in payload["assets"]}
    assert actions[names[1]] == "conflict"
    assert all(actions[name] == "not-attempted" for name in names if name != names[1])
    assert payload["conflicts"] == [names[1]]
    assert payload["draft"] is True
    assert "still a draft" in payload["error"]
    uploaded = {path.split("name=")[-1] for method, path in fake.requests if "/uploads/" in path}
    assert uploaded == set()
    assert fake.asset_names(TAG) == [names[1]]
    assert fake.release_for(TAG)["draft"] is True


def test_a_conflict_on_a_published_release_leaves_it_untouched(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    """Recovery against an already-public release: every other asset is missing and sorts first."""
    directory, _, built = assembled
    release = fake.add_release(TAG, draft=False, name=f"minecraft: v{VERSION}")
    fake.add_tag(TAG, built.commit)
    names = sorted(path.name for path in directory.iterdir())
    fake.add_asset(release, names[-1], b"someone else's bytes")

    before = len(fake.requests)
    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7
    assert payload["conflicts"] == [names[-1]]
    assert payload["draft"] is False
    assert "still a draft" not in payload["error"]
    assert [path for method, path in fake.requests[before:] if "/uploads/" in path] == []
    assert [method for method, _ in fake.requests[before:] if method in ("POST", "PATCH", "DELETE")] == []
    assert fake.asset_names(TAG) == [names[-1]]
    assert fake.release_for(TAG)["draft"] is False


def test_stable_without_a_release_exits_seven(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7
    assert "no release for" in payload["error"]


def test_stable_without_a_tag_exits_seven(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    fake.add_release(TAG, draft=True)

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7
    assert "force-tag-creation" in payload["error"]


def test_stable_tag_pointing_elsewhere_exits_seven(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    stable_draft(fake, "a" * 40)

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7
    assert payload["tagCommit"] == "a" * 40
    assert "recovery must check out the tag" in payload["error"]
    assert fake.asset_names(TAG) == []


def test_undraft_never_happens_when_verification_fails(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    stable_draft(fake, built.commit)
    fake.corrupt_download(built.targets["paper-1.21.11"]["file"])

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7
    assert built.targets["paper-1.21.11"]["file"] in payload["error"]
    assert fake.release_for(TAG)["draft"] is True


def test_stable_puts_the_asset_table_first_once_and_keeps_release_notes(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    notes = "## 0.1.1\n\n* a fix ([#1](https://example.invalid))"
    stable_draft(fake, built.commit, body=notes)

    assert publish(run, fake, directory, built.root, "--target-commit", built.commit)[0] == 0
    once = fake.release_for(TAG)["body"]
    assert publish(run, fake, directory, built.root, "--target-commit", built.commit)[0] == 0
    twice = fake.release_for(TAG)["body"]

    assert once == twice
    # The download table leads (#756); release-please's notes follow unchanged.
    assert once.startswith("<!-- takaro-maint:assets:begin -->")
    assert once.rstrip().endswith(notes)
    assert once.count("<!-- takaro-maint:assets:begin -->") == 1
    assert built.targets["fabric-26.2"]["file"] in once
    assert "protocol (local)" in once


def test_patching_the_draft_keeps_its_tag(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    """GitHub drops a draft release's tag when a PATCH omits tag_name.

    Every stable publication patches release-please's draft — first its body, then `draft:
    false` — so an omitted tag_name would detach the tag the whole release hangs off and
    publish the set as `untagged-<hash>`.
    """
    directory, _, built = assembled
    stable_draft(fake, built.commit, body="## 0.1.1\n\n* a fix")

    first, _, err = publish(run, fake, directory, built.root, "--target-commit", built.commit, "--no-finalize")
    assert first == 0, err
    assert fake.release_for(TAG) is not None, "the body patch detached the draft's tag"

    second, payload, err = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert second == 0, f"{err}\n{payload}"
    assert payload["finalized"] is True
    assert fake.release_for(TAG)["draft"] is False
    assert [r["tag_name"] for r in fake.releases] == [TAG]


def test_no_finalize_leaves_the_draft_alone(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    stable_draft(fake, built.commit)

    code, payload, err = publish(run, fake, directory, built.root, "--target-commit", built.commit, "--no-finalize")

    assert code == 0, err
    assert payload["finalized"] is False
    assert payload["verified"] is True
    assert fake.release_for(TAG)["draft"] is True


# -- rolling and pr ----------------------------------------------------------------------


def test_rolling_replaces_the_old_release_only_after_a_complete_staging_set(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    directory = reassemble(run, inputs, tmp_path / "dev", channel="rolling", tag=DEV_TAG)
    old = fake.add_release(DEV_TAG, prerelease=True)
    fake.add_asset(old, "takaro-fabric-0.0.9.jar", b"the previous build")
    fake.add_tag(DEV_TAG, "b" * 40)

    code, payload, err = publish(
        run,
        fake,
        directory,
        inputs.root,
        "--target-commit",
        inputs.commit,
        "--run-id",
        "gha-1-1",
        channel="rolling",
        tag=DEV_TAG,
    )

    assert code == 0, err
    order = [(method, path) for method, path in fake.requests if method in {"POST", "DELETE", "PATCH"}]
    create = next(i for i, (method, path) in enumerate(order) if method == "POST" and path.endswith("/releases"))
    uploads = [i for i, (method, path) in enumerate(order) if "/uploads/" in path]
    drop_old = next(
        i for i, (method, path) in enumerate(order) if method == "DELETE" and path.endswith(f"/{old['id']}")
    )
    drop_tag = next(i for i, (method, path) in enumerate(order) if method == "DELETE" and "git/refs/tags" in path)
    swap = next(i for i, (method, path) in enumerate(order) if method == "PATCH")
    assert create < min(uploads) and max(uploads) < drop_old < drop_tag < swap

    assert payload["staging"]["tag"] == f"{DEV_TAG}.staging-gha-1-1"
    assert payload["replaced"] == {"releaseId": old["id"], "tagMoved": True}
    assert len([r for r in fake.releases if r["tag_name"] == DEV_TAG]) == 1
    assert fake.release_for(DEV_TAG)["prerelease"] is True
    assert fake.release_for(DEV_TAG)["draft"] is False
    assert "takaro-fabric-0.0.9.jar" not in fake.asset_names(DEV_TAG)
    assert fake.asset_names(DEV_TAG) == sorted(path.name for path in directory.iterdir())
    assert fake.tags[DEV_TAG] == inputs.commit


def test_a_failure_before_the_swap_leaves_the_old_release_intact(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    directory = reassemble(run, inputs, tmp_path / "dev", channel="rolling", tag=DEV_TAG)
    old = fake.add_release(DEV_TAG, prerelease=True)
    fake.add_asset(old, "takaro-fabric-0.0.9.jar", b"the previous build")
    fake.add_tag(DEV_TAG, "b" * 40)
    fake.fail_uploads_after(2)

    code, payload, _ = publish(
        run,
        fake,
        directory,
        inputs.root,
        "--target-commit",
        inputs.commit,
        "--run-id",
        "gha-2-1",
        channel="rolling",
        tag=DEV_TAG,
    )

    assert code == 9
    assert "stagingLeft" not in payload
    assert [r["tag_name"] for r in fake.releases] == [DEV_TAG]
    assert fake.release_for(DEV_TAG)["id"] == old["id"]
    assert fake.asset_names(DEV_TAG) == ["takaro-fabric-0.0.9.jar"]
    assert fake.tags[DEV_TAG] == "b" * 40


def test_an_interrupted_publication_resumes_to_completion(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    directory = reassemble(run, inputs, tmp_path / "dev", channel="rolling", tag=DEV_TAG)
    old = fake.add_release(DEV_TAG, prerelease=True)
    fake.add_tag(DEV_TAG, "b" * 40)
    fake.fail_uploads_after(1)
    fake.fail_on(r"/releases/\d+$", "DELETE")

    first, payload, _ = publish(
        run,
        fake,
        directory,
        inputs.root,
        "--target-commit",
        inputs.commit,
        "--run-id",
        "gha-3-1",
        channel="rolling",
        tag=DEV_TAG,
    )
    assert first == 9
    assert payload["stagingLeft"]["tag"] == f"{DEV_TAG}.staging-gha-3-1"
    assert any(r["tag_name"] == f"{DEV_TAG}.staging-gha-3-1" for r in fake.releases)

    fake.fail_uploads_after(10_000)
    fake._fail_on = None
    second, payload, err = publish(
        run,
        fake,
        directory,
        inputs.root,
        "--target-commit",
        inputs.commit,
        "--run-id",
        "gha-3-2",
        channel="rolling",
        tag=DEV_TAG,
    )

    assert second == 0, err
    assert not any(str(r["tag_name"]).startswith(f"{DEV_TAG}.staging-") for r in fake.releases)
    assert [r["tag_name"] for r in fake.releases] == [DEV_TAG]
    assert fake.release_for(DEV_TAG)["id"] != old["id"]
    assert fake.asset_names(DEV_TAG) == sorted(path.name for path in directory.iterdir())


def test_a_publisher_never_deletes_other_tags_outputs(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    directory = reassemble(run, inputs, tmp_path / "dev", channel="rolling", tag=DEV_TAG)
    bystanders = [
        fake.add_release("pr-5-minecraft", prerelease=True),
        fake.add_release("7d2d-dev", prerelease=True),
        fake.add_release(f"{CONNECTOR}-v0.1.0"),
        fake.add_release("pr-5-minecraft.staging-gha-9-9", draft=True),
    ]
    fake.add_release(DEV_TAG, prerelease=True)
    fake.add_tag(DEV_TAG, "b" * 40)

    code, _, err = publish(
        run,
        fake,
        directory,
        inputs.root,
        "--target-commit",
        inputs.commit,
        "--run-id",
        "gha-4-1",
        channel="rolling",
        tag=DEV_TAG,
    )

    assert code == 0, err
    survivors = {r["tag_name"] for r in fake.releases}
    assert {r["tag_name"] for r in bystanders} <= survivors


def test_pr_channel_uses_the_staging_protocol_with_the_pr_title_and_notes(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    tag = "pr-42-minecraft"
    directory = reassemble(run, inputs, tmp_path / "pr", channel="pr", tag=tag)

    code, payload, err = publish(
        run,
        fake,
        directory,
        inputs.root,
        "--target-commit",
        inputs.commit,
        "--run-id",
        "gha-5-1",
        "--pr-number",
        "42",
        channel="pr",
        tag=tag,
    )

    assert code == 0, err
    release = fake.release_for(tag)
    assert release["name"] == "Minecraft Connector — build for PR #42"
    assert "Disposable build (`0.1.1`) for PR #42" in release["body"]
    assert "<!-- takaro-maint:assets:begin -->" in release["body"]
    assert release["prerelease"] is True
    assert payload["staging"]["tag"] == f"{tag}.staging-gha-5-1"


# -- refusals before anything moves ------------------------------------------------------


def test_local_files_that_disagree_with_sha256sums_exit_five_before_any_request(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    (directory / built.targets["fabric-26.2"]["file"]).write_bytes(b"tampered")

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 5
    assert payload["asset"] == built.targets["fabric-26.2"]["file"]
    assert fake.requests == []


def test_no_token_exits_nine(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs], monkeypatch: pytest.MonkeyPatch
) -> None:
    directory, _, built = assembled
    monkeypatch.delenv("GH_TOKEN")
    monkeypatch.setattr("shutil.which", lambda _name: None)

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 9
    assert "GH_TOKEN" in payload["error"]


def test_a_set_assembled_for_another_tag_is_refused(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled

    code, payload, _ = publish(
        run, fake, directory, built.root, "--target-commit", built.commit, tag="minecraft-v9.9.9"
    )

    assert code == 7
    assert "tag" in payload["error"]
    assert fake.requests == []


def test_the_token_never_appears_in_any_output(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    stable_draft(fake, built.commit)

    code, payload, stderr = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 0
    assert TOKEN not in json.dumps(payload)
    assert TOKEN not in stderr


def test_an_explicit_token_never_appears_in_any_output(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs], monkeypatch: pytest.MonkeyPatch
) -> None:
    directory, _, built = assembled
    stable_draft(fake, built.commit)
    monkeypatch.delenv("GH_TOKEN", raising=False)

    # ``--verbose`` is a global flag, so this one call cannot go through ``publish()``.
    code, payload, stderr = run(
        "--verbose",
        "release",
        "publish",
        "--connector",
        CONNECTOR,
        "--channel",
        "stable",
        "--tag",
        TAG,
        "--assembled",
        str(directory),
        "--repo",
        REPO,
        "--api-url",
        fake.api_url,
        "--target-commit",
        built.commit,
        "--token",
        "explicit-token-value",
        repo=built.root,
    )

    assert code == 0, stderr
    assert "explicit-token-value" not in json.dumps(payload)
    assert "explicit-token-value" not in stderr


def test_every_published_asset_hashes_to_what_the_record_says(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs]
) -> None:
    directory, _, built = assembled
    stable_draft(fake, built.commit)
    assert publish(run, fake, directory, built.root, "--target-commit", built.commit)[0] == 0

    record = json.loads((directory / f"takaro-{CONNECTOR}-{VERSION}.compat.json").read_text())
    for asset in record["assets"]:
        assert hashlib.sha256(fake.asset_bytes(TAG, asset["name"])).hexdigest() == asset["sha256"]


def test_a_freshly_assembled_retry_of_the_same_commit_is_still_a_no_op(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs], tmp_path: Path
) -> None:
    """Recovery re-runs `assemble` before `publish`, so the retry must recognise its own bytes."""
    directory, _, built = assembled
    stable_draft(fake, built.commit)
    assert publish(run, fake, directory, built.root, "--target-commit", built.commit)[0] == 0

    again = reassemble(run, built, tmp_path / "recovered", channel="stable", tag=TAG)
    code, payload, err = publish(run, fake, again, built.root, "--target-commit", built.commit)

    assert code == 0, err
    assert {asset["action"] for asset in payload["assets"]} == {"skipped-identical"}


@pytest.mark.parametrize(("field", "value"), [("repo", "someone/else"), ("commit", "c" * 40)])
def test_a_set_assembled_for_another_repo_or_commit_is_refused(
    run: Any, fake: FakeReleases, assembled: tuple[Path, dict[str, Any], Inputs], field: str, value: str
) -> None:
    directory, _, built = assembled
    record_path = directory / f"takaro-{CONNECTOR}-{VERSION}.compat.json"
    record = json.loads(record_path.read_text())
    record["source"][field] = value
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    _rewrite_checksums(directory)

    code, payload, _ = publish(run, fake, directory, built.root, "--target-commit", built.commit)

    assert code == 7
    assert payload[field] == value
    assert fake.requests == []


def test_a_swap_that_fails_after_the_old_release_is_gone_names_the_staging_draft(
    run: Any, fake: FakeReleases, inputs: Inputs, tmp_path: Path
) -> None:
    directory = reassemble(run, inputs, tmp_path / "dev", channel="rolling", tag=DEV_TAG)
    fake.add_release(DEV_TAG, prerelease=True)
    fake.add_tag(DEV_TAG, "b" * 40)
    fake.fail_on(r"/git/refs/tags/", "DELETE")

    code, payload, _ = publish(
        run,
        fake,
        directory,
        inputs.root,
        "--target-commit",
        inputs.commit,
        "--run-id",
        "gha-7-1",
        channel="rolling",
        tag=DEV_TAG,
    )

    assert code == 9
    assert payload["staging"] == {"tag": f"{DEV_TAG}.staging-gha-7-1", "releaseId": payload["staging"]["releaseId"]}
    assert payload["replaced"]["removed"] is True
    assert any(r["tag_name"] == f"{DEV_TAG}.staging-gha-7-1" for r in fake.releases)


def _rewrite_checksums(directory: Path) -> None:
    lines = [
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}"
        for path in sorted(directory.iterdir(), key=lambda p: p.name)
        if path.is_file() and path.name != "SHA256SUMS"
    ]
    (directory / "SHA256SUMS").write_text("\n".join(lines) + "\n")
