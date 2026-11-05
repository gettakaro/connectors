"""A docker CLI and a Minecraft server, faked well enough to drive the real `verify`.

The stub docker keeps one state directory per container name, so a run that boots the same
data dir twice (restart) or boots a deliberately wrong jar (negative) is observable. The stub
server derives its identity from the container environment and its target-check line from the
jar that was actually deployed, so a mismatched jar is refused the way Fabric Loader refuses it.
"""

from __future__ import annotations

import contextlib
import json
import os
import shlex
import signal
import sys
from pathlib import Path
from typing import Any

import pytest

from conftest import make_jar, sha256

DOCKER_STUB = r"""
import json, os, shutil, signal, subprocess, sys, time

state = os.environ["STUB_STATE"]
argv = sys.argv[1:]
with open(state + "/argv.jsonl", "a") as handle:
    handle.write(json.dumps(argv) + "\n")

def env_of(args):
    values = {}
    for index, item in enumerate(args):
        if item == "-e" and index + 1 < len(args):
            key, _, value = args[index + 1].partition("=")
            values[key] = value
    return values

def name_of(args):
    return args[args.index("--name") + 1] if "--name" in args else args[-1]

def data_of(args):
    for index, item in enumerate(args):
        if item == "-v" and index + 1 < len(args) and args[index + 1].endswith(":/data"):
            return args[index + 1].rsplit(":", 1)[0]
    return ""

if argv[:1] == ["network"]:
    print("127.0.0.1")
    sys.exit(0)

if argv[:1] == ["run"]:
    name = name_of(argv)
    values = env_of(argv)
    box = os.path.join(state, name)
    os.makedirs(box, exist_ok=True)
    # A name may be reused by a later test run; a stale marker would report a dead container.
    if os.path.exists(box + "/exited"):
        os.remove(box + "/exited")
    values["STUB_DATA"] = data_of(argv)
    values["STUB_NAME"] = name
    with open(box + "/env.json", "w") as handle:
        json.dump(values, handle)
    child = dict(os.environ)
    child.update(values)
    child["STUB_BOX"] = box
    process = subprocess.Popen([sys.executable, os.environ["STUB_SERVER"]], env=child,
                               stdout=open(box + "/server.out", "w"), stderr=subprocess.STDOUT)
    # `rm` needs this to stop the process the way a real `docker rm` stops a container.
    with open(box + "/pid", "w") as handle:
        handle.write(str(process.pid))
    print("stub-container-" + name)
    sys.exit(0)

if argv[:1] == ["logs"]:
    box = os.path.join(state, argv[-1])
    path = box + "/server.out"
    while not os.path.exists(path):
        time.sleep(0.05)
    with open(path) as handle:
        while True:
            line = handle.readline()
            if line:
                sys.stdout.write(line)
                sys.stdout.flush()
            else:
                if os.path.exists(box + "/exited"):
                    sys.stdout.write(handle.read())
                    break
                time.sleep(0.1)
    sys.exit(0)

if argv[:1] == ["inspect"]:
    fmt = argv[argv.index("-f") + 1]
    box = os.path.join(state, argv[-1])
    marker = box + "/exited"
    code = open(marker).read().strip() or "0" if os.path.exists(marker) else None
    if "PortBindings" in fmt:
        print("{}")
    elif "Running" in fmt and "ExitCode" in fmt:
        print("false|" + code if code is not None else "true|0")
    elif "Running" in fmt:
        print("false" if code is not None else "true")
    sys.exit(0)

if argv[:1] == ["ps"]:
    sys.exit(0)

if argv[:1] == ["rm"]:
    for name in [item for item in argv[1:] if not item.startswith("-")]:
        box = os.path.join(state, name)
        # An orphan sweep names containers this stub never started; there is nothing to stop.
        if not os.path.exists(box + "/pid"):
            continue
        with open(box + "/pid") as handle:
            pid = int(handle.read().strip())
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pid = 0
        deadline = time.time() + 5
        while pid and time.time() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                pid = 0
                break
            time.sleep(0.05)
        if pid:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        # A `logs -f` follower waits for this marker; without it, it never drains and ends.
        if not os.path.exists(box + "/exited"):
            with open(box + "/exited", "w") as handle:
                handle.write("137")
    with open(state + "/removed", "a") as handle:
        handle.write(" ".join(argv) + "\n")
    sys.exit(0)

sys.exit(0)
"""

STUB_SERVER = r"""
import asyncio, glob, json, os, sys, zipfile
import websockets

BOX = os.environ["STUB_BOX"]
DATA = os.environ.get("STUB_DATA", "")
NAME = os.environ.get("STUB_NAME", "")
FAIL = os.environ.get("STUB_FAIL", "")
VERSION = os.environ.get("VERSION", "26.2")
LOADER = os.environ.get("FABRIC_LOADER_VERSION", "0.19.5")

ITEMS = [{"code": "minecraft:diamond_sword", "name": "Diamond Sword", "description": ""},
         {"code": "minecraft:stone", "name": "Stone", "description": ""}]
ENTITIES = [{"code": "minecraft:zombie", "name": "Zombie", "description": "", "type": "hostile"},
            {"code": "minecraft:cow", "name": "Cow", "description": "", "type": "friendly"}]

def log(line):
    print(line, flush=True)

def die(code):
    with open(BOX + "/exited", "w") as handle:
        handle.write(str(code))
    sys.exit(0)

def deployed_stamp():
    '''The Takaro jar the deploy (or the negative check) actually put in mods/.'''
    listing = sorted(glob.glob(os.path.join(DATA, "mods", "*.jar")))
    with open(BOX + "/mods.txt", "w") as handle:
        handle.write("\n".join(os.path.basename(path) for path in listing) + "\n")
    for path in listing:
        try:
            with zipfile.ZipFile(path) as archive:
                return json.loads(archive.read("META-INF/takaro-target.json"))
        except (KeyError, OSError, zipfile.BadZipFile):
            continue
    return None

async def session(url):
    '''One connect-identify-serve cycle. Returns the close code, or None when asked to stop.'''
    socket = await websockets.connect(url)
    try:
        log("[Server thread/INFO]: WebSocket connected, sending identify...")
        await socket.send(json.dumps({"type": "identify",
                                      "payload": {
                                          "identityToken": os.environ.get("TAKARO_IDENTITY_TOKEN", "x"),
                                          "registrationToken": os.environ.get("TAKARO_REGISTRATION_TOKEN", "y")}}))
        async for raw in socket:
            frame = json.loads(raw)
            if frame.get("type") == "identifyResponse":
                log("[Server thread/INFO]: Identified successfully")
                continue
            if frame.get("type") != "request":
                continue
            action = frame["payload"]["action"]
            args = json.loads(frame["payload"]["args"] or "{}")
            if action == "testReachability":
                payload = {"connectable": True}
            elif action == "getPlayers":
                payload = []
            elif action == "listItems":
                payload = ITEMS
            elif action == "listEntities":
                payload = ENTITIES
            elif action == "executeConsoleCommand":
                log("[Server thread/INFO]: [Server] " + args["command"].split(" ", 1)[1])
                payload = {"rawResult": "", "success": True}
            else:
                payload = None
            await socket.send(json.dumps({"type": "response", "requestId": frame["requestId"],
                                          "payload": payload}))
            if action == "shutdown":
                return None
    except websockets.ConnectionClosed:
        pass
    finally:
        await socket.close()
    return socket.close_code

async def main():
    # The runner hands the container host.docker.internal; on this side of the stub that
    # is the loopback address the fake Takaro is bound to.
    url = os.environ["TAKARO_WS_URL"].replace("host.docker.internal", "127.0.0.1")
    log("[Server thread/INFO]: Loading Minecraft %s with Fabric Loader %s" % (VERSION, LOADER))
    stamp = deployed_stamp()
    revision = (stamp or {}).get("revision", VERSION)
    if revision != VERSION and FAIL != "negative":
        # What Fabric Loader really writes when fabric.mod.json pins another game version.
        log("[main/ERROR]: Mod resolution failed")
        log("[main/ERROR]: Incompatible mods found!")
        log("[main/ERROR]: Mod 'Takaro Minecraft Connector' (takaro) %s requires version %s of "
            "'Minecraft' (minecraft), but only the wrong version is present: %s!"
            % ((stamp or {}).get("connectorVersion", "0.1.1"), revision, VERSION))
        log('mc-server-runner Minecraft server failed {"exitCode": 1}')
        die(1)
    if FAIL == "hang":
        while True:
            await asyncio.sleep(1)
    log('[Server thread/INFO]: Done (12.345s)! For help, type "help"')
    check = {"target": (stamp or {}).get("target", "unknown"),
             "fingerprint": (stamp or {}).get("fingerprint", ""),
             "connectorVersion": (stamp or {}).get("connectorVersion", "0.1.1"),
             "runtime": {"gameVersion": "0.0.0" if FAIL == "connector-load" else VERSION,
                         "loader": "fabric", "loaderVersion": LOADER,
                         "java": int(os.environ.get("TAKARO_JAVA", "25"))},
             "policy": "enforce", "result": "ok", "reasons": []}
    log("[Server thread/INFO]: Takaro target-check: " + json.dumps(check))
    if FAIL == "restart" and NAME.endswith("-restart"):
        # Booted, but the connector never comes up: the restart check must not hang on it.
        await asyncio.sleep(1)
        die(1)
    while True:
        code = await session(url)
        if code is None:
            die(0)
        log("[Server thread/INFO]: WebSocket closed (code=%s, reason=going away, remote=true)" % code)
        if FAIL == "reconnect":
            die(1)
        log("[Server thread/INFO]: Reconnecting in 0s...")
        await asyncio.sleep(0.5)

asyncio.run(main())
"""


def process_is_gone(pid: int) -> bool:
    """True when ``pid`` no longer runs, a reaped zombie counting as gone."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    except PermissionError:
        return False
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[-1].split()
    except OSError:
        return True
    return fields[0] == "Z"


def install_docker_stub(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, request: pytest.FixtureRequest) -> Path:
    """Point ``TAKARO_MAINT_DOCKER`` at the stub and return its state directory.

    Each test module declares its own ``docker_stub`` fixture around this, so the stub is
    shared without a name imported into a module that also takes it as a test argument.
    """
    state = tmp_path / "stub"
    state.mkdir()
    stub = tmp_path / "docker_stub.py"
    stub.write_text(DOCKER_STUB)
    server = tmp_path / "stub_server.py"
    server.write_text(STUB_SERVER)
    monkeypatch.setenv("STUB_STATE", str(state))
    monkeypatch.setenv("STUB_SERVER", str(server))
    monkeypatch.setenv("STUB_RUN_ID", "t1")
    monkeypatch.setenv("TAKARO_MAINT_DOCKER", f"{shlex.quote(sys.executable)} {shlex.quote(str(stub))}")

    def _kill_survivors() -> None:
        # The safety net for a test that dies before the runner's `finally` runs its `rm`.
        for marker in state.glob("*/pid"):
            with contextlib.suppress(OSError, ValueError):
                os.kill(int(marker.read_text().strip()), signal.SIGKILL)

    request.addfinalizer(_kill_survivors)
    return state


def artifacts_for(run: Any, wired: Any, tmp_path: Path, targets: tuple[str, ...] = ("fabric-26.2",)) -> Path:
    """A dist directory holding one built jar per target, with the manifest that names them."""
    out = tmp_path / "dist"
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for target_id in targets:
        _, resolved, _ = run("targets", "resolve", "--game", "minecraft", "--target", target_id, repo=wired.root)
        name = f"takaro-minecraft-mod-{target_id}-0.1.1.jar"
        jar = make_jar(
            out / name,
            target=target_id,
            fingerprint=resolved["fingerprint"],
            revision=resolved["revision"],
        )
        rows.append(
            {
                "role": "server-mod",
                "target": target_id,
                "fingerprint": resolved["fingerprint"],
                "file": name,
                "sha256": sha256(jar.read_bytes()),
                "size": jar.stat().st_size,
            }
        )
    (out / "build-manifest.json").write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "connector": "minecraft",
                "version": "0.1.1",
                "sourceRevision": "deadbeef",
                "dirty": False,
                "builtAt": "2026-09-17T00:00:00Z",
                "toolchain": {
                    "image": "eclipse-temurin",
                    "tag": "25-jdk",
                    "digest": "sha256:" + "0" * 64,
                    "mode": "container",
                },
                "artifacts": rows,
            }
        )
    )
    return out
