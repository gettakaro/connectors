"""NeoForge specifics: the env the rig and the container need, and how FML names itself."""

from __future__ import annotations

import re
from typing import Any

from . import fabric
from .fabric import TARGET_CHECK_PREFIX

# NeoForgeMod's own banner, pinned on the line a 21.11.45 server really writes:
# "NeoForge mod loading, version 21.11.45, for MC 1.21.11".
# Deliberately not the itzg image's "Running NeoForge <v> installer for Minecraft <v>" line:
# that one appears before the server starts and says nothing about what it loaded.
_BANNER = re.compile(
    r"NeoForge mod loading, version (?P<loader>\d+\.\d+\.\d+), for MC (?P<game>\d+\.\d+(?:\.\d+)?)",
)


def env(resolved: dict[str, Any], prefix: str) -> dict[str, str]:
    """NeoForge adds the loader version and the pre-staged installer the itzg image needs."""
    loader = resolved["inputs"]["loader"]
    return {
        f"{prefix}_VERSION": str(resolved["revision"]),
        f"{prefix}_LOADER_VERSION": str(loader["loaderVersion"]),
        f"{prefix}_INSTALLER": "/data/" + str(loader["installPath"]),
    }


def runtime_env(resolved: dict[str, Any]) -> dict[str, str]:
    """The container environment recorded on the target itself."""
    return dict(resolved["runtime"]["container"].get("env", {}))


def parse_runtime_identity(log_line: str) -> dict[str, Any] | None:
    """Either the FML banner or the connector's own target-check line."""
    match = _BANNER.search(log_line)
    if match:
        return {
            "gameVersion": match.group("game"),
            "loader": "neoforge",
            "loaderVersion": match.group("loader"),
        }
    if TARGET_CHECK_PREFIX in log_line:
        # core/ writes that line, identically on every platform.
        return fabric.parse_runtime_identity(log_line)
    return None
