"""Minecraft's gameplay checks: a headless player joins the target and Takaro drives it.

The protocol ladder proves the connector answers on an empty server. These checks put a
player on it. A Mineflayer client (``client/``) joins the booted container through its
network namespace, so no host port is published. Then the fake Takaro sends the same
requests real Takaro would, and every row asserts the game state the player itself observed
next to what the connector reported and emitted: the inventory the client holds, the
position the server forced it to, the chat line it received.

Mineflayer speaks the protocols in ``client.json``'s ``nativeVersions``. A newer target
is reached through ViaProxy, which runs in the server's network namespace too, on the
target's own pinned image (it already carries the Java ViaProxy needs). Every report names
which way its player came in, so a translated run never passes for a native one.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import math
import os
import re
import shutil
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ... import net, paths
from ...verify import checks
from ...verify.runner import Container, docker_command

CLIENT_DIR = Path(__file__).parent / "client"

CHECK_IDS = (
    "gameplay-join",
    "gameplay-chat",
    "gameplay-give-item",
    "gameplay-inventory",
    "gameplay-teleport",
    "gameplay-leave",
)

#: What a real game client still has to show, because nothing here exercises it.
MANUAL_COVERAGE = (
    "online-mode (Microsoft account) login; CI servers run offline-mode",
    "the vanilla client itself: rendering, UI and typed slash commands",
    "native 26.2+ client protocol where the report names ViaProxy",
    "player-death and entity-killed events",
)

GAME_PORT = 25565
PROXY_PORT = 25568
SPAWN_BUDGET = 120.0
PROXY_BUDGET = 90.0
EVENT_BUDGET = 30.0
QUIT_BUDGET = 30.0
GIVE_ITEM = ("minecraft:diamond", "diamond", 3)
# How far above and beside spawn the player is sent. High enough to land in air, close
# enough to stay inside the chunks the server already loaded.
TELEPORT_OFFSET = (8.0, 12.0, 8.0)
POSITION_TOLERANCE = 0.6
# The connector reads the location after the teleport while the player falls straight
# down, so only x and z have to stay put.
LOCATION_TOLERANCE = 1.0

PROXY_READY = re.compile(r"Binding proxy server to")


def pins() -> dict[str, Any]:
    pinned: dict[str, Any] = json.loads((CLIENT_DIR / "client.json").read_text(encoding="utf-8"))
    return pinned


def node_image(pinned: dict[str, Any]) -> str:
    return f"{pinned['node']['image']}@{pinned['node']['digest']}"


def needs_proxy(pinned: dict[str, Any], revision: str) -> bool:
    return revision not in pinned["mineflayer"]["nativeVersions"]


def client_version(pinned: dict[str, Any], revision: str) -> str:
    """The protocol the player logs in with: the target's own, or the proxy's client side."""
    return str(pinned["mineflayer"]["proxyClientVersion"]) if needs_proxy(pinned, revision) else revision


def client_cache() -> Path:
    """Everything the player needs that is not in the repository; CI caches this one directory."""
    return paths.cache_dir() / "minecraft-client"


def kit_dir() -> Path:
    """Where this lockfile's ``node_modules`` live, so a cache hit is exactly this lockfile."""
    lock = net.sha256_file(CLIENT_DIR / "package-lock.json")
    return client_cache() / lock[:16]


def install_client(pinned: dict[str, Any]) -> Path:
    """``npm ci`` the client once per lockfile, inside the pinned Node image."""
    kit = kit_dir()
    if (kit / ".complete").is_file():
        return kit
    shutil.rmtree(kit, ignore_errors=True)
    kit.mkdir(parents=True)
    for name in ("package.json", "package-lock.json"):
        shutil.copy2(CLIENT_DIR / name, kit / name)
    npm_cache = client_cache() / "npm"
    npm_cache.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [
            *docker_command(),
            "run",
            "--rm",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "-e",
            "HOME=/tmp",
            "-e",
            "npm_config_cache=/npm-cache",
            "-v",
            f"{kit}:/client",
            "-v",
            f"{npm_cache}:/npm-cache",
            "-w",
            "/client",
            node_image(pinned),
            "npm",
            "ci",
            "--ignore-scripts",
            "--no-audit",
            "--no-fund",
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=600,
    )
    if completed.returncode != 0:
        shutil.rmtree(kit, ignore_errors=True)
        raise RuntimeError(f"npm ci exited {completed.returncode}: {(completed.stdout + completed.stderr)[-2000:]}")
    (kit / ".complete").write_text(pinned["mineflayer"]["version"] + "\n", encoding="utf-8")
    return kit


def viaproxy_jar(pinned: dict[str, Any]) -> Path:
    spec = pinned["viaproxy"]
    return net.download(spec["url"], net.Expectation(sha256=spec["sha256"], size=int(spec["size"])), client_cache())


# --------------------------------------------------------------------------- the player


@dataclass
class Player:
    """One client process: what it said so far, and a way to ask it things."""

    process: asyncio.subprocess.Process
    log_path: Path
    events: list[dict[str, Any]] = field(default_factory=list)
    _readers: list[asyncio.Task[None]] = field(default_factory=list)

    def start_reading(self) -> None:
        assert self.process.stdout is not None and self.process.stderr is not None
        self._readers = [
            asyncio.create_task(self._read(self.process.stdout, "out")),
            asyncio.create_task(self._read(self.process.stderr, "err")),
        ]

    async def _read(self, stream: asyncio.StreamReader, channel: str) -> None:
        with self.log_path.open("a", encoding="utf-8") as handle:
            async for raw in stream:
                line = raw.decode("utf-8", errors="replace").rstrip("\n")
                handle.write(f"{channel} {line}\n")
                handle.flush()
                if channel != "out":
                    continue
                with contextlib.suppress(json.JSONDecodeError):
                    event = json.loads(line)
                    if isinstance(event, dict):
                        self.events.append(event)

    async def send(self, op: str, **fields: Any) -> bool:
        """Whether the command reached the player; one that has already left cannot be told."""
        assert self.process.stdin is not None
        try:
            self.process.stdin.write((json.dumps({"op": op, **fields}) + "\n").encode())
            await self.process.stdin.drain()
        except (BrokenPipeError, ConnectionResetError):
            return False
        return True

    def gone(self) -> bool:
        return self.process.returncode is not None or any(e.get("event") in ("end", "kicked") for e in self.events)

    def why_gone(self) -> str:
        last = next((e for e in reversed(self.events) if e.get("event") in ("kicked", "end")), None)
        return f"the player is no longer on the server ({last or f'exit {self.process.returncode}'})"

    async def wait_for(
        self, predicate: Callable[[dict[str, Any]], bool], timeout: float, *, since: int = 0
    ) -> dict[str, Any] | None:
        """The first event from index ``since`` on that matches, or ``None`` at the deadline."""
        deadline = time.monotonic() + timeout
        while True:
            for event in self.events[since:]:
                if predicate(event):
                    return event
            if time.monotonic() >= deadline or self.process.returncode is not None:
                return None
            await asyncio.sleep(0.1)

    async def ask(self, op: str, answer: str, timeout: float = 10.0) -> dict[str, Any] | None:
        mark = len(self.events)
        await self.send(op)
        return await self.wait_for(lambda event: event.get("event") == answer, timeout, since=mark)

    async def close(self, timeout: float) -> int | None:
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(self.process.wait(), timeout)
        for reader in self._readers:
            with contextlib.suppress(asyncio.TimeoutError, asyncio.CancelledError):
                await asyncio.wait_for(reader, 5)
        return self.process.returncode


def game_events(fake: Any, kind: str, since: int = 0) -> list[dict[str, Any]]:
    """The ``data`` of every ``kind`` event the connector pushed, v0 and v1 framing alike."""
    found = []
    for payload in fake.events[since:]:
        if isinstance(payload, dict) and payload.get("type") == kind and isinstance(payload.get("data"), dict):
            found.append(payload["data"])
    return found


async def wait_game_event(
    fake: Any, kind: str, predicate: Callable[[dict[str, Any]], bool], timeout: float, since: int = 0
) -> dict[str, Any] | None:
    deadline = time.monotonic() + timeout
    while True:
        for data in game_events(fake, kind, since):
            if predicate(data):
                return data
        if time.monotonic() >= deadline:
            return None
        await asyncio.sleep(0.1)


def _player_of(data: dict[str, Any]) -> dict[str, Any]:
    player = data.get("player")
    return player if isinstance(player, dict) else {}


def _result(check_id: str, timer: Any, detail: dict[str, Any], problems: list[str]) -> checks.CheckResult:
    return checks.CheckResult(
        check_id, "pass" if not problems else "fail", timer.elapsed_ms, {**detail, "problems": problems}
    )


async def _request(fake: Any, action: str, args: dict[str, Any], problems: list[str]) -> Any:
    try:
        return await fake.request(action, args)
    except Exception as exc:  # noqa: BLE001 - reported as a check failure
        problems.append(f"{action} failed: {exc}")
        return None


# --------------------------------------------------------------------------- the scenario


class Gameplay:
    """One player's whole visit: join, chat, an item, a teleport, leave."""

    def __init__(self, run: Any, fake: Any) -> None:
        self.run = run
        self.fake = fake
        self.pinned = pins()
        self.revision = str(run.target.record["revision"])
        self.proxied = needs_proxy(self.pinned, self.revision)
        # Sixteen characters at most, and new on every run so a stale save never matches.
        self.username = f"tmverify{run.nonce}"
        self.player: Player | None = None
        self.game_id: str | None = None
        self.spawn: dict[str, Any] | None = None
        self.join_mark = 0
        self.client_log = run.out / "gameplay-client.log"
        self.client_name = f"{run.container_name}-client"

    def client_identity(self) -> dict[str, Any]:
        identity: dict[str, Any] = {
            "library": f"mineflayer@{self.pinned['mineflayer']['version']}",
            "image": node_image(self.pinned),
            "loginVersion": client_version(self.pinned, self.revision),
            "username": self.username,
            "viaProxy": None,
        }
        if self.proxied:
            identity["viaProxy"] = {
                "version": self.pinned["viaproxy"]["version"],
                "sha256": self.pinned["viaproxy"]["sha256"],
                "targetVersion": self.revision,
            }
        return identity

    def _labels(self) -> list[str]:
        argv = ["--label", f"tm.run={self.run.options.run_id}", "--label", f"tm.ttl={int(time.time()) + 3 * 3600}"]
        for label in self.run.options.labels:
            argv += ["--label", label]
        return argv

    def start_proxy(self, jar: Path) -> Container:
        work = self.run.data_dir / ".viaproxy"
        work.mkdir(exist_ok=True)
        name = f"{self.run.container_name}-viaproxy"
        argv = [
            *docker_command(),
            "run",
            "-d",
            "--name",
            name,
            *self._labels(),
            "--network",
            f"container:{self.run.container_name}",
            "--memory",
            "768m",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "-e",
            "HOME=/work",
            "-v",
            f"{jar}:/viaproxy/ViaProxy.jar:ro",
            "-v",
            f"{work}:/work",
            "-w",
            "/work",
            "--entrypoint",
            "java",
            self.run.resolved["containerRef"],
            "-Xmx640m",
            "-jar",
            "/viaproxy/ViaProxy.jar",
            "cli",
            "--bind-address",
            f"127.0.0.1:{PROXY_PORT}",
            "--target-address",
            f"127.0.0.1:{GAME_PORT}",
            "--target-version",
            self.revision,
            "--auth-method",
            "NONE",
            "--chat-signing",
            "false",
        ]
        container = Container(
            name=name, argv=argv, log_file=self.run.out / "viaproxy.log", docker_log=self.run.docker_log
        )
        self.run.containers.append(container)
        self.run.extra_logs.append(container.log_file)
        container.start()
        return container

    async def start_player(self, kit: Path) -> Player:
        port = PROXY_PORT if self.proxied else GAME_PORT
        argv = [
            *docker_command(),
            "run",
            "-i",
            "--rm",
            "--name",
            self.client_name,
            *self._labels(),
            "--network",
            f"container:{self.run.container_name}",
            "--memory",
            "768m",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "-v",
            f"{kit / 'node_modules'}:/client/node_modules:ro",
            "-v",
            f"{CLIENT_DIR / 'client.mjs'}:/client/client.mjs:ro",
            "-v",
            f"{CLIENT_DIR / 'package.json'}:/client/package.json:ro",
            "-w",
            "/client",
            node_image(self.pinned),
            "node",
            "client.mjs",
            "127.0.0.1",
            str(port),
            self.username,
            client_version(self.pinned, self.revision),
        ]
        # Registered for the run's cleanup before it exists: `docker rm -f` of a name that is
        # already gone is harmless, a leaked player is not.
        self.run.containers.append(
            Container(name=self.client_name, argv=argv, log_file=self.client_log, docker_log=self.run.docker_log)
        )
        self.run.extra_logs.append(self.client_log)
        with self.run.docker_log.open("a", encoding="utf-8") as handle:
            handle.write(" ".join(argv) + "\n")
        process = await asyncio.create_subprocess_exec(
            *argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        player = Player(process=process, log_path=self.client_log)
        player.start_reading()
        return player

    # -- rows -----------------------------------------------------------------
    async def join(self) -> checks.CheckResult:
        with checks._Timer() as timer:
            problems: list[str] = []
            detail: dict[str, Any] = {"client": self.client_identity(), "manualCoverage": list(MANUAL_COVERAGE)}
            # Before the player exists, so its player-connected event cannot slip past the mark.
            mark = self.join_mark = len(self.fake.events)
            try:
                pinned = self.pinned
                kit = await asyncio.to_thread(install_client, pinned)
                if self.proxied:
                    jar = await asyncio.to_thread(viaproxy_jar, pinned)
                    proxy = await asyncio.to_thread(self.start_proxy, jar)
                    ready = await asyncio.to_thread(
                        checks.wait_for_line, proxy.log_file, PROXY_READY, PROXY_BUDGET, proxy.alive
                    )
                    if not ready:
                        problems.append(f"ViaProxy did not bind within {PROXY_BUDGET:.0f} s; see viaproxy.log")
                if not problems:
                    self.player = await self.start_player(kit)
            except Exception as exc:  # noqa: BLE001 - the player could not be fielded at all
                problems.append(f"the client could not be started: {exc}")
            if self.player is not None:
                spawn = await self.player.wait_for(
                    lambda event: event.get("event") in ("spawn", "kicked", "end"), SPAWN_BUDGET
                )
                if spawn is None or spawn.get("event") != "spawn":
                    problems.append(f"the client did not spawn within {SPAWN_BUDGET:.0f} s: {spawn}")
                else:
                    self.spawn = spawn
                    detail["spawn"] = {k: spawn.get(k) for k in ("uuid", "version", "protocol", "position")}
                    connected = await wait_game_event(
                        self.fake,
                        "player-connected",
                        lambda data: _player_of(data).get("name") == self.username,
                        EVENT_BUDGET,
                        mark,
                    )
                    if connected is None:
                        problems.append(f"no player-connected event named {self.username} within {EVENT_BUDGET:.0f} s")
                    else:
                        detail["playerConnected"] = connected
                        self.game_id = _player_of(connected).get("gameId") or None
                        if not self.game_id:
                            problems.append("the player-connected event carries no gameId")
                        elif self.game_id != spawn.get("uuid"):
                            problems.append(
                                f"the event's gameId {self.game_id} is not the client's uuid {spawn['uuid']}"
                            )
                    players = await _request(self.fake, "getPlayers", {}, problems)
                    detail["getPlayers"] = players
                    listed = [p for p in players or [] if isinstance(p, dict) and p.get("name") == self.username]
                    if not listed:
                        problems.append(f"getPlayers does not list {self.username}")
                    elif listed[0].get("gameId") != self.game_id:
                        problems.append("getPlayers lists the player under another gameId than its event")
        return _result("gameplay-join", timer, detail, problems)

    async def chat(self) -> checks.CheckResult:
        assert self.player is not None and self.game_id is not None
        with checks._Timer() as timer:
            problems: list[str] = []
            said = f"takaro-verify-chat-{self.run.nonce}"
            mark = len(self.fake.events)
            await self.player.send("chat", text=said)
            event = await wait_game_event(
                self.fake, "chat-message", lambda data: data.get("msg") == said, EVENT_BUDGET, mark
            )
            if event is None:
                problems.append(f"no chat-message event carried {said!r} within {EVENT_BUDGET:.0f} s")
            elif _player_of(event).get("gameId") != self.game_id or _player_of(event).get("name") != self.username:
                problems.append(f"the chat-message event names another player: {_player_of(event)}")
            reply = f"takaro-verify-reply-{self.run.nonce}"
            seen = len(self.player.events)
            await _request(
                self.fake, "sendMessage", {"message": reply, "opts": {"recipient": {"gameId": self.game_id}}}, problems
            )
            heard = await self.player.wait_for(
                lambda e: e.get("event") == "message" and reply in str(e.get("text")), EVENT_BUDGET, since=seen
            )
            if heard is None:
                problems.append(f"the client never received the sendMessage line {reply!r}")
        return _result(
            "gameplay-chat",
            timer,
            {"said": said, "chatMessage": event, "sent": reply, "received": heard},
            problems,
        )

    async def give_item(self) -> checks.CheckResult:
        assert self.player is not None and self.game_id is not None
        code, client_name, amount = GIVE_ITEM
        with checks._Timer() as timer:
            problems: list[str] = []
            before = _count(await self.player.ask("inventory", "inventory"), client_name)
            await _request(
                self.fake, "giveItem", {"player": {"gameId": self.game_id}, "item": code, "amount": amount}, problems
            )
            after = before
            deadline = time.monotonic() + EVENT_BUDGET
            while after - before < amount and time.monotonic() < deadline and not self.player.gone():
                await asyncio.sleep(1)
                after = _count(await self.player.ask("inventory", "inventory"), client_name)
            if after - before != amount:
                problems.append(f"the client holds {after - before} more {client_name}, expected {amount}")
        return _result(
            "gameplay-give-item",
            timer,
            {"item": code, "amount": amount, "clientCountBefore": before, "clientCountAfter": after},
            problems,
        )

    async def inventory(self) -> checks.CheckResult:
        assert self.game_id is not None
        code, _, amount = GIVE_ITEM
        with checks._Timer() as timer:
            problems: list[str] = []
            items = await _request(self.fake, "getPlayerInventory", {"gameId": self.game_id}, problems)
            rows = [item for item in items or [] if isinstance(item, dict) and item.get("code") == code]
            total = sum(_int(row.get("amount")) for row in rows)
            if total != amount:
                problems.append(f"getPlayerInventory reports {total} {code}, expected the {amount} given")
            for row in rows:
                if not row.get("name") or str(row.get("name")).startswith(checks.CATALOGUE_PREFIXES):
                    problems.append(f"{code} has no display name: {row.get('name')!r}")
        return _result("gameplay-inventory", timer, {"getPlayerInventory": items}, problems)

    async def teleport(self) -> checks.CheckResult:
        assert self.player is not None and self.game_id is not None and self.spawn is not None
        with checks._Timer() as timer:
            problems: list[str] = []
            origin = self.spawn["position"]
            target = {
                axis: math.floor(float(origin[axis])) + offset + (0.5 if axis != "y" else 0.0)
                for axis, offset in zip(("x", "y", "z"), TELEPORT_OFFSET, strict=True)
            }
            mark = len(self.player.events)
            await _request(self.fake, "teleportPlayer", {"player": {"gameId": self.game_id}, **target}, problems)
            moved = await self.player.wait_for(
                lambda e: e.get("event") == "forcedMove" and _near(e.get("position"), target, POSITION_TOLERANCE),
                EVENT_BUDGET,
                since=mark,
            )
            if moved is None:
                problems.append(f"the server never moved the client to {target}")
            location = await _request(self.fake, "getPlayerLocation", {"gameId": self.game_id}, problems)
            if not _near(location, target, LOCATION_TOLERANCE, axes=("x", "z")):
                problems.append(f"getPlayerLocation reports {location}, not the teleport target {target}")
            elif float(location.get("y", math.inf)) > target["y"] + POSITION_TOLERANCE:
                problems.append(f"getPlayerLocation reports y {location.get('y')} above the teleport target")
        return _result(
            "gameplay-teleport",
            timer,
            {"target": target, "clientForcedMove": moved, "getPlayerLocation": location},
            problems,
        )

    async def leave(self) -> checks.CheckResult:
        assert self.player is not None and self.game_id is not None
        with checks._Timer() as timer:
            problems: list[str] = []
            left_early = self.player.gone()
            # A player that was already dropped left before this row began.
            mark = self.join_mark if left_early else len(self.fake.events)
            if left_early:
                problems.append(self.player.why_gone())
            await self.player.send("quit")
            code = await self.player.close(QUIT_BUDGET)
            if code != 0 and not left_early:
                problems.append(f"the client exited {code}, expected 0 after a requested quit")
            event = await wait_game_event(
                self.fake,
                "player-disconnected",
                lambda data: _player_of(data).get("gameId") == self.game_id,
                EVENT_BUDGET,
                mark,
            )
            if event is None:
                problems.append(f"no player-disconnected event for {self.game_id} within {EVENT_BUDGET:.0f} s")
            players = await _request(self.fake, "getPlayers", {}, problems)
            if any(isinstance(p, dict) and p.get("gameId") == self.game_id for p in players or []):
                problems.append("getPlayers still lists the player after it left")
        return _result(
            "gameplay-leave",
            timer,
            {"clientExitCode": code, "playerDisconnected": event, "getPlayers": players},
            problems,
        )

    async def stop(self) -> None:
        if self.player is not None and self.player.process.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                self.player.process.kill()
            await self.player.close(5)


def _count(inventory: dict[str, Any] | None, name: str) -> int:
    if not inventory:
        return 0
    return sum(_int(item.get("count")) for item in inventory.get("items", []) if item.get("name") == name)


def _int(value: Any) -> int:
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _near(position: Any, target: dict[str, float], tolerance: float, axes: tuple[str, ...] = ("x", "y", "z")) -> bool:
    if not isinstance(position, dict):
        return False
    try:
        return all(abs(float(position[axis]) - target[axis]) <= tolerance for axis in axes)
    except (KeyError, TypeError, ValueError):
        return False


async def _guarded(check_id: str, row: Callable[[], Any]) -> checks.CheckResult:
    """A row that raised is a failed row with the reason, never a run without a report."""
    started = time.monotonic()
    try:
        result: checks.CheckResult = await row()
    except Exception as exc:  # noqa: BLE001 - anything a row did not expect is its failure
        elapsed = int((time.monotonic() - started) * 1000)
        return checks.CheckResult(check_id, "fail", elapsed, {"problems": [f"{type(exc).__name__}: {exc}"]})
    return result


async def run_gameplay(run: Any, fake: Any) -> None:
    """Every selected gameplay row, in order. A player that never joined skips the rest."""
    selected = [check_id for check_id in CHECK_IDS if run.wanted(check_id)]
    for check_id in CHECK_IDS:
        if check_id not in selected:
            run.skip(check_id, run.not_selected_reason(check_id))
    if not selected:
        return
    session = Gameplay(run, fake)
    try:
        # Joining is the precondition of every other row, so it runs whenever any of them does.
        joined = await _guarded("gameplay-join", session.join)
        if "gameplay-join" in selected or joined.status == "fail":
            run.record(joined)
        rows: tuple[tuple[str, Callable[[], Any]], ...] = (
            ("gameplay-chat", session.chat),
            ("gameplay-give-item", session.give_item),
            ("gameplay-inventory", session.inventory),
            ("gameplay-teleport", session.teleport),
            ("gameplay-leave", session.leave),
        )
        for check_id, row in rows:
            if check_id not in selected:
                continue
            if joined.status != "pass":
                run.skip(check_id, "gameplay-join failed; no player on the server")
            elif check_id == "gameplay-inventory" and not run.wanted("gameplay-give-item"):
                run.skip(check_id, "needs gameplay-give-item, which was not selected")
            elif session.player is not None and session.player.gone() and check_id != "gameplay-leave":
                run.record(checks.CheckResult(check_id, "fail", 0, {"problems": [session.player.why_gone()]}))
            else:
                run.record(await _guarded(check_id, row))
    finally:
        await session.stop()
