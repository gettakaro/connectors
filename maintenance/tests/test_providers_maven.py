"""The generic Maven repository provider: acquisition by sha256 and discovery from maven-metadata.xml."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from takaro_maint.exit_codes import MaintError, UsageError
from takaro_maint.providers import provider_for

GROUP_PATH = "/release/com/example/server/Server"


def metadata(*versions: str) -> bytes:
    rows = "".join(f"<version>{v}</version>" for v in versions)
    head = versions[0] if versions else ""
    return (
        "<?xml version='1.0'?><metadata><groupId>com.example.server</groupId><artifactId>Server</artifactId>"
        f"<versioning><latest>{head}</latest><release>{head}</release>"
        f"<versions>{rows}</versions></versioning></metadata>"
    ).encode()


def source(base_url: str, **channels: Any) -> dict[str, Any]:
    return {
        "id": "server-maven",
        "provider": "maven",
        "baseUrl": base_url,
        "watch": {
            "kind": "game",
            "component": "dummy",
            "group": "com.example.server",
            "artifact": "Server",
            "channels": channels
            or {"release": {"branch": "release", "metadataPath": GROUP_PATH + "/maven-metadata.xml"}},
        },
    }


def observe(*versions: str, **channels: Any) -> Any:
    from fake_upstream import FakeUpstream

    with FakeUpstream() as upstream:
        upstream.add(GROUP_PATH + "/maven-metadata.xml", metadata(*versions))
        return provider_for("maven").observe(source(upstream.base_url, **channels))


def test_the_head_is_the_last_listed_version_not_the_release_pointer() -> None:
    result = observe("0.7.7", "0.5.10", "0.6.9", "0.7.5", "0.7.7")

    assert result.heads == {"release": "0.7.7"}
    assert result.history == "heads-only"
    only = result.observations[0]
    assert (only.kind, only.component, only.rev) == ("game", "dummy", "0.7.7")
    assert only.facts["coordinate"] == "com.example.server:Server:0.7.7"
    assert only.facts["artifact"]["url"].endswith("/release/com/example/server/Server/0.7.7/Server-0.7.7.jar")


def test_a_disabled_channel_is_not_fetched() -> None:
    result = observe(
        "1.0.0",
        release={"branch": "release", "metadataPath": GROUP_PATH + "/maven-metadata.xml"},
        **{"pre-release": {"branch": "pre-release", "metadataPath": "/pre/missing.xml", "enabled": False}},
    )

    assert result.heads == {"release": "1.0.0"}


def test_every_observation_matches_the_observation_schema() -> None:
    from takaro_maint import observations

    for observation in observe("1.0.0", "1.1.0").observations:
        document = observations.to_json(observation, game="dummy", source_id="server-maven")
        assert observations.validate(document) == [], document


def test_a_channel_without_a_metadata_path_is_a_usage_error() -> None:
    with pytest.raises(UsageError):
        observe("1.0.0", release={"branch": "release"})


def test_an_empty_listing_fails_the_source() -> None:
    with pytest.raises(MaintError):
        observe()


def test_fetch_input_checks_the_pinned_digest(tmp_path: Path) -> None:
    from conftest import sha256
    from fake_upstream import FakeUpstream

    payload = b"server jar bytes"
    with FakeUpstream() as upstream:
        upstream.add("/release/x/Server-1.jar", payload)
        url = upstream.base_url + "/release/x/Server-1.jar"
        spec = {"kind": "http-file", "sha256": sha256(payload), "size": len(payload)}
        out = provider_for("maven").fetch_input(spec, {"url": url}, tmp_path / "Server.jar", tmp_path / "cache")
        assert out.read_bytes() == payload
        with pytest.raises(MaintError):
            provider_for("maven").fetch_input(
                {**spec, "sha256": "0" * 64}, {"url": url}, tmp_path / "Other.jar", tmp_path / "cache2"
            )
