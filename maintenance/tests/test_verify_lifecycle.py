"""`verify`'s lifecycle checks: reconnect, restart, the refused sibling jar, and hosted mode.

Every test drives the real command through the docker stub and a fake Takaro, so what is
asserted is the report the command writes and the containers it really asked for.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any

import pytest

from conftest import REPO_ROOT
from fake_docker import artifacts_for, install_docker_stub, process_is_gone

UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", re.IGNORECASE)
RETAINED = ("report.json", "docker.log", "fake-takaro.log", "server.log", "install.json", "deploy.json")


@pytest.fixture
def docker_stub(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, request: pytest.FixtureRequest) -> Path:
    return install_docker_stub(tmp_path, monkeypatch, request)


def verify(run: Any, wired: Any, artifacts: Path, out: Path, *extra: str, target: str = "fabric-26.2") -> Any:
    return run(
        "verify",
        "--game",
        "minecraft",
        "--target",
        target,
        "--artifacts",
        str(artifacts),
        "--out",
        str(out),
        "--run-id",
        "t1",
        "--startup-timeout",
        "60",
        *extra,
        repo=wired.root,
    )


def report_of(out: Path, target: str = "fabric-26.2") -> dict[str, Any]:
    return json.loads((out / target / "report.json").read_text())


def row(report: dict[str, Any], check_id: str) -> dict[str, Any]:
    return next(check for check in report["checks"] if check["id"] == check_id)


def docker_runs(state: Path) -> list[list[str]]:
    calls = [json.loads(line) for line in (state / "argv.jsonl").read_text().splitlines()]
    return [call for call in calls if call[:1] == ["run"]]


def named(call: list[str]) -> str:
    return call[call.index("--name") + 1]


def mounted(call: list[str]) -> str:
    return next(call[i + 1] for i, item in enumerate(call) if item == "-v")


# --------------------------------------------------------------------------- reconnect


def test_reconnect_after_a_server_side_close_reidentifies_within_the_budget(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out)

    assert code == 0, payload
    report = report_of(out)
    reconnect = row(report, "reconnect")
    assert reconnect["status"] == "pass", reconnect
    assert reconnect["detail"]["closeCode"] == 1001
    assert reconnect["detail"]["identifyCountAfter"] == reconnect["detail"]["identifyCountBefore"] + 1
    assert reconnect["detail"]["testReachability"]["connectable"] is True

    frames = [json.loads(line) for line in (out / "fabric-26.2" / "fake-takaro.log").read_text().splitlines()]
    identifies = [f for f in frames if f["direction"] == "in" and f["frame"]["type"] == "identify"]
    assert len(identifies) >= 2


def test_a_connector_that_never_reconnects_fails_the_reconnect_check(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("STUB_FAIL", "reconnect")
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out)

    assert code == 8, payload
    report = report_of(out)
    reconnect = row(report, "reconnect")
    assert reconnect["status"] == "fail"
    assert any("20 s" in problem for problem in reconnect["detail"]["problems"])
    for check_id in ("build", "startup", "connector-load", "identify", "heartbeat", "players", "console"):
        assert row(report, check_id)["status"] == "pass", check_id
    assert report["outcome"] == "fail"
    # Lifecycle rows gate the outcome, never the level: this run died at the shutdown check.
    assert report["level"] == "startup"


# --------------------------------------------------------------------------- restart


def test_restart_boots_the_same_data_dir_and_leaves_the_ledger_unchanged(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out)

    assert code == 0, payload
    report = report_of(out)
    restart = row(report, "restart")
    assert restart["status"] == "pass", restart
    assert restart["detail"]["ledgerUnchanged"] is True
    assert restart["detail"]["startup"]["inputsIntact"] is True
    assert restart["detail"]["exitCode"] == 0

    calls = docker_runs(docker_stub)
    assert [named(call) for call in calls] == [
        "takaro-verify-minecraft-fabric-26.2-t1",
        "takaro-verify-minecraft-fabric-26.2-t1-restart",
    ]
    assert mounted(calls[0]) == mounted(calls[1])
    removed = (docker_stub / "removed").read_text()
    assert "takaro-verify-minecraft-fabric-26.2-t1-restart" in removed
    assert "takaro-verify-minecraft-fabric-26.2-t1\n" in removed
    assert "server-restart.log" in [log["name"] for log in report["logs"]]


def test_a_restart_that_never_identifies_fails(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("STUB_FAIL", "restart")
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out)

    assert code == 8, payload
    restart = row(report_of(out), "restart")
    assert restart["status"] == "fail"
    assert any("identif" in problem for problem in restart["detail"]["problems"])
    removed = (docker_stub / "removed").read_text()
    assert "takaro-verify-minecraft-fabric-26.2-t1-restart" in removed
    assert "takaro-verify-minecraft-fabric-26.2-t1\n" in removed


# --------------------------------------------------------------------------- negative


def test_negative_requires_the_sibling_artifact_to_be_refused(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path, targets=("fabric-26.2", "fabric-26.1.2"))
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out, "--negative")

    assert code == 0, payload
    report = report_of(out)
    negative = row(report, "negative-wrong-target")
    assert negative["status"] == "pass", negative
    assert negative["detail"]["siblingTarget"] == "fabric-26.1.2"
    assert negative["detail"]["refusedBy"] == "loader"
    assert negative["detail"]["identifyFramesDuringNegative"] == 0

    calls = docker_runs(docker_stub)
    assert named(calls[2]) == "takaro-verify-minecraft-fabric-26.2-t1-negative"
    mods = (docker_stub / named(calls[2]) / "mods.txt").read_text()
    assert "takaro-minecraft-mod-fabric-26.1.2-0.1.1.jar" in mods
    assert "takaro-minecraft-mod-fabric-26.2-0.1.1.jar" not in mods
    assert "server-negative.log" in [log["name"] for log in report["logs"]]


def test_negative_fails_when_the_wrong_artifact_identifies(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("STUB_FAIL", "negative")
    artifacts = artifacts_for(run, wired, tmp_path, targets=("fabric-26.2", "fabric-26.1.2"))
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out, "--negative")

    assert code == 8, payload
    negative = row(report_of(out), "negative-wrong-target")
    assert negative["status"] == "fail"
    assert negative["detail"]["identifyFramesDuringNegative"] == 1


def test_negative_is_skipped_without_a_sibling_artifact(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out, "--negative")

    assert code == 0, payload
    negative = row(report_of(out), "negative-wrong-target")
    assert negative["status"] == "skip"
    assert "fabric-26.1.2" in negative["detail"]["reason"]


def test_paper_and_neoforge_have_no_sibling_target() -> None:
    from takaro_maint import catalog as catalog_pkg
    from takaro_maint import paths
    from takaro_maint.games.minecraft.verify import sibling_target

    paths.set_repo_root(REPO_ROOT)
    catalog = catalog_pkg.load()
    by_id = {target.id: target for target in catalog.game("minecraft").targets}

    assert sibling_target(catalog, by_id["fabric-26.2"]).id == "fabric-26.1.2"
    assert sibling_target(catalog, by_id["fabric-26.1.2"]).id == "fabric-26.2"
    assert sibling_target(catalog, by_id["paper-1.21.11"]) is None
    assert sibling_target(catalog, by_id["neoforge-1.21.11"]) is None


# --------------------------------------------------------------------------- isolation


def test_sigint_removes_every_container_and_the_data_dir_and_exits_130(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("STUB_FAIL", "hang")
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"
    name = "takaro-verify-minecraft-fabric-26.2-t1"

    process = subprocess.Popen(
        [
            sys.executable,
            "-m",
            "takaro_maint",
            "--repo-root",
            str(wired.root),
            "verify",
            "--game",
            "minecraft",
            "--target",
            "fabric-26.2",
            "--artifacts",
            str(artifacts),
            "--out",
            str(out),
            "--run-id",
            "t1",
            "--startup-timeout",
            "120",
        ],
        env={**os.environ, "PYTHONPATH": str(REPO_ROOT / "maintenance/src")},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    deadline = time.monotonic() + 120
    while not (docker_stub / name / "env.json").is_file():
        assert time.monotonic() < deadline and process.poll() is None, "the stub container never started"
        time.sleep(0.2)
    data_dir = Path(json.loads((docker_stub / name / "env.json").read_text())["STUB_DATA"])
    assert data_dir.is_dir()

    process.send_signal(signal.SIGINT)
    process.communicate(timeout=120)

    assert process.returncode == 130
    assert name in (docker_stub / "removed").read_text()
    # The whole point of the interrupt path: the hung server is dead, not merely disowned.
    assert process_is_gone(int((docker_stub / name / "pid").read_text().strip()))
    assert not data_dir.exists()
    assert (out / "fabric-26.2" / "docker.log").is_file()


def test_retained_files_never_contain_the_registration_token(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out)

    assert code == 0, payload
    box = docker_stub / "takaro-verify-minecraft-fabric-26.2-t1"
    token = json.loads((box / "env.json").read_text())["TAKARO_REGISTRATION_TOKEN"]
    assert len(token) > 6
    for name in RETAINED:
        text = (out / "fabric-26.2" / name).read_text()
        assert token not in text, name
    assert "TAKARO_REGISTRATION_TOKEN=<redacted>" in (out / "fabric-26.2" / "docker.log").read_text()


def test_the_full_report_validates_against_the_schema_with_lifecycle_rows(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, repo_root: Path
) -> None:
    from jsonschema import Draft202012Validator

    artifacts = artifacts_for(run, wired, tmp_path, targets=("fabric-26.2", "fabric-26.1.2"))
    out = tmp_path / "reports"

    code, payload, _ = verify(run, wired, artifacts, out, "--negative")

    assert code == 0, payload
    report = report_of(out)
    schema = json.loads((repo_root / "catalog/schema/v1/verify-report.schema.json").read_text())
    assert list(Draft202012Validator(schema).iter_errors(report)) == []
    assert [check["id"] for check in report["checks"]] == [
        "build",
        "startup",
        "connector-load",
        "identify",
        "heartbeat",
        "players",
        "catalog-items",
        "catalog-entities",
        "console",
        "gameplay-join",
        "gameplay-chat",
        "gameplay-give-item",
        "gameplay-inventory",
        "gameplay-teleport",
        "gameplay-leave",
        "reconnect",
        "shutdown",
        "restart",
        "negative-wrong-target",
    ]
    assert report["takaro"] == "local"
    assert report["level"] == "gameplay"
    assert report["coverage"]["gameplay"].startswith("covered - join, chat")


def test_runtime_identity_is_scanned_for_every_platform_banner(tmp_path: Path) -> None:
    from takaro_maint.games import adapter_for
    from takaro_maint.games.minecraft.verify import scan_runtime_identity

    adapter = adapter_for("minecraft")
    banners = {
        "fabric": (
            "[16:20:01] [main/INFO]: Loading Minecraft 26.1.2 with Fabric Loader 0.19.3",
            {"gameVersion": "26.1.2", "loader": "fabric", "loaderVersion": "0.19.3"},
        ),
        "paper": (
            "[21:04:10 INFO]: This server is running Paper version 1.21.11-132-main@abcdef "
            "(2026-09-10T00:00:00Z) (Implementing API version 1.21.11-R0.1-SNAPSHOT)",
            {"gameVersion": "1.21.11", "loader": "paper", "loaderVersion": "132"},
        ),
        "neoforge": (
            "[19:42:55] [main/INFO]: NeoForge mod loading, version 21.11.45, for MC 1.21.11",
            {"gameVersion": "1.21.11", "loader": "neoforge", "loaderVersion": "21.11.45"},
        ),
    }
    for platform, (banner, expected) in banners.items():
        log = tmp_path / f"{platform}.log"
        log.write_text("[Server thread/INFO]: Starting minecraft server\n" + banner + "\n")
        assert scan_runtime_identity(adapter, log) == expected, platform

    empty = tmp_path / "empty.log"
    empty.write_text("[Server thread/INFO]: nothing identifying here\n")
    assert scan_runtime_identity(adapter, empty) == {}


# --------------------------------------------------------------------------- hosted mode

HOSTED_KEYS = (
    "TAKARO_WS_URL",
    "TAKARO_REGISTRATION_TOKEN",
    "TAKARO_HOST",
    "TAKARO_USERNAME",
    "TAKARO_PASSWORD",
    "TAKARO_DOMAIN_ID",
)
HOSTED_RETAINED = ("report.json", "docker.log", "server.log", "install.json", "deploy.json")
IDENTITY_PREFIX = "takaro-maint-minecraft-fabric-26.2-"


class HostedRest:
    """Takaro's REST API, faked down to the six calls a hosted verification makes.

    A gameserver appears under the identity only once the connector has really identified
    on the websocket side, so `hosted-registration` proves self-registration rather than
    a row this fixture pre-created.
    """

    def __init__(self, ws: Any, loop: Any, seeded: dict[str, str] | None = None, broken: str = "") -> None:
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

        self.ws = ws
        self.loop = loop
        self.servers: dict[str, str] = dict(seeded or {})
        self.calls: list[str] = []
        self.methods: list[str] = []
        self.domains: list[str] = []
        self.deleted_at: list[float] = []
        self.broken = broken
        fake = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_: Any) -> None:
                pass

            def _body(self) -> dict[str, Any]:
                length = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(length) if length else b""
                try:
                    return dict(json.loads(raw or b"{}"))
                except json.JSONDecodeError:
                    return {}

            def _reply(self, payload: dict[str, Any], cookie: bool = False) -> None:
                body = json.dumps(payload).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                if cookie:
                    self.send_header("Set-Cookie", "takaro-session=fake; Path=/")
                self.end_headers()
                self.wfile.write(body)

            def _route(self, method: str) -> None:
                fake.domains.append(self.headers.get("X-Takaro-Domain") or "")
                fake.methods.append(f"{method} {self.path}")
                if fake.broken and self.path.endswith(fake.broken):
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                parts = [part for part in self.path.split("/") if part]
                body = self._body()
                if method == "POST" and parts == ["login"]:
                    fake.calls.append("login")
                    self._reply({"data": {"token": "fake-session-token"}}, cookie=True)
                elif method == "POST" and parts == ["gameserver", "search"]:
                    fake.calls.append("search")
                    self._reply({"data": fake.search(body)})
                elif method == "GET" and parts[:1] == ["gameserver"] and parts[-1:] == ["reachability"]:
                    fake.calls.append("reachability")
                    self._reply({"data": {"connectable": True}})
                elif method == "GET" and parts[:1] == ["gameserver"] and parts[-1:] == ["players"]:
                    fake.calls.append("players")
                    self._reply({"data": []})
                elif method == "POST" and parts[:1] == ["gameserver"] and parts[-1:] == ["shutdown"]:
                    fake.calls.append("shutdown")
                    fake.ask_the_connector_to_stop()
                    self._reply({"data": {}})
                elif method == "DELETE" and len(parts) == 2 and parts[0] == "gameserver":
                    fake.calls.append("delete")
                    fake.servers.pop(parts[1], None)
                    fake.deleted_at.append(time.time())
                    self._reply({"data": {}})
                else:
                    self._reply({"data": None})

            def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler's spelling
                self._route("POST")

            def do_GET(self) -> None:  # noqa: N802
                self._route("GET")

            def do_DELETE(self) -> None:  # noqa: N802
                self._route("DELETE")

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def search(self, body: dict[str, Any]) -> list[dict[str, str]]:
        """Exact-match on `filters.identityToken`, or the whole listing when there is none."""
        wanted = (body.get("filters") or {}).get("identityToken")
        identified = (self.ws.identified or {}).get("identityToken")
        if identified is not None and identified not in self.servers.values():
            self.servers[str(uuid.uuid4())] = identified
        rows = [{"id": sid, "identityToken": token} for sid, token in self.servers.items()]
        return [row for row in rows if row["identityToken"] in wanted] if wanted else rows

    def ask_the_connector_to_stop(self) -> None:
        future = asyncio.run_coroutine_threadsafe(self.ws.request("shutdown", {}, timeout=30), self.loop)
        with contextlib.suppress(Exception):
            future.result(timeout=35)

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=10)


@contextlib.contextmanager
def hosted_takaro(
    monkeypatch: pytest.MonkeyPatch,
    log_path: Path,
    seeded: dict[str, str] | None = None,
    broken: str = "",
) -> Any:
    """The real websocket fake and the REST fake, wired into the six hosted variables."""
    from takaro_maint.verify.fake_takaro import FakeTakaro

    loop = asyncio.new_event_loop()
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()
    ws = FakeTakaro(host="127.0.0.1", log_path=log_path)
    port = asyncio.run_coroutine_threadsafe(ws.start(), loop).result(timeout=20)
    rest = HostedRest(ws, loop, seeded, broken)
    monkeypatch.setenv("TAKARO_WS_URL", f"ws://127.0.0.1:{port}/")
    monkeypatch.setenv("TAKARO_REGISTRATION_TOKEN", "hosted-registration-token-value")
    monkeypatch.setenv("TAKARO_HOST", rest.url)
    monkeypatch.setenv("TAKARO_USERNAME", "verify-robot@example.invalid")
    monkeypatch.setenv("TAKARO_PASSWORD", "hosted-password-value")
    monkeypatch.setenv("TAKARO_DOMAIN_ID", str(uuid.uuid4()))
    try:
        yield rest
    finally:
        rest.close()
        asyncio.run_coroutine_threadsafe(ws.stop(), loop).result(timeout=20)
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=10)
        loop.close()


def test_hosted_mode_needs_its_environment(
    run: Any, wired: Any, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    for key in HOSTED_KEYS:
        monkeypatch.delenv(key, raising=False)
    artifacts = artifacts_for(run, wired, tmp_path)

    code, _, stderr = verify(run, wired, artifacts, tmp_path / "reports", "--takaro", "hosted")

    assert code == 2
    for key in HOSTED_KEYS:
        assert key in stderr, key

    monkeypatch.setenv("TAKARO_PASSWORD", "hunter2-decoy")
    code, payload, stderr = verify(run, wired, artifacts, tmp_path / "reports", "--takaro", "hosted")

    assert code == 2
    assert "TAKARO_PASSWORD" not in stderr
    assert "hunter2-decoy" not in stderr and "hunter2-decoy" not in str(payload)


def test_hosted_run_registers_reaches_lists_shuts_down_and_deletes(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    with hosted_takaro(monkeypatch, tmp_path / "ws.log") as rest:
        code, payload, stderr = verify(run, wired, artifacts, out, "--takaro", "hosted")

    assert code == 0, (payload, stderr)
    report = report_of(out)
    assert report["takaro"] == "hosted"
    assert report["level"] == "startup"
    assert report["outcome"] == "pass"
    for check_id in (
        "build",
        "startup",
        "connector-load",
        "identify",
        "hosted-registration",
        "heartbeat",
        "players",
        "shutdown",
    ):
        assert row(report, check_id)["status"] == "pass", (check_id, row(report, check_id))
    for check_id in ("catalog-items", "catalog-entities", "console", "reconnect", "negative-wrong-target"):
        assert row(report, check_id)["status"] == "skip"
        assert "local fake" in row(report, check_id)["detail"]["reason"]
    assert row(report, "restart")["status"] == "skip"
    assert row(report, "heartbeat")["detail"]["connectable"] is True
    assert row(report, "players")["detail"]["players"] == 0
    assert row(report, "shutdown")["detail"]["exitCode"] == 0

    assert rest.calls[0] == "login"
    assert rest.calls[1] == "search", "the sweep for leftovers comes before anything else"
    assert [call for call in rest.calls if call in ("reachability", "players", "shutdown", "delete")] == [
        "reachability",
        "players",
        "shutdown",
        "delete",
    ]
    assert all(domain for domain in rest.domains), rest.domains
    assert any(entry.startswith("GET ") and entry.endswith("/reachability") for entry in rest.methods), rest.methods
    assert rest.servers == {}, "the hosted run must delete every gameserver it registered"


def test_hosted_retained_files_carry_no_ids_hosts_or_tokens(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    with hosted_takaro(monkeypatch, tmp_path / "ws.log"):
        code, payload, _ = verify(run, wired, artifacts, out, "--takaro", "hosted")
        secrets = [os.environ[key] for key in HOSTED_KEYS]
        ws_host = "127.0.0.1"

    assert code == 0, payload
    for name in HOSTED_RETAINED:
        text = (out / "fabric-26.2" / name).read_text()
        assert not UUID.search(text), f"{name} kept a UUID"
        assert ws_host not in text, f"{name} kept the Takaro host"
        for value in secrets:
            assert value not in text, f"{name} kept {value[:4]}..."
    registration = row(report_of(out), "hosted-registration")
    assert registration["detail"]["gameServerId"] == "<redacted>"
    assert registration["detail"]["identity"].startswith(IDENTITY_PREFIX)


def test_a_hosted_run_that_dies_still_redacts_the_files_it_keeps(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Nothing about the redaction is conditional on the run reaching its report.

    A boot failure, a docker error or an interrupt leaves the same server.log behind, and
    that is the file that gets copied into evidence.
    """
    from takaro_maint.games.minecraft import verify as hooks

    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"
    gameserver_id = str(uuid.uuid4())

    with hosted_takaro(monkeypatch, tmp_path / "ws.log"):
        host = os.environ["TAKARO_HOST"]
        token = os.environ["TAKARO_REGISTRATION_TOKEN"]

        async def die_after_logging(target_run: Any, _alive: Any) -> Any:
            # What the log holds by the time a hosted run can still fall over.
            with target_run.server_log.open("a", encoding="utf-8") as handle:
                handle.write(f"[Server thread/INFO]: {host} accepted {gameserver_id} with {token}\n")
            raise RuntimeError("docker went away mid-run")

        monkeypatch.setattr(hooks, "_hosted_identify", die_after_logging)
        code, payload, _ = verify(run, wired, artifacts, out, "--takaro", "hosted")

    assert code == 1, payload
    assert not (out / "fabric-26.2" / "report.json").exists(), "a run that died writes no report"
    kept = (out / "fabric-26.2" / "server.log").read_text()
    assert gameserver_id not in kept and "<uuid>" in kept
    assert host not in kept and token not in kept
    assert kept.count("<redacted>") >= 2, kept


def test_a_stale_hosted_registration_is_deleted_before_the_boot(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"
    stale = str(uuid.uuid4())

    with hosted_takaro(monkeypatch, tmp_path / "ws.log", seeded={stale: IDENTITY_PREFIX + "p0-old"}) as rest:
        code, payload, _ = verify(run, wired, artifacts, out, "--takaro", "hosted")

    assert code == 0, payload
    assert rest.calls[:3] == ["login", "search", "delete"]
    booted_at = (docker_stub / "takaro-verify-minecraft-fabric-26.2-t1" / "env.json").stat().st_mtime
    assert rest.deleted_at[0] <= booted_at, "the stale gameserver must be gone before the container starts"
    assert stale not in rest.servers
    registered = row(report_of(out), "hosted-registration")["detail"]["identity"]
    assert registered.startswith(IDENTITY_PREFIX) and registered != IDENTITY_PREFIX + "p0-old"


def test_a_takaro_api_that_refuses_a_call_fails_that_row_and_still_cleans_up(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An API error is a verdict on one check, not a traceback that loses the whole report."""
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    with hosted_takaro(monkeypatch, tmp_path / "ws.log", broken="/reachability") as rest:
        code, payload, _ = verify(run, wired, artifacts, out, "--takaro", "hosted")

    assert code == 8, payload
    report = report_of(out)
    heartbeat = row(report, "heartbeat")
    assert heartbeat["status"] == "fail"
    assert any("404" in problem for problem in heartbeat["detail"]["problems"]), heartbeat
    assert row(report, "hosted-registration")["status"] == "pass"
    assert report["outcome"] == "fail"
    assert rest.servers == {}, "a failed hosted run still deletes the gameserver it registered"


def test_a_hosted_identity_always_fits_takaros_identity_column() -> None:
    """Takaro answers an over-long identityToken with a bare 400 on identify.

    Observed against the real Takaro: every identity of 50 characters or fewer registered,
    and `takaro-maint-minecraft-neoforge-1.21.11-p1fh-374e93` (51) failed with
    "Identify failed: Request failed with status code 400" and no gameserver. The identity
    must therefore be built to fit instead of assembled and hoped for.
    """
    from takaro_maint import catalog as catalog_pkg
    from takaro_maint import paths
    from takaro_maint.games.minecraft.verify import (
        IDENTITY_LIMIT,
        hosted_identity,
        hosted_identity_prefix,
    )

    paths.set_repo_root(REPO_ROOT)
    catalog = catalog_pkg.load()
    targets = list(catalog.game("minecraft").targets)
    assert targets, "the minecraft catalog should declare targets"

    for target in targets:
        prefix = hosted_identity_prefix(target)
        for run_id in ("p1", "p1fh2", "gha-35553725171-1", "x" * 40):
            identity = hosted_identity(target, run_id, "abc123")
            assert len(identity) <= IDENTITY_LIMIT, (target.id, run_id, identity)
            # The stale-registration sweep matches on the prefix and the nonce keeps two
            # runs apart, so neither may be what the clamp eats.
            assert identity.startswith(prefix), (target.id, run_id, identity)
            assert identity.endswith("abc123"), (target.id, run_id, identity)

    # The prefix cannot depend on the run id, or a later run could not sweep an earlier one.
    neoforge = next(t for t in targets if t.id == "neoforge-1.21.11")
    assert hosted_identity(neoforge, "p1h", "525b35").startswith(hosted_identity_prefix(neoforge))
    assert hosted_identity(neoforge, "p1fh", "374e93").startswith(hosted_identity_prefix(neoforge))
