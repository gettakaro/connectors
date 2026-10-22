"""The 7D2D ``prismacore`` verify family: the PrismaCore command contract over the protocol."""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Any

from fake_verify import CannedSocket, FakeRun
from takaro_maint.games.seven_days import checks_prismacore as family
from takaro_maint.games.seven_days import verify as hooks


def console(outputs: dict[str, str]) -> Any:
    def answer(action: str, params: Any) -> dict[str, Any]:
        del action
        return {"rawResult": outputs[params["command"]] + "\n", "success": True}

    return answer


def contract_outputs() -> dict[str, str]:
    return {**{command: expected for command, expected in family.CASES}, "isbloodmoon": "false"}


def test_the_family_is_part_of_the_7d2d_run() -> None:
    assert family in hooks.FAMILIES
    assert "prismacore" in hooks.CHECK_IDS


def test_every_command_and_id_form_is_covered() -> None:
    commands = [command for command, _ in family.CASES]
    for name in ("rl", "resetlevel", "rs", "resetskillpoints"):
        assert name in commands
    joined = " ".join(commands)
    assert family.OFFLINE_NAME in joined
    assert f"EOS_{family.OFFLINE_EOS}" in joined
    assert f"Steam_{family.OFFLINE_STEAM}" in joined
    expected = {e for _, e in family.CASES}
    assert family.RL_OFFLINE in expected and family.RS_OFFLINE in expected
    assert "ERR: Level must be 1 or greater." in expected
    assert "ERR: Count must be 0 or greater." in expected


def test_the_contract_passes_verbatim(tmp_path: Path) -> None:
    run = FakeRun(tmp_path)
    fake = CannedSocket({"executeConsoleCommand": console(contract_outputs())})

    result = asyncio.run(family.check_prismacore(run, fake))

    assert result.status == "pass", result.detail
    sent = [params["command"] for action, params, _ in fake.requests if action == "executeConsoleCommand"]
    assert sent[-1] == "isbloodmoon"
    assert len(sent) == len(family.CASES) + 1


def test_a_changed_line_an_unknown_command_or_a_bad_bloodmoon_fails(tmp_path: Path) -> None:
    outputs = contract_outputs()
    outputs[f"rl {family.OFFLINE_NAME} 5"] = "You have reset the level of this player to level 5."
    outputs["rs"] = "*** ERROR: unknown command 'rs'"
    outputs["isbloodmoon"] = "True"
    fake = CannedSocket({"executeConsoleCommand": console(outputs)})

    result = asyncio.run(family.check_prismacore(FakeRun(tmp_path), fake))

    assert result.status == "fail"
    problems = " ".join(result.detail["problems"])
    assert "rl TakaroVerifyOffline 5" in problems
    assert "'rs' printed" in problems
    assert "isbloodmoon" in problems


def test_a_failed_request_fails_and_an_unselected_check_is_skipped(tmp_path: Path) -> None:
    fake = CannedSocket({"executeConsoleCommand": RuntimeError("socket closed")})
    assert asyncio.run(family.check_prismacore(FakeRun(tmp_path), fake)).status == "fail"

    junk = CannedSocket({"executeConsoleCommand": {"success": True}})
    assert asyncio.run(family.check_prismacore(FakeRun(tmp_path / "junk"), junk)).status == "fail"

    skipped = FakeRun(tmp_path / "skipped", wanted=set())
    asyncio.run(family.run_checks(skipped, fake, lambda: True))
    assert skipped.skips == [("prismacore", "not selected by --checks")]


def test_before_boot_remembers_one_offline_player_where_the_server_looks(tmp_path: Path) -> None:
    run = FakeRun(tmp_path)
    (run.data_dir / "sdtdserver.xml").write_text(
        '<ServerSettings>\n  <property name="GameWorld" value="Pregen06k01"/>\n'
        '  <property name="GameName"\tvalue="Verify"/>\n</ServerSettings>\n',
        encoding="utf-8",
    )

    family.before_boot(run)

    path = run.data_dir / ".takaro/runtime/saves/Saves/Pregen06k01/Verify/players.xml"
    body = path.read_text(encoding="utf-8")
    assert f'userid="{family.OFFLINE_EOS}"' in body
    assert f'nativeuserid="{family.OFFLINE_STEAM}"' in body
    assert f'playername="{family.OFFLINE_NAME}"' in body
    assert 'lastlogin="20' in body


def test_the_default_save_is_navezgane(tmp_path: Path) -> None:
    assert family.players_xml_path(tmp_path) == tmp_path / ".takaro/runtime/saves/Saves/Navezgane/MyGame/players.xml"
