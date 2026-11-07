"""``takaro-maint icons promote`` against two in-process S3 fakes (staging and production)."""

from __future__ import annotations

import hashlib
import json
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from fake_s3 import KEY_ID, PROD_KEY_ID, FakeS3, serving, serving_production
from icons_support import (
    BUCKET_VARIABLES,
    GAME,
    INDEX_CACHE,
    INDEX_KEY,
    WEBP_CACHE,
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


def _promote(run: Any, *extra: str, game: str = GAME) -> tuple[int, Any, str]:
    return run("icons", "promote", "--game", game, *extra)


def test_promote_into_empty_production(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    code, payload, stderr = _promote(run)
    assert code == 0 and payload["op"] == "icons promote"
    assert "never removes" not in stderr

    assert [request.method for request in production.requests] == ["GET"] + ["HEAD"] * 5 + ["PUT"] * 6 + ["GET"]
    assert production.puts()[-1].key == INDEX_KEY
    for request in production.puts()[:-1]:
        assert (request.content_type, request.cache_control) == ("image/webp", WEBP_CACHE)
    last = production.puts()[-1]
    assert (last.content_type, last.cache_control) == ("application/json", INDEX_CACHE)
    assert production.objects[INDEX_KEY][0] == staging.objects[INDEX_KEY][0]
    assert webp_objects(production) == webp_objects(staging)
    assert [request.method for request in staging.requests] == ["GET"] * 6
    assert {request.signed_with for request in production.requests} == {PROD_KEY_ID}
    assert {request.signed_with for request in staging.requests} == {KEY_ID}
    assert len(payload["objects"]["copied"]) == 5 and payload["objects"]["skipped"] == []
    assert payload["index"]["action"] == "uploaded" and payload["index"]["codes"] == len(published["icons"])


def test_promote_keeps_every_production_code(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    production_only, old_banner = "a" * 64 + ".webp", "b" * 64 + ".webp"
    production.seed(f"{GAME}/{production_only}", b"production art", "image/webp")
    seed_index(production, {"prod-only": production_only, "fixture-banner": old_banner})
    code, payload, stderr = _promote(run)
    assert code == 0, payload
    assert "warning" in stderr and "prod-only" in stderr and "never removes" in stderr

    live = stored_index(production)["icons"]
    assert live["prod-only"] == production_only
    assert live["fixture-banner"] == published["icons"]["fixture-banner"]
    assert {code: name for code, name in live.items() if code != "prod-only"} == published["icons"]
    assert payload["index"]["kept"] == ["prod-only"]
    assert payload["index"]["repointed"] == [
        {"code": "fixture-banner", "from": old_banner, "to": published["icons"]["fixture-banner"]}
    ]
    assert payload["index"]["added"] == sorted(set(published["icons"]) - {"fixture-banner"})
    assert production.deletes() == []
    assert production.objects[f"{GAME}/{production_only}"][0] == b"production art"


def test_promote_is_idempotent(run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]) -> None:
    assert _promote(run)[0] == 0
    production.requests.clear()
    code, payload, stderr = _promote(run)
    assert code == 0
    assert "never removes" not in stderr
    assert production.puts() == []
    assert payload["index"]["action"] == "unchanged"
    assert len(payload["objects"]["skipped"]) == 5 and payload["objects"]["copied"] == []


def test_promote_copies_only_what_production_lacks(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    already = sorted(webp_objects(staging))[:2]
    for key in already:
        production.seed(key, staging.objects[key][0], "image/webp")
    code, payload, _ = _promote(run)
    assert code == 0
    assert payload["objects"]["skipped"] == already
    assert len(payload["objects"]["copied"]) == 3
    assert [request.key for request in production.puts()[:-1]] == payload["objects"]["copied"]


def test_dry_run_writes_nothing(run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]) -> None:
    code, payload, _ = _promote(run, "--dry-run")
    assert code == 0
    assert production.puts() == [] and production.objects == {}
    assert payload["dryRun"] is True and len(payload["objects"]["toCopy"]) == 5
    assert payload["index"]["action"] == "would-upload"
    assert [request.method for request in staging.requests] == ["GET"] + ["HEAD"] * 5


def test_a_disabled_production_index_stays_disabled(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    seed_index(production, {}, enabled=False)
    code, payload, stderr = _promote(run)
    assert code == 0
    assert stored_index(production)["enabled"] is False and payload["index"]["enabled"] is False
    assert "warning" in stderr and "disabled" in stderr


def test_a_disabled_production_pack_stays_disabled_through_a_later_promote(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    assert _promote(run)[0] == 0
    assert run("icons", "set-enabled", "--env", "production", "--game", GAME, "--enabled", "false")[0] == 0
    assert _promote(run)[0] == 0
    assert stored_index(production)["enabled"] is False


def test_a_missing_staging_pack_is_exit_4(run: Any, staging: FakeS3, production: FakeS3) -> None:
    code, payload, _ = _promote(run)
    assert code == 4 and "publish it to staging first" in payload["error"]
    assert production.requests == []


def test_invalid_indexes_stop_the_promotion(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    staging.seed(INDEX_KEY, json.dumps({**published, "game": "other-game"}).encode())
    assert _promote(run)[0] == 7
    staging.seed(INDEX_KEY, b"{not json")
    assert _promote(run)[0] == 7
    staging.seed(INDEX_KEY, json.dumps(published).encode())
    production.seed(INDEX_KEY, b"[]")
    assert _promote(run)[0] == 7
    assert production.puts() == []


@pytest.mark.parametrize("extra", [[], ["--dry-run"]])
def test_a_staging_object_that_is_missing_is_a_contract_violation(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any], extra: list[str]
) -> None:
    del staging.objects[sorted(webp_objects(staging))[0]]
    code, payload, _ = _promote(run, *extra)
    assert code == 7 and payload["problems"]
    assert production.puts() == []


def test_a_staging_object_with_the_wrong_hash_is_exit_5(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    first, second = sorted(webp_objects(staging))[:2]
    staging.seed(first, staging.objects[second][0], "image/webp")
    code, _, _ = _promote(run)
    assert code == 5
    assert production.puts() == []


def test_a_staging_object_that_is_not_webp_is_a_contract_violation(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    junk = b"not an image"
    name = hashlib.sha256(junk).hexdigest() + ".webp"
    staging.seed(INDEX_KEY, json.dumps({**published, "icons": {**published["icons"], "fixture-junk": name}}).encode())
    staging.seed(f"{GAME}/{name}", junk, "image/webp")
    code, _, _ = _promote(run)
    assert code == 7
    assert production.puts() == []


def test_a_failed_object_upload_leaves_the_index_alone_and_a_rerun_resumes(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    production.fail_on("PUT", sorted(webp_objects(staging))[2])
    code, _, _ = _promote(run)
    assert code == 10
    assert INDEX_KEY not in [request.key for request in production.puts()]
    assert INDEX_KEY not in production.objects

    production.clear_failures()
    production.requests.clear()
    code, payload, _ = _promote(run)
    assert code == 0
    assert len(payload["objects"]["skipped"]) == 2 and len(payload["objects"]["copied"]) == 3
    assert production.puts()[-1].key == INDEX_KEY


@pytest.mark.parametrize("game", ["_store", "Bad_Game"])
def test_bad_games_are_refused_without_requests(run: Any, staging: FakeS3, production: FakeS3, game: str) -> None:
    code, payload, _ = _promote(run, game=game)
    assert code == 2 and payload["ok"] is False
    assert staging.requests == [] and production.requests == []


@pytest.mark.parametrize("prefix", ["STG_", "PROD_"])
@pytest.mark.parametrize("name", BUCKET_VARIABLES)
def test_a_missing_variable_is_named(
    run: Any, staging: FakeS3, production: FakeS3, monkeypatch: pytest.MonkeyPatch, prefix: str, name: str
) -> None:
    monkeypatch.delenv(prefix + name)
    code, payload, _ = _promote(run)
    assert code == 2 and prefix + name in payload["error"]
    assert staging.requests == [] and production.requests == []


def test_one_bucket_cannot_be_both_sides(run: Any, staging: FakeS3, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("PROD_R2_ENDPOINT", staging.endpoint + "/")
    monkeypatch.setenv("PROD_R2_BUCKET", staging.bucket)
    monkeypatch.setenv("PROD_R2_ACCESS_KEY_ID", "AKIAFIXTUREPRODKEY01")
    monkeypatch.setenv("PROD_R2_SECRET_ACCESS_KEY", "fixture-prod-secret-access-key-value")
    code, payload, _ = _promote(run)
    assert code == 2 and "same bucket" in payload["error"]
    assert staging.requests == []


def test_a_production_endpoint_must_be_https(
    run: Any, staging: FakeS3, production: FakeS3, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PROD_R2_ENDPOINT", "http://example.invalid")
    code, payload, _ = _promote(run)
    assert code == 2 and "https" in payload["error"]
    assert staging.requests == [] and production.requests == []


def test_no_bucket_variable_is_ever_printed(
    run: Any, staging: FakeS3, production: FakeS3, published: dict[str, Any]
) -> None:
    outputs = [_promote(run, "--dry-run"), _promote(run)]
    production.fail_on("GET", "index")
    outputs.append(_promote(run))
    outputs.append(_promote(run, game="_store"))
    assert [code for code, _, _ in outputs] == [0, 0, 10, 2]
    for _, payload, stderr in outputs:
        assert_nothing_leaked(payload, stderr)
