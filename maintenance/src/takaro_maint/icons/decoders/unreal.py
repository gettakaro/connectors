"""The Unreal decoder: settings, the pinned Oodle library and the export requests the container understands."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ... import net, paths
from ...exit_codes import BuildFailed, UsageError
from . import container

DECODER_NAME = "unreal"
OODLE_TOOL_ID = "oodle"
OODLE_MOUNT = "/opt/oodle/liboo2corelinux64.so.9"
GAME_MOUNT = "/game"


@dataclass(frozen=True)
class OodleSpec:
    version: str
    file: str
    url: str
    size: int
    sha256: str


@dataclass(frozen=True)
class UnrealSettings:
    engine: str
    aes_key_env: str | None

    @classmethod
    def of(cls, block: dict[str, Any]) -> UnrealSettings:
        unreal = block["unreal"]
        return cls(engine=str(unreal["engine"]), aes_key_env=unreal.get("aesKeyEnv"))


def read_oodle_lock() -> OodleSpec:
    path = paths.repo_root() / "maintenance" / "tools.lock.json"
    try:
        entry = json.loads(path.read_text(encoding="utf-8"))["tools"][OODLE_TOOL_ID]
        return OodleSpec(
            version=str(entry["version"]),
            file=str(entry["file"]),
            url=str(entry["url"]),
            size=int(entry["size"]),
            sha256=str(entry["sha256"]),
        )
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        raise UsageError(f"{path} does not pin '{OODLE_TOOL_ID}' ({exc})") from exc


def oodle_cached(cache: Path) -> bool:
    return (cache / "blobs" / "sha256" / read_oodle_lock().sha256).is_file()


def ensure_oodle(cache: Path) -> Path:
    """The pinned Oodle library in the content-addressed cache; a wrong hash is exit 5, an unreachable URL exit 4."""
    spec = read_oodle_lock()
    return net.download(spec.url, net.Expectation(sha256=spec.sha256, size=spec.size), cache)


def require_key_variable(settings: UnrealSettings) -> None:
    """A named AES key variable must be set before anything else runs; the value is never read here."""
    if settings.aes_key_env and not os.environ.get(settings.aes_key_env):
        raise UsageError(
            f"environment variable {settings.aes_key_env} is not set; the build reads the game's AES key from it",
            variable=settings.aes_key_env,
        )


@dataclass(frozen=True)
class UnrealDecoder:
    settings: UnrealSettings
    image: container.DecoderImage
    oodle_path: Path
    cache: Path

    def request(self, exports: list[dict[str, Any]]) -> dict[str, Any]:
        return {
            "schemaVersion": 1,
            "decoder": DECODER_NAME,
            "settings": {
                "engine": self.settings.engine,
                "oodle": OODLE_MOUNT,
                "aesKeyEnv": self.settings.aes_key_env,
            },
            "root": GAME_MOUNT,
            "exports": exports,
        }

    def export(self, root: Path, exports: list[dict[str, Any]], *, inputs_sha256: str) -> container.Manifest:
        """Run ``exports`` against the game files under ``root`` (mounted read-only)."""
        mounts = [
            container.Mount(root, GAME_MOUNT),
            container.Mount(self.oodle_path, OODLE_MOUNT),
        ]
        env = [self.settings.aes_key_env] if self.settings.aes_key_env else []
        manifest = container.run_cached(
            self.image,
            self.request(exports),
            mounts,
            inputs_sha256=inputs_sha256,
            cache=self.cache,
            env_names=env,
        )
        if manifest.tool.get("unloaded"):
            raise BuildFailed(
                f"{manifest.tool['unloaded']} containers stayed unmounted (an encrypted pak without a key?)",
                unloaded=manifest.tool["unloaded"],
            )
        return manifest
