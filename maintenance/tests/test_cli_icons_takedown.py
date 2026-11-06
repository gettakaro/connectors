"""``takaro-maint icons takedown`` against in-process S3 fakes."""

from __future__ import annotations

import json
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from fake_s3 import PROD_KEY_ID, FakeS3, serving, serving_production
from icons_support import (
    GAME,
    INDEX_KEY,
    assert_nothing_leaked,
    publish_fixture,
    seed_index,
    stored_index,
    webp_objects,
)


@pytest.fixture
def staging(monkeypatch: pytest.MonkeyPatch) -> Iterator[FakeS3]:
    with serving(monkeypatch) as server:
        yield server


@pytest.fixture
def production(monkeypatch: pytest.MonkeyPatch) -> Iterator[FakeS3]:
    with serving_production(monkeypatch) as server:
        yield server


@pytest.fixture
def published(run: Any, tmp_path: Path, staging: FakeS3, production: FakeS3) -> dict[str, Any]:
    return publish_fixture(run, tmp_path, staging)


def _takedown(run: Any, *extra: str, env: str = "staging", game: str = GAME) -> tuple[int, Any, str]:
    return run("icons", "takedown", "--env", env, "--game", game, *extra)


def _key(name: str) -> str:
    return f"{GAME}/{name}"


def test_taking_down_a_code_removes_it_and_its_object(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    pebble = published["icons"]["fixture-pebble"]
    objects_before = webp_objects(staging)
    code, payload, stderr = _takedown(run, "--code", "fixture-pebble")
    assert code == 0 and payload["op"] == "icons takedown"

    assert [(request.method, request.key) for request in staging.requests] == [
        ("GET", INDEX_KEY),
        ("PUT", INDEX_KEY),
        ("GET", INDEX_KEY),
        ("DELETE", _key(pebble)),
        ("HEAD", _key(pebble)),
    ]
    stored = stored_index(staging)
    assert stored["icons"] == {code: name for code, name in published["icons"].items() if code != "fixture-pebble"}
    assert stored["enabled"] is True and stored["game"] == GAME
    assert webp_objects(staging) == {key: body for key, body in objects_before.items() if key != _key(pebble)}
    assert payload["removedCodes"] == ["fixture-pebble"]
    assert payload["objects"] == {"deleted": [_key(pebble)], "retained": []}
    assert payload["index"] == {"action": "uploaded", "codes": 5, "enabled": True}
    assert "warning" in stderr and _key(pebble) in stderr and "purge" in stderr
    assert production.requests == []


def test_an_object_another_code_still_uses_is_retained(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    shared = published["icons"]["fixture-circle"]
    assert shared == published["icons"]["fixture-circle-alias"]
    code, payload, _ = _takedown(run, "--code", "fixture-circle")
    assert code == 0
    assert staging.deletes() == [] and _key(shared) in staging.objects
    assert payload["objects"] == {"deleted": [], "retained": [{"object": shared, "codes": ["fixture-circle-alias"]}]}
    assert "fixture-circle" not in stored_index(staging)["icons"]


def test_taking_down_every_code_of_a_shared_object_deletes_it(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    shared = published["icons"]["fixture-circle"]
    code, payload, _ = _takedown(run, "--code", "fixture-circle", "--code", "fixture-circle-alias")
    assert code == 0
    assert payload["removedCodes"] == ["fixture-circle", "fixture-circle-alias"]
    assert [request.key for request in staging.deletes()] == [_key(shared)]
    assert _key(shared) not in staging.objects


def test_taking_down_an_object_removes_every_code_pointing_at_it(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    shared = published["icons"]["fixture-circle"]
    code, payload, _ = _takedown(run, "--object", shared)
    assert code == 0
    assert payload["removedCodes"] == ["fixture-circle", "fixture-circle-alias"]
    assert payload["objects"]["deleted"] == [_key(shared)]
    assert set(stored_index(staging)["icons"]) == set(published["icons"]) - {"fixture-circle", "fixture-circle-alias"}
    assert [request.key for request in staging.deletes()] == [_key(shared)]


def test_an_orphaned_object_can_be_deleted_without_touching_the_index(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    old_banner = published["icons"]["fixture-banner"]
    repointed = {**published["icons"], "fixture-banner": published["icons"]["fixture-crate"]}
    seed_index(staging, repointed)
    code, payload, _ = _takedown(run, "--object", old_banner)
    assert code == 0, payload
    assert payload["index"]["action"] == "unchanged" and payload["removedCodes"] == []
    assert staging.puts() == []
    assert _key(old_banner) not in staging.objects
    assert stored_index(staging)["icons"] == repointed


def test_every_code_can_be_taken_down(run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]) -> None:
    code, payload, _ = _takedown(run, "--code", *published["icons"])
    assert code == 0, payload
    assert stored_index(staging) == {"game": GAME, "enabled": True, "icons": {}}
    assert webp_objects(staging) == {}
    assert payload["index"]["codes"] == 0


def test_a_disabled_pack_stays_disabled(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    seed_index(staging, published["icons"], enabled=False)
    assert _takedown(run, "--code", "fixture-pebble")[0] == 0
    assert stored_index(staging)["enabled"] is False


def test_dry_run_plans_without_writing(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    code, payload, _ = _takedown(run, "--code", "fixture-pebble", "fixture-circle", "--dry-run")
    assert code == 0 and payload["dryRun"] is True
    assert staging.puts() == [] and staging.deletes() == []
    assert payload["index"]["action"] == "would-upload"
    assert payload["objects"]["toDelete"] == [_key(published["icons"]["fixture-pebble"])]
    assert payload["objects"]["retained"][0]["codes"] == ["fixture-circle-alias"]
    assert stored_index(staging)["icons"] == published["icons"]


def test_a_failed_delete_reports_what_is_pending_and_a_rerun_finishes(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    pebble = published["icons"]["fixture-pebble"]
    staging.fail_on("DELETE", pebble)
    code, payload, _ = _takedown(run, "--code", "fixture-pebble")
    assert code == 10 and payload["pendingObjects"] == [pebble]
    assert f"--object {pebble}" in payload["error"]
    assert "fixture-pebble" not in stored_index(staging)["icons"]
    assert _key(pebble) in staging.objects

    staging.clear_failures()
    code, payload, _ = _takedown(run, "--object", pebble)
    assert code == 0 and payload["objects"]["deleted"] == [_key(pebble)]
    assert _key(pebble) not in staging.objects


def test_production_touches_only_the_production_bucket(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    gone = "c" * 64 + ".webp"
    production.seed(_key(gone), b"art", "image/webp")
    seed_index(production, {"prod-only": gone})
    code, payload, _ = _takedown(run, "--code", "prod-only", env="production")
    assert code == 0, payload
    assert staging.requests == []
    assert {request.signed_with for request in production.requests} == {PROD_KEY_ID}
    assert _key(gone) not in production.objects
    assert stored_index(staging)["icons"] == published["icons"]


@pytest.mark.parametrize(
    "arguments",
    [
        ["--code", "no-such-code"],
        ["--code", "fixture-pebble", "--code", "no-such-code"],
        ["--object", "d" * 64 + ".webp"],
        ["--code", "fixture-pebble", "--object", "d" * 64 + ".webp"],
        [],
        ["--object", "index.json"],
        ["--object", f"{GAME}/{'e' * 64}.webp"],
        ["--object", "../x.webp"],
        ["--object", "E" * 64 + ".webp"],
    ],
)
def test_refusals_write_nothing(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any], arguments: list[str]
) -> None:
    code, payload, _ = _takedown(run, *arguments)
    assert code == 2 and payload["ok"] is False
    assert staging.puts() == [] and staging.deletes() == []
    assert production.requests == []


@pytest.mark.parametrize(
    ("env", "game"), [("prod", GAME), ("Staging", GAME), ("", GAME), ("staging", "_store"), ("staging", "Bad_Game")]
)
def test_bad_env_or_game_makes_no_requests(run: Any, staging: FakeS3, production: FakeS3, env: str, game: str) -> None:
    code, _, _ = _takedown(run, "--code", "fixture-pebble", env=env, game=game)
    assert code == 2
    assert staging.requests == [] and production.requests == []


def test_a_missing_index_is_exit_4_and_an_invalid_one_exit_7(run: Any, staging: FakeS3, production: FakeS3) -> None:
    assert _takedown(run, "--code", "fixture-pebble")[0] == 4
    staging.seed(INDEX_KEY, json.dumps({"game": GAME, "enabled": "yes", "icons": {}}).encode())
    assert _takedown(run, "--code", "fixture-pebble")[0] == 7
    assert staging.puts() == [] and staging.deletes() == []


def test_no_bucket_variable_is_ever_printed(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    outputs = [
        _takedown(run, "--code", "fixture-pebble", "--dry-run"),
        _takedown(run, "--code", "fixture-pebble"),
        _takedown(run, "--code", "nope"),
        _takedown(run, "--code", "x", env="prod"),
    ]
    staging.fail_on("GET", "index")
    outputs.append(_takedown(run, "--code", "fixture-crate"))
    assert [code for code, _, _ in outputs] == [0, 0, 2, 2, 10]
    for _, payload, stderr in outputs:
        assert_nothing_leaked(payload, stderr)
