"""The verification report: the evidence a run produced, validated against its schema."""

from __future__ import annotations

import datetime as dt
import json
import subprocess
from pathlib import Path
from typing import Any

from .. import __version__, net
from ..catalog import ids, schema
from ..exit_codes import VerificationFailed
from ..publish.manifest import source_revision, watched_paths

LEVELS = ("build", "contract", "startup", "protocol")
LEVEL_CHECKS: dict[str, tuple[str, ...]] = {
    "build": ("build",),
    "contract": ("build",),
    "startup": ("startup",),
    "protocol": (
        "connector-load",
        "identify",
        "heartbeat",
        "players",
        "catalog-items",
        "catalog-entities",
        "console",
        "shutdown",
    ),
}
GAME_PROTOCOL_CHECKS: dict[str, tuple[str, ...]] = {
    "ark": (
        "native-health",
        "sidecar-identify",
        "ark-heartbeat",
        "roster",
        "catalog",
        "entities",
        "ark-empty-broadcast",
        "ark-targeted-offline",
        "ark-console",
        "ark-saveworld",
        "native-shutdown",
        "owned-save-reload",
        "stop",
    ),
}


def level_for(checks: list[dict[str, Any]], game: str | None = None) -> str:
    """The highest class whose checks all passed; ``none`` when not even ``build`` did.

    A skipped or failed check never counts as a pass, so a partial run (``--checks players``)
    reports the class it actually reached rather than the lowest class it never observed.
    """
    status = {check["id"]: check["status"] for check in checks}
    reached = "none"
    for level in LEVELS:
        required = (
            GAME_PROTOCOL_CHECKS.get(game, LEVEL_CHECKS[level])
            if level == "protocol" and game is not None
            else LEVEL_CHECKS[level]
        )
        if required and all(status.get(check_id) == "pass" for check_id in required):
            reached = level
        else:
            break
    return reached


def outcome_for(checks: list[dict[str, Any]]) -> str:
    """``pass`` only when a check was actually observed to pass and none failed.

    Skips are not successes: a run in which every check was skipped proves nothing and
    must not come back as a pass.
    """
    statuses = [check["status"] for check in checks]
    if "fail" in statuses or "pass" not in statuses:
        return "fail"
    return "pass"


def repo_identity(repo_root: Path, watched: list[str]) -> tuple[str, str, bool]:
    """``owner/repo`` from the origin remote, HEAD, and whether any watched path is dirty."""

    def git(*args: str) -> str:
        try:
            return subprocess.run(
                ["git", *args], cwd=repo_root, capture_output=True, text=True, check=True
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return ""

    remote = git("remote", "get-url", "origin")
    repo = "unknown"
    if remote:
        cleaned = remote.removesuffix(".git")
        if ":" in cleaned and "//" not in cleaned:
            repo = cleaned.split(":", 1)[1]
        else:
            repo = "/".join(cleaned.rstrip("/").split("/")[-2:])
    revision, dirty = source_revision(repo_root, watched)
    return repo, revision, dirty


def build_report(
    *,
    target: Any,
    game_record: dict[str, Any],
    manifest: dict[str, Any],
    artifacts_dir: Path,
    runtime: dict[str, Any],
    checks: list[dict[str, Any]],
    started_at: str,
    logs: list[Path],
    repo_root: Path,
    takaro: str = "local",
) -> dict[str, Any]:
    repo, revision, dirty = repo_identity(repo_root, watched_paths(target.game))
    coverage: dict[str, Any] = {"gameplay": "not covered - recorded client evidence pending (#174)"}
    if target.game == "ark":
        # The ARK runner can be repaired without rebuilding the already shipped native
        # and sidecar bytes. Keep their build-manifest source distinct from this tool.
        runner_revision, runner_dirty = source_revision(repo_root, ["maintenance"])
        coverage["verificationRunner"] = {"revision": runner_revision, "dirty": runner_dirty}
        revision = manifest.get("sourceRevision", revision)
        dirty = manifest.get("dirty", dirty)
        if runtime.get("readOnlyBase") is not None:
            coverage["readOnlyBase"] = runtime["readOnlyBase"]
    inputs: dict[str, Any] = {}
    for name, spec in target.record["inputs"].items():
        if spec["kind"] == "mojang-version":
            inputs[name] = {
                "url": ids.resolved_url(game_record, spec["server"]["source"], spec["server"]["path"]),
                "sha1": spec["server"]["sha1"],
                "size": int(spec["server"]["size"]),
            }
            continue
        url = ids.input_url(game_record, spec)
        if url is None:
            raise VerificationFailed(
                f"input '{name}' ({spec['kind']}) names no URL to record as evidence; "
                f"its provider has to answer input_url"
            )
        files = spec.get("files")
        if isinstance(files, dict):
            # An input that pins a set of files is evidence about each of them, so the
            # report names every file the target declared rather than the set as a whole.
            for path, entry in sorted(files.items()):
                row: dict[str, Any] = {"url": f"{url}/{path}"}
                if entry.get("sha256"):
                    row["sha256"] = entry["sha256"]
                if entry.get("size") is not None:
                    row["size"] = int(entry["size"])
                inputs[f"{name}:{path}"] = row
            continue
        inputs[name] = {"url": url, "sha256": spec["sha256"]}
    rows = [row for row in manifest["artifacts"] if row["target"] == target.id]
    container = target.record["runtime"]["container"]
    report = {
        "schemaVersion": 1,
        "kind": "runtime-verification",
        "tool": {"name": "takaro-maint", "version": __version__},
        "source": {"repo": repo, "revision": revision, "dirty": dirty},
        "target": {
            "game": target.game,
            "id": target.id,
            "fingerprint": target.fingerprint,
            "inputs": inputs,
        },
        "artifacts": [
            {
                "role": row["role"],
                "file": row["file"],
                "sha256": row["sha256"],
                "connectorVersion": manifest["version"],
            }
            for row in rows
        ],
        "runtime": {
            "image": {"ref": ids.container_ref(container), "digest": container["digest"]},
            "gameVersion": runtime.get("gameVersion"),
            "loader": runtime.get("loader"),
            "loaderVersion": runtime.get("loaderVersion"),
            "java": runtime.get("java"),
        },
        "takaro": takaro,
        "level": level_for(checks, target.game),
        "checks": checks,
        "outcome": outcome_for(checks),
        "startedAt": started_at,
        "finishedAt": dt.datetime.now(dt.UTC).isoformat().replace("+00:00", "Z"),
        "logs": [{"name": log.name, "sha256": net.sha256_file(log)} for log in logs if log.is_file()],
        "coverage": coverage,
    }
    del artifacts_dir
    return report


def write_report(out: Path, report: dict[str, Any]) -> Path:
    errors = schema.errors_for("verify-report.schema.json", report)
    if errors:
        raise VerificationFailed("the verification report does not validate: " + "; ".join(errors))
    out.mkdir(parents=True, exist_ok=True)
    path = out / "report.json"
    path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return path
