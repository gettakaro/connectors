"""7D2D verify family ``prismacore``: the PrismaCore-compatible console commands.

Every command runs through the protocol's ``executeConsoleCommand`` and its output is
compared verbatim with the PrismaCore output contract. ``rl``/``rs`` need a player the
registry knows but who is not online; a verify server has no clients, so ``before_boot``
writes one into the save's ``players.xml``, which the mod adds to its registry at boot.

A family module exposes ``CHECK_IDS``, ``before_boot(run)`` and ``run_checks(run, fake,
alive)``; ``verify.py`` composes the families, so a new command family is a new file.
"""

from __future__ import annotations

import re
import time
from pathlib import Path
from typing import Any

from ...verify import checks

CHECK_IDS = ("prismacore",)

# A player the game remembers but who never connects during the run.
OFFLINE_EOS = "0002f304f304f304f304f304f304f304"
OFFLINE_STEAM = "76561190000000304"
OFFLINE_NAME = "TakaroVerifyOffline"
UNKNOWN_NAME = "TakaroVerifyNobody"

PLAYERS_XML = """<?xml version="1.0" encoding="UTF-8"?>
<!-- Written by takaro-maint verify: one offline player for the prismacore checks. -->
<persistentplayerdata version="1">
  <player platform="EOS" userid="{eos}" nativeplatform="Steam" nativeuserid="{steam}"
          playername="{name}" playgroup="Standalone" lastlogin="{lastlogin}" position="0,60,0" />
</persistentplayerdata>
"""

WRONG_ARGS = "ERR: Wrong number of arguments, expected 2, found {n}"
RL_OFFLINE = "ERR: Player is offline. Player MUST be online for level reset."
RS_OFFLINE = "ERR: Player is offline. Player MUST be online for skillpoints reset."

#: (command, exact expected output). The offline player is addressed by every id form.
CASES: tuple[tuple[str, str], ...] = (
    ("rl", WRONG_ARGS.format(n=0)),
    ("resetlevel", WRONG_ARGS.format(n=0)),
    ("rs", WRONG_ARGS.format(n=0)),
    ("resetskillpoints", WRONG_ARGS.format(n=0)),
    (f"rl {OFFLINE_NAME}", WRONG_ARGS.format(n=1)),
    (f"rs {OFFLINE_NAME} 1 2", WRONG_ARGS.format(n=3)),
    (f"rl {OFFLINE_NAME} abc", "ERR: Invalid level parameter abc. Must be a valid integer"),
    (f"rs {OFFLINE_NAME} x", "ERR: Invalid count parameter x. Must be a valid integer"),
    (f"rl {OFFLINE_NAME} 0", "ERR: Level must be 1 or greater."),
    (f"rs {OFFLINE_NAME} -1", "ERR: Count must be 0 or greater."),
    (f"rl {OFFLINE_NAME} 5", RL_OFFLINE),
    (f"resetlevel EOS_{OFFLINE_EOS} 5", RL_OFFLINE),
    (f"rs Steam_{OFFLINE_STEAM} 3", RS_OFFLINE),
    (f"resetskillpoints {OFFLINE_NAME.lower()} 0", RS_OFFLINE),
    (f"rl {UNKNOWN_NAME} 5", f'"{UNKNOWN_NAME}" is not a valid entity id, player name or user id.'),
    (f"rs EOS_{'f' * 32} 5", f'"EOS_{"f" * 32}" is not a valid entity id, player name or user id.'),
)

BLOOD_MOON = re.compile(r"\A(true|false)\Z")


def _server_setting(data_dir: Path, name: str, default: str) -> str:
    for config in ("sdtdserver.xml", "serverconfig.xml"):
        path = data_dir / config
        if path.is_file():
            found = re.search(
                rf'<property\s+name="{name}"\s+value="([^"]*)"', path.read_text(encoding="utf-8", errors="replace")
            )
            if found:
                return found.group(1)
    return default


def players_xml_path(data_dir: Path) -> Path:
    """Where this server keeps players.xml: the saves mount, then Saves/<world>/<game>/."""
    world = _server_setting(data_dir, "GameWorld", "Navezgane")
    game = _server_setting(data_dir, "GameName", "MyGame")
    return data_dir / ".takaro" / "runtime" / "saves" / "Saves" / world / game / "players.xml"


def before_boot(run: Any) -> None:
    path = players_xml_path(run.data_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        PLAYERS_XML.format(
            eos=OFFLINE_EOS,
            steam=OFFLINE_STEAM,
            name=OFFLINE_NAME,
            lastlogin=time.strftime("%Y-%m-%d %H:%M:%S"),
        ),
        encoding="utf-8",
    )


async def run_checks(run: Any, fake: Any, alive: Any) -> None:
    del alive
    if run.wanted("prismacore"):
        run.record(await check_prismacore(run, fake))
    else:
        run.skip("prismacore", "not selected by --checks")


async def check_prismacore(run: Any, fake: Any) -> checks.CheckResult:
    """rl, rs and isbloodmoon are registered and print the PrismaCore contract verbatim."""
    with checks._Timer() as timer:
        problems: list[str] = []
        results: list[dict[str, Any]] = []
        for command, expected in CASES:
            output = await _console(fake, command, problems)
            results.append({"command": command, "expected": expected, "output": output})
            if output is not None and output != expected:
                problems.append(f"'{command}' printed {output!r}, expected {expected!r}")
        moon = await _console(fake, "isbloodmoon", problems)
        results.append({"command": "isbloodmoon", "expected": "true|false", "output": moon})
        if moon is not None and not BLOOD_MOON.match(moon):
            problems.append(f"'isbloodmoon' printed {moon!r}, expected exactly true or false")
    return checks.CheckResult(
        "prismacore",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"commands": results, "offlinePlayer": OFFLINE_NAME, "problems": problems},
        {"file": run.server_log.name},
    )


async def _console(fake: Any, command: str, problems: list[str]) -> str | None:
    try:
        answer = await fake.request("executeConsoleCommand", {"command": command})
    except Exception as exc:  # noqa: BLE001 - reported as a check failure
        problems.append(f"'{command}' failed: {exc}")
        return None
    if not isinstance(answer, dict) or not isinstance(answer.get("rawResult"), str):
        problems.append(f"'{command}' answered {answer!r}, expected a rawResult")
        return None
    return str(answer["rawResult"]).strip()
