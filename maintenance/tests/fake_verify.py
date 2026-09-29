"""Small fakes for driving every game-specific verification coroutine under pytest."""

from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from takaro_maint.verify.checks import CheckResult
from takaro_maint.verify.runner import RunOptions


class CannedSocket:
    def __init__(
        self,
        answers: dict[str, Any | Exception],
        *,
        identify_count: int = 0,
        reconnects: bool = True,
    ) -> None:
        self.answers = answers
        self.identify_count = identify_count
        self.reconnects = reconnects
        self.host = "127.0.0.1"
        self.port = 34567
        self.identified: dict[str, Any] | None = {"gameServerId": "test"} if identify_count else None
        self.events: list[dict[str, Any]] = []
        self.requests: list[tuple[str, Any, float | None]] = []
        self.disconnects: list[tuple[int, str]] = []
        self.ping_error: Exception | None = None
        self.app_pings = 0

    async def request(self, action: str, params: Any, timeout: float | None = None) -> Any:
        self.requests.append((action, params, timeout))
        answer = self.answers.get(action)
        if isinstance(answer, Exception):
            raise answer
        if callable(answer):
            return answer(action, params)
        return answer

    async def ping(self, timeout: float | None = None, payload: bytes | None = None) -> float:
        del timeout, payload
        if self.ping_error is not None:
            raise self.ping_error
        return 0.001

    async def disconnect(self, code: int, reason: str) -> None:
        self.disconnects.append((code, reason))
        if self.reconnects:
            self.identify_count += 1

    async def wait_for_identify(self, timeout: float | None = None) -> dict[str, Any]:
        del timeout
        if self.identify_count < 1:
            raise TimeoutError("no identify frame")
        return self.identified or {}


class FakeContainer:
    def __init__(self, log_file: Path, *, exit_code: int = 0, alive: bool = True) -> None:
        self.name = "verify-container"
        self.log_file = log_file
        self.exit_code = exit_code
        self._alive = alive

    def alive(self) -> bool:
        return self._alive

    def wait_for_exit(self, timeout: float) -> int:
        del timeout
        return self.exit_code

    def remove(self) -> None:
        self._alive = False


class FakeRun:
    def __init__(
        self,
        tmp_path: Path,
        *,
        target: Any | None = None,
        wanted: set[str] | None = None,
        options: RunOptions | None = None,
    ) -> None:
        self.out = tmp_path / "out"
        self.data_dir = tmp_path / "data"
        self.out.mkdir(parents=True, exist_ok=True)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.server_log = self.out / "server.log"
        self.fake_log = self.out / "fake-takaro.log"
        self.docker_log = self.out / "docker.log"
        for path in (self.server_log, self.fake_log, self.docker_log):
            path.write_text("", encoding="utf-8")
        self.options = options or RunOptions(artifacts=tmp_path, out=self.out, run_id="body-tests")
        self.target = target or SimpleNamespace(id="target", fp16="0123456789abcdef", record={"revision": "1"})
        self.resolved: dict[str, Any] = {}
        self._wanted = wanted
        self.results: list[CheckResult] = []
        self.skips: list[tuple[str, str]] = []
        self.container: FakeContainer | None = FakeContainer(self.server_log)
        self.containers: list[FakeContainer] = [self.container]
        self.bridge: FakeContainer | None = None
        self.ws_url = "ws://127.0.0.1:34567/"
        self.startup_timeout = 0.01

    def wanted(self, check_id: str) -> bool:
        return self._wanted is None or check_id in self._wanted

    def record(self, result: CheckResult) -> None:
        self.results.append(result)

    def skip(self, check_id: str, reason: str) -> None:
        self.skips.append((check_id, reason))

    def boot(self, ws_url: str, **kwargs: Any) -> FakeContainer:
        del ws_url
        log = self.out / str(kwargs.get("log_name") or "boot.log")
        log.touch()
        container = FakeContainer(log)
        self.container = container
        self.containers.append(container)
        return container

    def takaro_env(self, suffix: str) -> dict[str, str]:
        del suffix
        return {"TAKARO_REGISTRATION_TOKEN": "test-token"}


def docker_stub(tmp_path: Path, answers: dict[str, tuple[int, str, str]] | None = None) -> Path:
    """Write a docker executable whose first subcommand selects a canned response."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir(exist_ok=True)
    script = bin_dir / "docker"
    payload = json.dumps(answers or {})
    script.write_text(
        "#!/usr/bin/env python3\n"
        "import json, sys\n"
        f"answers = json.loads({payload!r})\n"
        "code, out, err = answers.get(sys.argv[1] if len(sys.argv) > 1 else '', [0, '', ''])\n"
        "sys.stdout.write(out)\n"
        "sys.stderr.write(err)\n"
        "raise SystemExit(code)\n",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script
