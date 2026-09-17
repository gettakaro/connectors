"""Paper specifics: the env the rig and the container need, and how Paper names itself."""

from __future__ import annotations

import re
from typing import Any

from . import fabric
from .fabric import TARGET_CHECK_PREFIX

# "This server is running Paper version 1.21.11-132-main@… (…) (Implementing API version …)"
_BANNER = re.compile(r"This server is running Paper version (?P<game>[0-9][0-9.]*)-(?P<build>[0-9]+)\b")


def env(resolved: dict[str, Any], prefix: str) -> dict[str, str]:
    """Paper adds the build number and the pre-staged jar the itzg image needs."""
    loader = resolved["inputs"]["loader"]
    return {
        f"{prefix}_VERSION": str(resolved["revision"]),
        f"{prefix}_BUILD": str(loader["loaderVersion"]),
        f"{prefix}_CUSTOM_JAR": "/data/" + str(loader["installPath"]),
    }


def runtime_env(resolved: dict[str, Any]) -> dict[str, str]:
    """The container environment recorded on the target itself."""
    return dict(resolved["runtime"]["container"].get("env", {}))


def parse_runtime_identity(log_line: str) -> dict[str, Any] | None:
    """Either the Paper banner or the connector's own target-check line."""
    match = _BANNER.search(log_line)
    if match:
        return {
            "gameVersion": match.group("game"),
            "loader": "paper",
            "loaderVersion": match.group("build"),
        }
    if TARGET_CHECK_PREFIX in log_line:
        # core/ writes that line, identically on every platform.
        return fabric.parse_runtime_identity(log_line)
    return None
