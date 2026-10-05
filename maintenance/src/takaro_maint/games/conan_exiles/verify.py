"""Conan Exiles' verification hooks: the pinned server boots, and nothing more is claimed.

The connector is a native library inside the server process (``LD_PRELOAD`` on Linux), and
the verifier boots the server without it: the library needs a TLS Takaro endpoint and its own
``takaro.json``, and its protocol is proven by ``games/conan-exiles/native/tests`` (unit,
drift, fake-Takaro wire tests and the real library preloaded into a stand-in server) and by
the live runbook in ``games/conan-exiles/README.md``. So a Conan run proves ``build`` and
``startup`` -- the pinned depot bytes boot in the runtime image and stay the ledger's bytes --
and every base row that would need a connector to answer is excluded with that reason.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from ...verify.hooks import GameHooks

READY_LINE = re.compile(r"LogInit: Display: Engine is initialized\. Leaving FEngineLoop::Init\(\)")

_NO_CONNECTOR = (
    "the verifier boots the server without the native library, so nothing answers Takaro; "
    "games/conan-exiles/native/tests proves the protocol"
)

#: Base checks this connector cannot satisfy here, and why. A run that names no ``--checks``
#: excludes these rather than failing them.
UNSUPPORTED_CHECKS = {
    "connector-load": _NO_CONNECTOR,
    "identify": _NO_CONNECTOR,
    "heartbeat": _NO_CONNECTOR,
    "players": _NO_CONNECTOR,
    "catalog-items": f"spot-checks a Minecraft item id; {_NO_CONNECTOR}",
    "catalog-entities": f"spot-checks a Minecraft entity id; {_NO_CONNECTOR}",
    "console": f"drives a Minecraft command; {_NO_CONNECTOR}",
    "shutdown": f"Takaro's shutdown needs a connector to receive it; {_NO_CONNECTOR}",
}

BANNER_MARKERS = ("LogInit: Build:", "LogInit: Engine Version:")
SERVER_LOG = Path("ConanSandbox") / "Saved" / "Logs" / "ConanSandbox.log"


def scan_runtime_identity(adapter: Any, log_file: Path) -> dict[str, Any]:
    """The build and the engine that actually booted, merged from the two banner lines."""
    identity: dict[str, Any] = {}
    if not log_file.is_file():
        return identity
    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not any(marker in line for marker in BANNER_MARKERS):
                continue
            parsed = adapter.parse_runtime_identity(line)
            if not parsed:
                continue
            for key, value in parsed.items():
                if value is not None and identity.get(key) is None:
                    identity[key] = value
            if identity.get("gameVersion") and identity.get("loaderVersion"):
                break
    return identity


async def after_shutdown(run: Any, fake: Any, ws_url: str, ledger_inputs: list[dict[str, Any]]) -> None:
    """Keep the log the server writes inside its own tree, where the report can cite it."""
    del fake, ws_url, ledger_inputs
    source = run.data_dir / SERVER_LOG
    if not source.is_file():
        return
    destination = run.out / SERVER_LOG.name
    destination.write_bytes(source.read_bytes())
    if destination not in run.extra_logs:
        run.extra_logs.append(destination)


#: What this game contributes to a verification run; the runner reads nothing else.
HOOKS = GameHooks(
    ready_line=READY_LINE,
    unsupported_checks=UNSUPPORTED_CHECKS,
    after_shutdown=after_shutdown,
    scan_runtime_identity=scan_runtime_identity,
)
