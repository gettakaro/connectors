"""A release asset on a tag that is re-uploaded in place is pinned by its asset id, not the tag."""

from __future__ import annotations

import hashlib
import io
import json
import shutil
from pathlib import Path
from typing import Any

import pytest

from takaro_maint import net
from takaro_maint.catalog import loader
from takaro_maint.catalog.validate import validate_catalog
from takaro_maint.providers.github_release import GitHubReleaseProvider

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET = "carbon-26864795"
SPEC = {
    "kind": "github-release-asset",
    "repo": "CarbonCommunity/Carbon",
    "tag": "production_build",
    "asset": "Carbon.Linux.Release.tar.gz",
}


class RecordingTransport:
    def __init__(self, body: bytes) -> None:
        self.body = body
        self.headers: list[dict[str, str]] = []

    def open(self, url: str, headers: dict[str, str]) -> Any:
        del url
        self.headers.append(dict(headers))
        return io.BytesIO(self.body)


@pytest.fixture
def transport() -> Any:
    previous = net.get_transport()
    recorder = RecordingTransport(b"carbon bytes")
    net.set_transport(recorder)
    try:
        yield recorder
    finally:
        net.set_transport(previous)


def test_an_asset_id_is_fetched_from_the_api_by_id() -> None:
    url = GitHubReleaseProvider()._asset_url({**SPEC, "assetId": 783397591}, {"baseUrl": "https://api.github.com/"})
    assert url == "https://api.github.com/repos/CarbonCommunity/Carbon/releases/assets/783397591"


def test_without_an_asset_id_the_tag_url_is_kept() -> None:
    url = GitHubReleaseProvider()._asset_url(SPEC, {"baseUrl": "https://github.com"})
    assert (
        url
        == "https://github.com/CarbonCommunity/Carbon/releases/download/production_build/Carbon.Linux.Release.tar.gz"
    )


@pytest.mark.parametrize(
    ("url", "accept"),
    [
        ("https://api.github.com/repos/CarbonCommunity/Carbon/releases/assets/783397591", "application/octet-stream"),
        ("https://github.com/CarbonCommunity/Carbon/releases/download/production_build/x.tar.gz", None),
    ],
)
def test_an_asset_id_url_asks_for_the_bytes(transport: Any, tmp_path: Path, url: str, accept: str | None) -> None:
    expect = net.Expectation(sha256=hashlib.sha256(b"carbon bytes").hexdigest())
    net.fetch(url, tmp_path / "blob", expect)
    assert transport.headers[0].get("Accept") == accept


def _catalog_with(tmp_path: Path, edit: Any) -> loader.Catalog:
    root = tmp_path / "catalog"
    shutil.copytree(REPO_ROOT / "catalog", root)
    path = root / "rust" / "targets" / f"{TARGET}.json"
    record = json.loads(path.read_text(encoding="utf-8"))
    edit(record)
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return loader.load(root)


def _rows(catalog: loader.Catalog) -> list[dict[str, Any]]:
    rows = [check.as_dict() for check in validate_catalog(catalog).checks]
    return [row for row in rows if row["id"] == "release-asset-by-id" and row["file"].endswith(f"{TARGET}.json")]


def test_validate_refuses_a_moving_tag_without_an_asset_id(tmp_path: Path) -> None:
    rows = _rows(_catalog_with(tmp_path, lambda record: record["inputs"]["carbon"].pop("assetId")))
    assert [row["status"] for row in rows] == ["fail"]
    assert "production_build" in rows[0]["detail"]


def test_validate_accepts_the_asset_id_pin(tmp_path: Path) -> None:
    rows = _rows(_catalog_with(tmp_path, lambda record: None))
    assert [row["status"] for row in rows] == ["pass"]


def test_a_retired_target_is_not_held_to_it(tmp_path: Path) -> None:
    def retire(record: dict[str, Any]) -> None:
        record["inputs"]["carbon"].pop("assetId")
        record["support"]["status"] = "retired"
        record["default"] = False

    assert _rows(_catalog_with(tmp_path, retire)) == []


@pytest.mark.parametrize("selector", ["regex:^rolling-[0-9]+$", None])
def test_a_mutable_channel_without_an_exact_tag_still_guards_its_inputs(tmp_path: Path, selector: str | None) -> None:
    root = tmp_path / "catalog"
    shutil.copytree(REPO_ROOT / "catalog", root)
    game_path = root / "rust" / "game.json"
    game = json.loads(game_path.read_text(encoding="utf-8"))
    channel = game["sources"]["carbon-api"]["watch"]["channels"]["production"]
    if selector is None:
        channel.pop("tag")
    else:
        channel["tag"] = selector
    game_path.write_text(json.dumps(game, indent=2) + "\n", encoding="utf-8")
    path = root / "rust" / "targets" / f"{TARGET}.json"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["inputs"]["carbon"]["tag"] = "rolling-123"
    record["inputs"]["carbon"].pop("assetId")
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    rows = _rows(loader.load(root))

    assert [row["status"] for row in rows] == ["fail"]
