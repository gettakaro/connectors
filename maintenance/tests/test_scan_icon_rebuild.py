"""A new-build issue for a game with an ``icons`` block tells the maintainer to rebuild its Icon Pack.

Each scenario runs the real ``scan`` against the in-process fake GitHub with a stub game written into
a catalog copy, so none of them depends on which real games carry an ``icons`` block.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from fake_github import FakeGitHub
from takaro_maint.tracker import identity, issues
from test_seam_provider_issues import (  # noqa: F401
    _game_record,
    _head,
    _write,
    scan,
    stub_providers,
    support_issues,
    tracker,
)

pytestmark = pytest.mark.usefixtures("stub_providers")

CLIENT = " --client <client install>"


def line(catalog: str = "stubgame", pack: str = "stub-pack", client: str = CLIENT) -> str:
    return (
        "- [ ] the Icon Pack is rebuilt for the new target and published: "
        f"`maintenance/bin/takaro-maint icons build --game {catalog} --target <new target>{client} --out <out>`, "
        f"then `maintenance/bin/takaro-maint icons publish --env staging --pack <out>/pack/{pack}`, "
        f"then `maintenance/bin/takaro-maint icons promote --game {pack}`, "
        "then re-apply any earlier `icons takedown` on staging and production "
        "(see `maintenance/docs/icons.md`)"
    )


def with_icons(root: Path, **changes: Any) -> dict[str, Any]:
    block = json.loads((root / "catalog" / "zomboid" / "game.json").read_text(encoding="utf-8"))["icons"]
    block["game"] = "stub-pack"
    block.update(changes)
    record = _game_record("stubgame", "Stub Game", "stubprov", _head("public-888", "888"))
    record["icons"] = block
    return record


def owned(body: str) -> str:
    return body[body.index(issues.OWNED_BEGIN) : body.index(issues.OWNED_END) + len(issues.OWNED_END)]


def forget(fake: FakeGitHub, revision: str) -> None:
    dashboard = next(i for i in fake.issues if (identity.parse_marker(str(i["body"])) or {}).get("kind") == "dashboard")
    head, fence, rest = str(dashboard["body"]).partition("```json\n")
    raw, end, tail = rest.partition("\n```")
    state = json.loads(raw)
    for source in state["sources"].values():
        checkpoint = source["checkpoint"]
        checkpoint["seen"] = [entry for entry in checkpoint["seen"] if entry[0] != revision]
    dashboard["body"] = head + fence + json.dumps(state, sort_keys=True, indent=1) + end + tail


def test_a_new_build_of_a_game_with_an_icons_block_gets_the_rebuild_line(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _write(catalog_copy, with_icons(catalog_copy))

    with tracker(monkeypatch) as fake:
        code, payload, stderr = scan(run, catalog_copy, fake, "stubgame", "--bootstrap", "--publish")

        assert code == 0, stderr
        assert [e["action"] for e in payload["applied"]] == ["create-issue", "create-dashboard"]
        body = str(support_issues(fake)[0]["body"])

        assert body.count(line()) == 1
        block = owned(body)
        assert block.index("## Acceptance") < block.index(line())
        assert block.endswith(line() + "\n" + issues.OWNED_END)
        assert '→ "Adding a target" (pin build 888).' in body


def test_a_game_without_an_icons_block_gets_no_rebuild_line(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _write(catalog_copy, _game_record("plaingame", "Plain Game", "plainprov", _head("2.0", "12")))

    with tracker(monkeypatch) as fake:
        code, _, stderr = scan(run, catalog_copy, fake, "plaingame", "--bootstrap", "--publish")

        assert code == 0, stderr
        body = str(support_issues(fake)[0]["body"])
        assert "takaro-maint icons" not in body
        assert "Icon Pack" not in body
        assert "icons build" not in body
        acceptance = owned(body).split("## Acceptance\n\n")[1]
        assert len([row for row in acceptance.splitlines() if row.startswith("- [ ]")]) == 3


def test_an_open_issue_gains_the_line_when_the_game_gains_an_icons_block(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _write(catalog_copy, _game_record("stubgame", "Stub Game", "stubprov", _head("public-888", "888")))

    with tracker(monkeypatch) as fake:
        code, _, stderr = scan(run, catalog_copy, fake, "stubgame", "--bootstrap", "--publish")
        assert code == 0, stderr
        filed = support_issues(fake)[0]
        assert "Icon Pack" not in str(filed["body"])
        filed["body"] = str(filed["body"]) + "\n\nHuman note: waiting on the client build.\n"

        _write(catalog_copy, with_icons(catalog_copy))
        forget(fake, "public-888")
        code, payload, stderr = scan(run, catalog_copy, fake, "stubgame", "--publish")

        assert code == 0, stderr
        assert [e["action"] for e in payload["applied"]] == ["update-issue", "update-dashboard"]
        body = str(support_issues(fake)[0]["body"])
        assert body.count(line()) == 1
        assert body.endswith("\n\nHuman note: waiting on the client build.\n")


def test_a_rescan_with_the_icons_block_is_a_noop(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _write(catalog_copy, with_icons(catalog_copy))

    with tracker(monkeypatch) as fake:
        scan(run, catalog_copy, fake, "stubgame", "--bootstrap", "--publish")
        before = str(support_issues(fake)[0]["body"])

        forget(fake, "public-888")
        code, payload, stderr = scan(run, catalog_copy, fake, "stubgame", "--publish")

        assert code == 0, stderr
        assert [(e["action"], e.get("reason")) for e in payload["applied"]] == [
            ("noop", "tracked"),
            ("update-dashboard", None),
        ]
        assert str(support_issues(fake)[0]["body"]) == before


def test_a_pack_whose_pixels_do_not_come_from_a_client_omits_client(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Covers routes that later tickets add (server-only and public-download packs)."""
    record = with_icons(catalog_copy)
    record["icons"]["pixels"]["source"] = "server-depot"
    _write(catalog_copy, record)

    with tracker(monkeypatch) as fake:
        code, _, stderr = scan(run, catalog_copy, fake, "stubgame", "--bootstrap", "--publish")

        assert code == 0, stderr
        body = str(support_issues(fake)[0]["body"])
        assert body.count(line(client="")) == 1
        assert "--client" not in body


def test_a_malformed_icons_block_does_not_break_the_scan(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    record = _game_record("stubgame", "Stub Game", "stubprov", _head("public-888", "888"))
    record["icons"] = {"mapper": "x"}
    _write(catalog_copy, record)

    with tracker(monkeypatch) as fake:
        code, _, stderr = scan(run, catalog_copy, fake, "stubgame", "--bootstrap", "--publish")

        assert code == 0, stderr
        assert line(pack="stubgame", client="") in str(support_issues(fake)[0]["body"])
