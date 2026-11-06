"""Shared setup for the icon bucket command tests: a built fixture pack published to a staging fake."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from conftest import FIXTURES
from fake_s3 import FakeS3
from takaro_maint.icons.contract import serialize_index

GAME = "fixture-846"
INDEX_KEY = f"{GAME}/index.json"
FIXTURE = FIXTURES / "icons" / "fixture-846"
BUCKET_VARIABLES = ("R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "R2_ENDPOINT", "R2_BUCKET")
INDEX_CACHE = "public, max-age=300"
WEBP_CACHE = "public, max-age=31536000, immutable"


def build_fixture_pack(run: Any, tmp_path: Path) -> Path:
    out = tmp_path / "built"
    code, payload, _ = run("icons", "build", "--recipes", str(FIXTURE / "recipes.json"), "--out", str(out))
    assert code == 0, payload
    return out / "pack" / GAME


def publish_fixture(run: Any, tmp_path: Path, staging: FakeS3) -> dict[str, Any]:
    """Build and publish the fixture pack to the staging fake; returns its index and clears the request log."""
    pack = build_fixture_pack(run, tmp_path)
    code, payload, _ = run("icons", "publish", "--env", "staging", "--pack", str(pack))
    assert code == 0, payload
    staging.requests.clear()
    index: dict[str, Any] = json.loads((pack / "index.json").read_text())
    return index


def stored_index(fake: FakeS3) -> dict[str, Any]:
    stored: dict[str, Any] = json.loads(fake.objects[INDEX_KEY][0])
    return stored


def seed_index(fake: FakeS3, icons: dict[str, str], enabled: bool = True) -> None:
    fake.seed(INDEX_KEY, serialize_index(GAME, enabled, icons), "application/json")


def webp_objects(fake: FakeS3) -> dict[str, bytes]:
    return {key: value[0] for key, value in fake.objects.items() if key.endswith(".webp")}


def assert_nothing_leaked(*streams: object) -> None:
    """None of the eight bucket variable values (endpoint with port included) is in any output."""
    values = [os.environ[prefix + name] for prefix in ("STG_", "PROD_") for name in BUCKET_VARIABLES]
    assert len(values) == 8
    for stream in streams:
        text = stream if isinstance(stream, str) else json.dumps(stream)
        for value in values:
            assert value not in text
