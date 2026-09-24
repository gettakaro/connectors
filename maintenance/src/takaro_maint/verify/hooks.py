"""The typed seam between the verification runner and a game's own ``verify`` module.

``GameHooks`` makes each hook a declared field with a declared type, so mypy checks names
and signatures and a game that ships nothing gets the defaults rather than a missing
attribute. The runner consumes this object directly instead of discovering callbacks by
attribute-name conventions.

Every ``games/<game>/verify.py`` keeps its module-level constants (the tests import them)
and ends with one ``HOOKS = GameHooks(...)``.
"""

from __future__ import annotations

import re
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import checks as base_checks


@dataclass(frozen=True)
class GameHooks:
    """What one game contributes to a verification run. Every field has a default."""

    #: The server log line that says the boot finished.
    ready_line: re.Pattern[str] = base_checks.DONE_LINE

    #: How long this game normally needs to reach its ready line; the CLI may override it.
    startup_timeout: float | None = None

    #: Check ids this game adds to the base ladder.
    check_ids: tuple[str, ...] = ()

    #: Game check ids implemented by ``negative``. They are opt-in for a bare run, but an
    #: explicit ``--checks`` selection invokes the hook even without the convenience flag.
    negative_check_ids: tuple[str, ...] = ()

    #: Check ids that only a hosted run can produce.
    hosted_check_ids: tuple[str, ...] = ()

    #: Base check id -> why it cannot pass on this game, and which check stands in for it.
    #: A run that names no ``--checks`` excludes these rather than failing them.
    unsupported_checks: Mapping[str, str] = field(default_factory=dict)

    #: Anything the server must find on disk before its container starts.
    before_boot: Callable[[Any, dict[str, str]], Any] | None = None

    #: Optional game-specific preparation of a read-only, pre-existing server base.
    #: Returns attested pinned inputs and provenance; it never writes to that base.
    prepare_readonly_base: Callable[[Any, dict[str, Any]], tuple[list[dict[str, Any]], dict[str, Any]]] | None = None

    #: Anything that can only exist once the container does -- a sidecar on its network.
    after_boot: Callable[[Any, Any, dict[str, str]], Any] | None = None

    #: The game's own checks, run after the base protocol ladder.
    after_protocol: Callable[[Any, Any, Any], Awaitable[None]] | None = None

    #: Checks that need the server gone -- a re-boot, a re-hash of the install.
    after_shutdown: Callable[[Any, Any, str, list[dict[str, Any]]], Awaitable[None]] | None = None

    #: The ``--negative`` check: a wrong artifact has to be refused.
    negative: Callable[[Any, Any, str, dict[str, Any]], Awaitable[None]] | None = None

    #: ``--takaro hosted``: the whole run, against a real Takaro rather than the fake.
    run_hosted: Callable[[Any, dict[str, Any], list[dict[str, Any]], str], Awaitable[dict[str, Any]]] | None = None

    #: How this game's runtime identity is read out of the server log.
    scan_runtime_identity: Callable[[Any, Path], dict[str, Any] | None] | None = None
