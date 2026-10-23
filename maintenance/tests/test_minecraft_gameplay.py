"""Minecraft's gameplay rows against a scripted player and a scripted connector.

The real player is a Mineflayer process in a container; here it is an in-process stand-in
with the same surface, wired to a connector stand-in that changes its state the way the
server would. Each test breaks one link of the chain and requires the row that depends on
it, and only that row, to fail.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from fake_verify import FakeRun
from takaro_maint.games.minecraft import gameplay
from takaro_maint.games.minecraft import verify as minecraft_verify
from takaro_maint.verify import report

GAME_ID = "99983668-e0d2-3759-ba5e-93af316b1fdc"
SPAWN = {"x": -117.5, "y": 81.0, "z": -174.5}


class ScriptedPlayer:
    """The surface ``Gameplay`` uses of a ``Player``, with the world kept in memory."""

    def __init__(self, username: str, *, spawns: bool = True) -> None:
        self.username = username
        self.events: list[dict[str, Any]] = []
        self.inventory: dict[str, int] = {}
        self.position = dict(SPAWN)
        self.process = SimpleNamespace(returncode=None, kill=lambda: None)
        if spawns:
            self.events.append(
                {"event": "spawn", "username": username, "uuid": GAME_ID, "version": "1.21.11", "position": SPAWN}
            )
        else:
            self.events.append({"event": "kicked", "reason": "outdated server"})

    async def send(self, op: str, **fields: Any) -> bool:
        if self.gone():
            return False
        if op == "inventory":
            items = [{"name": name, "count": count} for name, count in self.inventory.items()]
            self.events.append({"event": "inventory", "items": items})
        elif op == "chat":
            self.events.append({"event": "chat", "text": fields["text"]})
            self.connector.said(fields["text"])
        elif op == "quit":
            self.process.returncode = 0
            self.connector.left()
        return True

    def kick(self, reason: str) -> None:
        self.events.append({"event": "kicked", "reason": reason})
        self.process.returncode = 1
        self.connector.left()

    def why_gone(self) -> str:
        return f"the player is no longer on the server ({self.events[-1]})"

    def gone(self) -> bool:
        return self.process.returncode is not None or any(e.get("event") == "kicked" for e in self.events)

    async def wait_for(self, predicate: Any, timeout: float, *, since: int = 0) -> dict[str, Any] | None:
        del timeout
        return next((event for event in self.events[since:] if predicate(event)), None)

    async def ask(self, op: str, answer: str, timeout: float = 10.0) -> dict[str, Any] | None:
        mark = len(self.events)
        await self.send(op)
        return await self.wait_for(lambda event: event.get("event") == answer, timeout, since=mark)

    async def close(self, timeout: float) -> int | None:
        del timeout
        return self.process.returncode


class ScriptedConnector:
    """The fake Takaro's view of a connector, with switches that break one behaviour each."""

    def __init__(self, player: ScriptedPlayer, *, broken: set[str] = frozenset()) -> None:  # type: ignore[assignment]
        self.player = player
        player.connector = self  # type: ignore[attr-defined]
        self.broken = broken
        self.events: list[dict[str, Any]] = []
        self.online = True
        self.requests: list[tuple[str, Any]] = []
        self._emit("player-connected", {"player": {"gameId": GAME_ID, "name": player.username}})

    def _emit(self, kind: str, data: dict[str, Any]) -> None:
        self.events.append({"type": kind, "data": data})

    def said(self, text: str) -> None:
        if "chat-event" not in self.broken:
            self._emit("chat-message", {"player": {"gameId": GAME_ID, "name": self.player.username}, "msg": text})

    def left(self) -> None:
        self.online = False
        self._emit("player-disconnected", {"player": {"gameId": GAME_ID, "name": self.player.username}})

    async def request(self, action: str, args: Any, timeout: float = 30.0) -> Any:
        del timeout
        self.requests.append((action, args))
        if action == "getPlayers":
            return [{"gameId": GAME_ID, "name": self.player.username}] if self.online else []
        if action == "sendMessage":
            self.player.events.append({"event": "message", "text": args["message"]})
            return {}
        if action == "giveItem":
            if "kick-on-give" in self.broken:
                self.player.inventory["diamond"] = args["amount"]
                self.player.kick("multiplayer.disconnect.invalid_player_movement")
                return {}
            if "raise-on-give" in self.broken:
                raise ConnectionResetError("Connection lost")
            if "give" not in self.broken:
                name = args["item"].removeprefix("minecraft:")
                self.player.inventory[name] = self.player.inventory.get(name, 0) + args["amount"]
            return {}
        if action == "getPlayerInventory":
            return [
                {"code": f"minecraft:{name}", "name": name.title(), "amount": count, "quality": ""}
                for name, count in self.player.inventory.items()
            ]
        if action == "teleportPlayer":
            if "teleport" not in self.broken:
                self.player.position = {axis: float(args[axis]) for axis in ("x", "y", "z")}
                self.player.events.append({"event": "forcedMove", "position": dict(self.player.position)})
            return {}
        if action == "getPlayerLocation":
            # The player is falling: y has dropped, x and z have not moved.
            return {**self.player.position, "y": self.player.position["y"] - 3, "dimension": "minecraft:overworld"}
        raise AssertionError(f"unexpected request {action}")


class GameplayRun(FakeRun):
    def __init__(self, tmp_path: Path, revision: str = "1.21.11", **kwargs: Any) -> None:
        super().__init__(tmp_path, target=SimpleNamespace(id="paper-1.21.11", record={"revision": revision}), **kwargs)
        self.nonce = "a1b2c3"
        self.container_name = "takaro-verify-minecraft-paper-1.21.11-t1"
        self.extra_logs: list[Path] = []
        self.resolved = {"containerRef": "itzg/minecraft-server@sha256:" + "0" * 64}

    def not_selected_reason(self, check_id: str) -> str:
        del check_id
        return "not selected by --checks"


def play(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, *, broken: set[str] = set(), spawns: bool = True) -> Any:  # noqa: B006
    run = GameplayRun(tmp_path)
    monkeypatch.setattr(gameplay, "install_client", lambda pinned: tmp_path / "kit")
    connectors: list[ScriptedConnector] = []

    async def start_player(self: gameplay.Gameplay, kit: Path) -> ScriptedPlayer:
        del kit
        player = ScriptedPlayer(self.username, spawns=spawns)
        connector = ScriptedConnector(player, broken=broken)
        # The fake Takaro is the object Gameplay already holds; give it the connector's state.
        self.fake.events = connector.events
        self.fake.request = connector.request
        connectors.append(connector)
        return player

    monkeypatch.setattr(gameplay.Gameplay, "start_player", start_player)
    monkeypatch.setattr(gameplay, "EVENT_BUDGET", 0.2)
    fake = SimpleNamespace(events=[], request=None)
    asyncio.run(gameplay.run_gameplay(run, fake))
    return run


def statuses(run: Any) -> dict[str, str]:
    found = {result.id: result.status for result in run.results}
    found.update({check_id: "skip" for check_id, _ in run.skips})
    return found


def test_a_player_that_does_everything_passes_every_row(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    run = play(tmp_path, monkeypatch)
    assert statuses(run) == dict.fromkeys(gameplay.CHECK_IDS, "pass")
    join = next(result for result in run.results if result.id == "gameplay-join")
    assert join.detail["client"]["viaProxy"] is None
    assert join.detail["client"]["loginVersion"] == "1.21.11"
    assert join.detail["playerConnected"]["player"]["gameId"] == GAME_ID
    teleport = next(result for result in run.results if result.id == "gameplay-teleport")
    assert teleport.detail["target"] == {"x": -117.5 + 8, "y": 81.0 + 12, "z": -174.5 + 8}


@pytest.mark.parametrize(
    ("broken", "failing"),
    [
        ({"chat-event"}, {"gameplay-chat"}),
        ({"give"}, {"gameplay-give-item", "gameplay-inventory"}),
        ({"teleport"}, {"gameplay-teleport"}),
    ],
)
def test_a_broken_behaviour_fails_its_own_rows(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, broken: set[str], failing: set[str]
) -> None:
    run = play(tmp_path, monkeypatch, broken=broken)
    assert {check_id for check_id, status in statuses(run).items() if status == "fail"} == failing
    assert {check_id for check_id, status in statuses(run).items() if status == "pass"} == set(
        gameplay.CHECK_IDS
    ) - failing


def test_a_player_that_never_spawns_fails_join_and_skips_the_rest(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    run = play(tmp_path, monkeypatch, spawns=False)
    assert [result.id for result in run.results] == ["gameplay-join"]
    assert run.results[0].status == "fail"
    assert "did not spawn" in run.results[0].detail["problems"][0]
    assert {check_id for check_id, _ in run.skips} == set(gameplay.CHECK_IDS[1:])


def test_the_gameplay_rows_run_before_the_lifecycle_rows() -> None:
    assert minecraft_verify.CHECK_IDS[: len(gameplay.CHECK_IDS)] == gameplay.CHECK_IDS
    assert report.GAME_GAMEPLAY_CHECKS["minecraft"] == gameplay.CHECK_IDS


@pytest.mark.parametrize(
    ("revision", "proxied", "login"),
    [("1.21.11", False, "1.21.11"), ("26.1.2", False, "26.1.2"), ("26.2", True, "26.1"), ("26.3", True, "26.1")],
)
def test_only_targets_mineflayer_cannot_speak_go_through_viaproxy(revision: str, proxied: bool, login: str) -> None:
    pinned = gameplay.pins()
    assert gameplay.needs_proxy(pinned, revision) is proxied
    assert gameplay.client_version(pinned, revision) == login


def test_every_maintained_target_has_a_route_in(repo_root: Path) -> None:
    pinned = gameplay.pins()
    for path in sorted((repo_root / "catalog/minecraft/targets").glob("*.json")):
        record = json.loads(path.read_text())
        if record["support"]["status"] == "retired":
            continue
        assert gameplay.client_version(pinned, record["revision"]), record["id"]


def test_the_client_pins_agree_with_the_lockfile() -> None:
    pinned = gameplay.pins()
    lock = json.loads((gameplay.CLIENT_DIR / "package-lock.json").read_text())
    assert lock["packages"]["node_modules/mineflayer"]["version"] == pinned["mineflayer"]["version"]
    assert (
        json.loads((gameplay.CLIENT_DIR / "package.json").read_text())["dependencies"]["mineflayer"]
        == (pinned["mineflayer"]["version"])
    )
    assert pinned["node"]["digest"].startswith("sha256:")
    assert len(pinned["viaproxy"]["sha256"]) == 64


def _rows(status: str = "pass") -> list[dict[str, Any]]:
    base = [
        {"id": check_id, "status": "pass", "detail": {}}
        for check_id in (
            "build",
            "startup",
            "connector-load",
            "identify",
            "heartbeat",
            "players",
            "catalog-items",
            "catalog-entities",
            "console",
            "shutdown",
        )
    ]
    play_rows = [{"id": check_id, "status": status, "detail": {}} for check_id in gameplay.CHECK_IDS]
    play_rows[0]["status"] = "pass"
    play_rows[0]["detail"] = {
        "client": {
            "library": "mineflayer@4.39.0",
            "loginVersion": "26.1",
            "viaProxy": {"version": "3.4.14", "targetVersion": "26.2"},
        },
        "manualCoverage": ["online-mode login"],
    }
    return base + play_rows


def test_gameplay_is_a_level_above_protocol_only_where_a_game_drives_a_player() -> None:
    assert report.level_for(_rows(), "minecraft") == "gameplay"
    assert report.level_for(_rows("fail"), "minecraft") == "protocol"
    assert report.level_for(_rows(), "valheim") == "protocol"


def test_the_coverage_names_what_was_played_how_and_what_stays_manual() -> None:
    covered = report.gameplay_coverage(_rows(), "minecraft")
    assert covered.startswith("covered - join, chat, give-item, inventory, teleport, leave")
    assert "through ViaProxy 3.4.14" in covered
    assert covered.endswith("manual: online-mode login")
    uncovered = report.gameplay_coverage(_rows("fail"), "minecraft")
    assert uncovered.startswith("not covered - gameplay-chat, gameplay-give-item")
    assert report.gameplay_coverage(_rows(), "valheim") == report.NO_GAMEPLAY


def test_a_player_kicked_mid_run_fails_the_rest_and_the_run_still_reports(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    run = play(tmp_path, monkeypatch, broken={"kick-on-give"})
    found = statuses(run)
    assert found["gameplay-join"] == found["gameplay-chat"] == "pass"
    assert {found[check_id] for check_id in gameplay.CHECK_IDS[2:]} == {"fail"}
    teleport = next(result for result in run.results if result.id == "gameplay-teleport")
    assert "invalid_player_movement" in teleport.detail["problems"][0]


def test_a_request_that_raises_fails_its_row_and_the_rest_go_on(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    run = play(tmp_path, monkeypatch, broken={"raise-on-give"})
    give = next(result for result in run.results if result.id == "gameplay-give-item")
    assert give.status == "fail"
    assert give.detail["problems"][0] == "giveItem failed: Connection lost"
    assert statuses(run)["gameplay-leave"] == "pass"


def test_an_exception_a_row_did_not_expect_becomes_its_failure() -> None:
    async def row() -> Any:
        raise ConnectionResetError("Connection lost")

    result = asyncio.run(gameplay._guarded("gameplay-leave", row))
    assert (result.id, result.status) == ("gameplay-leave", "fail")
    assert result.detail["problems"] == ["ConnectionResetError: Connection lost"]


# A stand-in for client.mjs speaking the same line protocol, so the real Player class is
# exercised against a real process: its pipes, its JSON parsing, its exit and a dead stdin.
ECHO_CLIENT = """
import json, sys
print(json.dumps({"event": "spawn", "uuid": "u", "position": {"x": 0, "y": 0, "z": 0}}), flush=True)
print("not json", flush=True)
print("diagnostics", file=sys.stderr, flush=True)
for line in sys.stdin:
    op = json.loads(line)["op"]
    if op == "inventory":
        print(json.dumps({"event": "inventory", "items": [{"name": "diamond", "count": 3}]}), flush=True)
    elif op == "quit":
        print(json.dumps({"event": "end", "reason": "quit"}), flush=True)
        sys.exit(0)
"""


def test_the_player_speaks_the_line_protocol_over_real_pipes(tmp_path: Path) -> None:
    import subprocess
    import sys

    async def scenario() -> tuple[Any, ...]:
        process = await asyncio.create_subprocess_exec(
            sys.executable, "-c", ECHO_CLIENT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        player = gameplay.Player(process=process, log_path=tmp_path / "client.log")
        player.start_reading()
        spawn = await player.wait_for(lambda event: event.get("event") == "spawn", 10)
        inventory = await player.ask("inventory", "inventory")
        quit_sent = await player.send("quit")
        code = await player.close(10)
        after_exit = await player.send("chat", text="too late")
        return spawn, inventory, quit_sent, code, after_exit, player.gone()

    spawn, inventory, quit_sent, code, after_exit, gone = asyncio.run(scenario())
    assert spawn is not None and spawn["uuid"] == "u"
    assert gameplay._count(inventory, "diamond") == 3
    assert (quit_sent, code, after_exit, gone) == (True, 0, False, True)
    log = (tmp_path / "client.log").read_text()
    assert "out not json" in log and "err diagnostics" in log
