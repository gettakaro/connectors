"""An isolated ARK verification rig backed by exact, read-only game files.

The server base is never an install destination or a writable Docker mount. The
container sees its files under /ark-base:ro; /ark is an owned directory containing
symlinks to immutable game assets and fresh Saved, TakaroArk and .takaro trees.
"""

from __future__ import annotations

import json
import re
import shutil
import stat
from pathlib import Path
from typing import Any

from ... import net
from ...exit_codes import IntegrityError, UsageError
from ...verify import checks

_TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|[{}]')


def _app_state(text: str) -> dict[str, Any]:
    """Parse the small, quoted KeyValues subset used by Steam app manifests."""
    tokens: list[str] = []
    end = 0
    for match in _TOKEN.finditer(text):
        if text[end : match.start()].strip():
            raise IntegrityError("Steam app manifest contains unsupported syntax")
        token = match.group()
        tokens.append(json.loads(token) if token.startswith('"') else token)
        end = match.end()
    if text[end:].strip():
        raise IntegrityError("Steam app manifest contains trailing unsupported syntax")

    def mapping(index: int) -> tuple[dict[str, Any], int]:
        result: dict[str, Any] = {}
        while index < len(tokens) and tokens[index] != "}":
            key = tokens[index]
            if key == "{" or key in result or index + 1 >= len(tokens):
                raise IntegrityError("Steam app manifest has a duplicate or malformed key")
            raw_value = tokens[index + 1]
            value: Any
            if raw_value == "{":
                value, index = mapping(index + 2)
            else:
                if raw_value == "}":
                    raise IntegrityError("Steam app manifest has a missing value")
                value = raw_value
                index += 2
            result[key] = value
        if index >= len(tokens) or tokens[index] != "}":
            raise IntegrityError("Steam app manifest has an unclosed section")
        return result, index + 1

    if len(tokens) < 3 or tokens[:2] != ["AppState", "{"]:
        raise IntegrityError("Steam app manifest has no AppState section")
    state, next_index = mapping(2)
    if next_index != len(tokens):
        raise IntegrityError("Steam app manifest has trailing sections")
    return state


def validate_base(base: Path, target: Any) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Attest the exact executable, Steamworks library, build and both depot manifests."""
    if not base.is_dir():
        raise UsageError(f"ARK read-only base is not a directory: {base}")
    spec = target.record["inputs"]["server"]
    app = str(spec["app"])
    manifest_path = base / "steamapps" / f"appmanifest_{app}.acf"
    if not manifest_path.is_file():
        raise IntegrityError(f"ARK base lacks {manifest_path.relative_to(base)}")
    state = _app_state(manifest_path.read_text(encoding="utf-8"))
    if state.get("appid") != app or state.get("buildid") != str(spec["buildid"]):
        raise IntegrityError("ARK base Steam app/build ID does not match the pinned target")
    if state.get("TargetBuildID") != str(spec["buildid"]):
        raise IntegrityError("ARK base Steam target build ID does not match the pinned target")
    installed = state.get("InstalledDepots")
    expected_depots = {str(name): row for name, row in spec["depots"].items()}
    if not isinstance(installed, dict) or set(installed) != set(expected_depots):
        raise IntegrityError("ARK base Steam depot set does not match the pinned target")
    for name, expected in expected_depots.items():
        actual = installed[name]
        if not isinstance(actual, dict) or actual.get("manifest") != str(expected["manifest"]):
            raise IntegrityError(f"ARK base Steam depot {name} manifest does not match the pinned target")
        if str(actual.get("size")) != str(expected["size"]):
            raise IntegrityError(f"ARK base Steam depot {name} size does not match the pinned target")

    inputs: list[dict[str, Any]] = []
    for relative, expected in sorted(spec["files"].items()):
        path = base / relative
        if not path.is_file():
            raise IntegrityError(f"ARK base lacks pinned input {relative}")
        current = path
        while current != base:
            if current.is_symlink():
                raise IntegrityError(f"ARK base pinned input {relative} traverses a symlink")
            current = current.parent
        digest = net.hash_file(path)
        if digest["sha256"] != expected["sha256"] or digest["size"] != int(expected["size"]):
            raise IntegrityError(f"ARK base pinned input {relative} has the wrong hash or size")
        inputs.append({"name": f"server:{relative}", "path": relative, **digest})
    provenance = {
        "path": str(base),
        "readOnlyMount": True,
        "steamApp": app,
        "steamBuild": state["buildid"],
        "depotManifests": {name: installed[name]["manifest"] for name in sorted(installed)},
        "appManifestSha256": net.sha256_file(manifest_path),
    }
    return inputs, provenance


def project_base(base: Path, owned: Path) -> None:
    """Expose base assets through a read-only mount, with all writable paths owned."""
    if owned.resolve().is_relative_to(base.resolve()) or base.resolve().is_relative_to(owned.resolve()):
        raise UsageError("ARK read-only base and owned runtime directory must be separate")
    shooter = base / "ShooterGame"
    if not shooter.is_dir() or not (shooter / "Binaries").is_dir():
        raise IntegrityError("ARK base ShooterGame directory is incomplete")
    for source in sorted(base.iterdir()):
        if source.name in ("ShooterGame", "TakaroArk", ".takaro"):
            continue
        if source.name == "linux64":
            linux64 = owned / "linux64"
            linux64.mkdir()
            for file in sorted(source.iterdir()):
                target = linux64 / file.name
                if file.name == "steamclient.so":
                    shutil.copy2(file, target)
                else:
                    target.symlink_to(f"/ark-base/linux64/{file.name}", target_is_directory=file.is_dir())
            continue
        (owned / source.name).symlink_to(f"/ark-base/{source.name}", target_is_directory=source.is_dir())
    projected_shooter = owned / "ShooterGame"
    projected_shooter.mkdir()
    for source in sorted(shooter.iterdir()):
        if source.name == "Saved":
            continue
        if source.name == "Binaries":
            binaries = projected_shooter / "Binaries"
            binaries.mkdir()
            for binary_child in sorted(source.iterdir()):
                if binary_child.name == "Linux":
                    linux = binaries / "Linux"
                    linux.mkdir()
                    for file in sorted(binary_child.iterdir()):
                        target = linux / file.name
                        if file.name == "ShooterGameServer":
                            # The engine resolves /proc/self/exe to find its Saved path.
                            # A symlink here would point that lookup at the preserved base.
                            shutil.copy2(file, target)
                        elif file.name == "BanList.txt":
                            # The engine writes this beside its executable, not
                            # under Saved. Project it as owned state below.
                            continue
                        else:
                            target.symlink_to(
                                f"/ark-base/ShooterGame/Binaries/Linux/{file.name}",
                                target_is_directory=file.is_dir(),
                            )
                    _project_ban_list(binary_child / "BanList.txt", linux / "BanList.txt")
                else:
                    (binaries / binary_child.name).symlink_to(
                        f"/ark-base/ShooterGame/Binaries/{binary_child.name}",
                        target_is_directory=binary_child.is_dir(),
                    )
            continue
        (projected_shooter / source.name).symlink_to(
            f"/ark-base/ShooterGame/{source.name}", target_is_directory=source.is_dir()
        )
    (projected_shooter / "Saved").mkdir()
    (owned / ".takaro" / "home").mkdir(parents=True)
    (owned / "TakaroArk").mkdir()


def _project_ban_list(source: Path, target: Path) -> None:
    """Seed an owned ARK BanList once; never overwrite bans from a prior boot."""
    if target.is_symlink():
        # Earlier read-only projections linked this file to /ark-base. That
        # link cannot contain owned changes, so replace it with a regular file.
        target.unlink()
    if target.exists():
        if not target.is_file():
            raise IntegrityError("ARK owned BanList path is not a regular file")
    elif source.exists():
        if source.is_symlink() or not source.is_file():
            raise IntegrityError("ARK base BanList is not a regular file")
        shutil.copy2(source, target)
    else:
        target.touch(mode=0o600, exist_ok=False)
    target.chmod(target.stat().st_mode | stat.S_IWUSR)


def prepare(run: Any, manifest: dict[str, Any]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    base = run.options.ark_readonly_base
    if base is None:
        raise UsageError("ARK read-only base path is required")
    if run.out.resolve().is_relative_to(base):
        raise UsageError("ARK verification report directory cannot be inside the read-only base")
    inputs, provenance = validate_base(base, run.target)
    result = checks.check_build(run.options.artifacts, manifest, run.target)
    if result.status != "pass":
        raise IntegrityError("ARK frozen artifacts do not match the target: " + "; ".join(result.detail["problems"]))
    project_base(base, run.data_dir)
    pinned_files = run.target.record["inputs"]["server"]["files"]
    for relative in ("ShooterGame/Binaries/Linux/ShooterGameServer", "linux64/steamclient.so"):
        owned_file = run.data_dir / relative
        if owned_file.is_symlink() or not owned_file.is_file():
            raise IntegrityError(f"ARK owned pinned input {relative} is not a regular file")
        digest = net.hash_file(owned_file)
        if digest["sha256"] != pinned_files[relative]["sha256"] or digest["size"] != pinned_files[relative]["size"]:
            raise IntegrityError(f"ARK owned pinned input {relative} does not match the target")
    for component in run.target.record["components"]:
        role = component["role"]
        rows = [row for row in manifest["artifacts"] if row["target"] == run.target.id and row["role"] == role]
        if len(rows) != 1:
            raise IntegrityError(f"ARK frozen manifest needs exactly one {role} artifact")
        artifact = run.options.artifacts / rows[0]["file"]
        run.adapter.after_deploy(run.data_dir, component, artifact)
    return inputs, provenance
