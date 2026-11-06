"""``takaro-maint icons set-enabled`` against in-process S3 fakes."""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from fake_s3 import KEY_ID, PROD_KEY_ID, FakeS3, serving, serving_production
from icons_support import (
    GAME,
    INDEX_CACHE,
    INDEX_KEY,
    assert_nothing_leaked,
    build_fixture_pack,
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


def _set(run: Any, enabled: str, *extra: str, env: str = "staging", game: str = GAME) -> tuple[int, Any, str]:
    return run("icons", "set-enabled", "--env", env, "--game", game, "--enabled", enabled, *extra)


def test_disabling_rewrites_only_the_flag(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    original_index = staging.objects[INDEX_KEY][0]
    objects_before = webp_objects(staging)
    code, payload, _ = _set(run, "false")
    assert code == 0 and payload["op"] == "icons set-enabled"

    assert [(request.method, request.key) for request in staging.requests] == [
        ("GET", INDEX_KEY),
        ("PUT", INDEX_KEY),
        ("GET", INDEX_KEY),
    ]
    stored = stored_index(staging)
    assert list(stored) == ["game", "enabled", "icons"]
    assert stored["enabled"] is False and stored["icons"] == published["icons"] and stored["game"] == GAME
    assert webp_objects(staging) == objects_before
    put = staging.puts()[0]
    assert (put.content_type, put.cache_control) == ("application/json", INDEX_CACHE)
    assert payload["enabled"] == {"from": True, "to": False}
    assert payload["index"] == {"action": "uploaded", "codes": len(published["icons"])}

    staging.requests.clear()
    assert _set(run, "true")[0] == 0
    assert staging.objects[INDEX_KEY][0] == original_index
    assert production.requests == []


def test_the_same_value_changes_nothing(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    code, payload, _ = _set(run, "true")
    assert code == 0 and payload["index"]["action"] == "unchanged"
    assert staging.puts() == []


def test_dry_run_changes_nothing(run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]) -> None:
    code, payload, _ = _set(run, "false", "--dry-run")
    assert code == 0 and payload["dryRun"] is True and payload["index"]["action"] == "would-upload"
    assert staging.puts() == [] and stored_index(staging)["enabled"] is True


def test_production_touches_only_the_production_bucket(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    seed_index(production, {"prod-only": "a" * 64 + ".webp"})
    code, payload, _ = _set(run, "false", env="production")
    assert code == 0, payload
    assert staging.requests == []
    assert {request.signed_with for request in production.requests} == {PROD_KEY_ID}
    assert stored_index(production)["enabled"] is False
    assert stored_index(staging)["enabled"] is True
    assert KEY_ID not in {request.signed_with for request in production.requests}


@pytest.mark.parametrize(
    "arguments",
    [
        ("True", "staging", GAME),
        ("1", "staging", GAME),
        ("yes", "staging", GAME),
        ("", "staging", GAME),
        ("false", "prod", GAME),
        ("false", "Staging", GAME),
        ("false", "", GAME),
        ("false", "staging", "_store"),
        ("false", "staging", "Bad_Game"),
    ],
)
def test_bad_arguments_are_refused_without_requests(
    run: Any, staging: FakeS3, production: FakeS3, arguments: tuple[str, str, str]
) -> None:
    enabled, env, game = arguments
    code, payload, _ = _set(run, enabled, env=env, game=game)
    assert code == 2 and payload["ok"] is False
    assert staging.requests == [] and production.requests == []


def test_a_missing_index_is_exit_4(run: Any, staging: FakeS3, production: FakeS3) -> None:
    code, payload, _ = _set(run, "false")
    assert code == 4 and GAME in payload["error"]
    assert staging.puts() == []


def test_an_invalid_index_is_exit_7_and_untouched(run: Any, staging: FakeS3, production: FakeS3) -> None:
    staging.seed(INDEX_KEY, b'{"game": "other", "enabled": true, "icons": {}}')
    code, payload, _ = _set(run, "false")
    assert code == 7 and payload["problems"]
    assert staging.puts() == []


def test_publish_leaves_a_disabled_pack_disabled(
    run: Any, tmp_path: Path, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    assert _set(run, "false")[0] == 0
    pack = build_fixture_pack(run, tmp_path / "again")
    assert run("icons", "publish", "--env", "staging", "--pack", str(pack))[0] == 0
    assert stored_index(staging)["enabled"] is False


def test_no_bucket_variable_is_ever_printed(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    outputs = [_set(run, "false", "--dry-run"), _set(run, "false"), _set(run, "yes"), _set(run, "false", env="prod")]
    staging.fail_on("GET", "index")
    outputs.append(_set(run, "true"))
    assert [code for code, _, _ in outputs] == [0, 0, 2, 2, 10]
    for _, payload, stderr in outputs:
        assert_nothing_leaked(payload, stderr)
