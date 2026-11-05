"""The download table in a release body (gettakaro/connectors#756)."""

from __future__ import annotations

from typing import Any

from takaro_maint.publish.channels import ASSETS_BEGIN, ASSETS_END, merge_body, render_table


def _record() -> dict[str, Any]:
    def artifact(name: str) -> dict[str, str]:
        return {"name": name, "url": f"https://example.invalid/{name}", "sha256": "ab" * 32}

    verification = {"required": "contract", "executed": None}
    return {
        "mode": "catalog",
        "tag": "7d2d-v0.7.1",
        "source": {"repo": "o/r"},
        "self": "takaro-7d2d-0.7.1.compat.json",
        "assets": [],
        "aliases": {},
        "targets": {
            "linux-3.2.0.b10": {
                "platform": "linux",
                "revision": "3.2.0.b10",
                "verification": verification,
                "artifacts": [artifact("takaro-7d2d-mod-3.2.0.b10-0.7.1.zip")],
            },
            "linux-3.4.0.b9": {
                "platform": "linux",
                "revision": "3.4.0.b9",
                "verification": verification,
                "artifacts": [artifact("takaro-7d2d-mod-3.4.0.b9-0.7.1.zip")],
            },
        },
    }


def test_each_row_leads_with_the_server_build_it_is_for() -> None:
    table = render_table(_record())

    assert "| your server build | download |" in table
    assert "| linux 3.4.0.b9 | [takaro-7d2d-mod-3.4.0.b9-0.7.1.zip]" in table
    assert "| linux 3.2.0.b10 | [takaro-7d2d-mod-3.2.0.b10-0.7.1.zip]" in table


def test_an_old_name_row_says_which_build_it_copies() -> None:
    record = _record()
    record["aliases"] = {
        "takaro-7d2d-mod.zip": {
            "target": "linux-3.2.0.b10",
            "of": "takaro-7d2d-mod-3.2.0.b10-0.7.1.zip",
            "sha256": "cd" * 32,
        }
    }

    assert "| linux 3.2.0.b10 (old name) | [takaro-7d2d-mod.zip]" in render_table(record)


def test_a_new_body_puts_the_download_table_before_the_changelog() -> None:
    body = merge_body("## Changelog\n\n* fix: something", "TABLE")

    assert body.index("TABLE") < body.index("## Changelog")
    assert body.startswith(ASSETS_BEGIN)


def test_an_existing_table_is_replaced_where_it_is() -> None:
    existing = f"## Changelog\n\n{ASSETS_BEGIN}\nOLD\n{ASSETS_END}\n"

    body = merge_body(existing, "NEW")

    assert body == f"## Changelog\n\n{ASSETS_BEGIN}\nNEW\n{ASSETS_END}\n"
