"""Turning per-target build and verification outputs into one complete release directory.

A connector is released as a set, not as a pile of jars: every non-retired target, every role
the catalog declares for it, each one carrying this target's fingerprint and backed by a passing
verification report at or above the level the target demands. Anything less does not assemble,
so an incomplete set can never reach a release page.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any

from .. import net
from ..catalog import ids, schema
from ..catalog.loader import Catalog, Target, resolve
from ..exit_codes import ConflictError, IntegrityError, UsageError, VerificationFailed
from ..verify.report import LEVELS
from . import compat_record
from .checksums import write_checksums
from .manifest import read_manifest

#: The verification ladder, lowest first. ``gameplay`` is reached only by a game whose harness
#: drives a player (Minecraft); a target of any other game that demands it can never be met.
RANKS: tuple[str, ...] = ("none", *LEVELS, "gameplay")

#: Files a legacy ``--dist`` directory holds that are descriptions of a build, not assets of it.
_NON_ASSET_NAMES = {"SHA256SUMS", "build-manifest.json"}


def rank(level: str) -> int:
    try:
        return RANKS.index(level)
    except ValueError:  # pragma: no cover - the schemas constrain both ends
        return -1


class Row:
    """One artifact a build manifest describes, plus the file it points at."""

    def __init__(self, row: dict[str, Any], path: Path) -> None:
        self.role = str(row["role"])
        self.target = str(row["target"])
        self.fingerprint = str(row["fingerprint"])
        self.file = str(row["file"])
        self.sha256 = str(row["sha256"])
        self.size = int(row["size"])
        self.path = path


# -- phase 1: the manifests --------------------------------------------------------------


def manifest_paths(dist: Path) -> list[Path]:
    """``dist/build-manifest.json``, or one per target subdirectory.

    CI downloads one ``dist-<connector>-<target>`` artifact per target without merging them, so
    the normal shape is one subdirectory per target. A subdirectory that holds no manifest is a
    stray download, not an empty target, and assembling around it would silently drop a target.
    """
    direct = dist / "build-manifest.json"
    if direct.is_file():
        return [direct]
    if not dist.is_dir():
        raise ConflictError(f"no build manifest: {dist} is not a directory")
    found: list[Path] = []
    for child in sorted(dist.iterdir()):
        if not child.is_dir():
            continue
        manifest = child / "build-manifest.json"
        if not manifest.is_file():
            raise ConflictError(
                f"unrecognised dist directory '{child.name}': it holds no build-manifest.json",
                directory=child.name,
            )
        found.append(manifest)
    if not found:
        raise ConflictError(f"no build manifest under {dist}")
    return found


def read_rows(
    dist: Path, *, connector: str, version: str, source_commit: str, allow_dirty: bool
) -> tuple[list[Row], bool]:
    """Every manifest row, with its bytes present and re-hashed against the manifest.

    Also the answer to "was any of this built from a tree that had uncommitted changes?". Each
    build records that for itself, and a build that was dirty stays dirty however clean the
    checkout doing the assembly happens to be — otherwise artifacts built beside uncommitted
    edits could be moved into a fresh clone and published as a clean build of that commit.
    """
    rows: list[Row] = []
    built_dirty = False
    for path in manifest_paths(dist):
        manifest = read_manifest(path)
        if manifest["connector"] != connector:
            raise ConflictError(
                f"{path.parent.name}/build-manifest.json is for connector '{manifest['connector']}', not '{connector}'"
            )
        if manifest["version"] != version:
            raise ConflictError(
                f"{path.parent.name}/build-manifest.json is for version '{manifest['version']}', not '{version}'"
            )
        if manifest["sourceRevision"] != source_commit:
            raise ConflictError(
                f"{path.parent.name}/build-manifest.json was built from '{manifest['sourceRevision']}' but this "
                f"assembly is for '{source_commit}' — recovery must check out the tag, or pass --source-commit",
                manifestRevision=manifest["sourceRevision"],
                sourceCommit=source_commit,
            )
        if manifest.get("dirty"):
            if not allow_dirty:
                raise ConflictError(
                    f"{path.parent.name}/build-manifest.json says the build was made from a dirty tree, "
                    "so its artifacts cannot be attributed to a commit; rebuild from a clean checkout",
                    directory=path.parent.name,
                    sourceCommit=source_commit,
                )
            built_dirty = True
        for entry in manifest["artifacts"]:
            rows.append(_row_with_bytes(path.parent, entry))
    return rows, built_dirty


def _row_with_bytes(directory: Path, entry: dict[str, Any]) -> Row:
    file = directory / str(entry["file"])
    if not file.is_file():
        raise ConflictError(f"{entry['file']} is in the build manifest but not in {directory.name}/")
    meta_path = directory / f"{entry['file']}.meta.json"
    if not meta_path.is_file():
        raise ConflictError(f"{entry['file']} has no {entry['file']}.meta.json beside it")
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConflictError(f"{meta_path.name}: not readable ({exc})") from exc
    for field in ("role", "target", "fingerprint", "sha256"):
        if meta.get(field) != entry[field]:
            raise ConflictError(
                f"{meta_path.name} disagrees with the build manifest on {field}: "
                f"{meta.get(field)!r} against {entry[field]!r}"
            )
    actual = net.sha256_file(file)
    if actual != entry["sha256"]:
        raise IntegrityError(
            f"{entry['file']} does not hash to what the build manifest recorded "
            f"(manifest {entry['sha256']}, file {actual})",
            file=str(entry["file"]),
            expected=entry["sha256"],
            actual=actual,
        )
    return Row(entry, file)


# -- phase 2: the set --------------------------------------------------------------------


def index_rows(rows: list[Row], targets: list[Target], *, version: str) -> dict[tuple[str, str], Row]:
    """``(target, role) -> row``, once every row is known to belong to this set."""
    by_id = {target.id: target for target in targets}
    for row in rows:
        if row.target not in by_id:
            raise ConflictError(
                f"unexpected target '{row.target}': it is not a non-retired target of this connector",
                target=row.target,
            )

    index: dict[tuple[str, str], Row] = {}
    for row in rows:
        key = (row.target, row.role)
        seen = index.get(key)
        if seen is not None and seen.sha256 != row.sha256:
            raise ConflictError(
                f"conflicting builds for {row.target}/{row.role}: {seen.sha256} and {row.sha256}",
                target=row.target,
                role=row.role,
            )
        index[key] = row

    missing: list[dict[str, str]] = []
    for target in targets:
        for component in target.record.get("components", []):
            role = str(component["role"])
            if (target.id, role) not in index:
                missing.append({"target": target.id, "role": role})
    if missing:
        raise ConflictError(
            "the set is incomplete: " + ", ".join(f"{m['target']}/{m['role']}" for m in missing),
            missing=missing,
        )

    for target in targets:
        for component in target.record.get("components", []):
            role = str(component["role"])
            row = index[(target.id, role)]
            if row.fingerprint != target.fingerprint:
                raise ConflictError(
                    f"stale fingerprint for {target.id}/{role}: the build recorded {row.fingerprint[:16]} "
                    f"but the catalog now says {target.fingerprint[:16]}",
                    target=target.id,
                    role=role,
                )
            expected = ids.artifact_file_name(target.record, role, version)
            if row.file != expected:
                raise ConflictError(
                    f"{target.id}/{role} is named {row.file}, not the catalog's {expected}",
                    target=target.id,
                    role=role,
                    expected=expected,
                )
    return index


# -- phase 3: the evidence ---------------------------------------------------------------


def read_reports(reports: Path | None) -> dict[str, tuple[Path, dict[str, Any]]]:
    """Every verification report under ``--reports``, keyed by the target it is about."""
    if reports is None or not reports.is_dir():
        return {}
    found: dict[str, tuple[Path, dict[str, Any]]] = {}
    for path in sorted(reports.rglob("report.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ConflictError(
                f"{path.name} under {path.parent.name}/ is not a readable verification report ({exc})"
            ) from exc
        errors = schema.errors_for("verify-report.schema.json", document)
        if errors:
            raise ConflictError(
                f"{path.parent.name}/{path.name} is not a valid verification report: " + "; ".join(errors)
            )
        target_id = str(document["target"]["id"])
        if target_id in found:
            raise ConflictError(f"two verification reports for target '{target_id}'", target=target_id)
        found[target_id] = (path, document)
    return found


def evidence_for(
    target: Target,
    index: dict[tuple[str, str], Row],
    reports: dict[str, tuple[Path, dict[str, Any]]],
    *,
    source_commit: str,
    allow_dirty: bool,
) -> tuple[dict[str, Any], Path | None, bool]:
    """What this target's verification proves, or the reason it does not prove enough."""
    required = str(target.record["verification"]["required"])
    found = reports.get(target.id)
    if found is None:
        if rank(required) >= rank("startup"):
            raise VerificationFailed(
                f"evidence missing for {target.id}: it requires a '{required}' verification report",
                target=target.id,
                required=required,
            )
        return {"required": required, "executed": None, "report": None, "outcome": None, "takaro": None}, None, False

    path, report = found
    outcome = str(report["outcome"])
    level = str(report["level"])
    if outcome != "pass":
        raise VerificationFailed(f"{target.id}: its verification report says outcome '{outcome}'", target=target.id)
    if rank(level) < rank(required):
        raise VerificationFailed(
            f"{target.id}: verified only to '{level}' but the catalog requires '{required}'",
            target=target.id,
            executed=level,
            required=required,
        )
    if report["target"]["fingerprint"] != target.fingerprint:
        raise ConflictError(
            f"{target.id}: the verification report is for fingerprint "
            f"{report['target']['fingerprint'][:16]}, the catalog says {target.fingerprint[:16]}",
            target=target.id,
        )
    if report["source"]["revision"] != source_commit:
        raise ConflictError(
            f"{target.id}: the verification report is for source {report['source']['revision']}, "
            f"this assembly is for {source_commit}",
            target=target.id,
        )
    verified_dirty = bool(report["source"].get("dirty"))
    if verified_dirty and not allow_dirty:
        raise ConflictError(
            f"{target.id}: the verification report was made from a dirty tree, so it cannot attest to "
            f"{source_commit}; verify from a clean checkout",
            target=target.id,
        )
    verified = {str(item["role"]): str(item["sha256"]) for item in report["artifacts"]}
    for component in target.record.get("components", []):
        role = str(component["role"])
        row = index[(target.id, role)]
        if verified.get(role) != row.sha256:
            raise ConflictError(
                f"{target.id}/{role}: verified bytes are not the published bytes "
                f"(report {verified.get(role)}, artifact {row.sha256})",
                target=target.id,
                role=role,
            )
    return (
        {
            "required": required,
            "executed": level,
            "report": None,
            "outcome": outcome,
            "takaro": str(report["takaro"]),
        },
        path,
        verified_dirty,
    )


# -- phase 4: the directory --------------------------------------------------------------


def prepare_out(out: Path) -> None:
    if out.exists():
        if not out.is_dir():
            raise UsageError(f"--out {out} exists and is not a directory")
        if any(out.iterdir()):
            raise UsageError(f"--out {out} is not empty; assembly never writes into an existing set")
    out.mkdir(parents=True, exist_ok=True)


class Names:
    """The names an assembly has already spoken for, so that nothing is quietly overwritten.

    Every asset is copied into one flat directory and then described by one entry in the compat
    record. If two of them land on the same name the second copy wins on disk while both entries
    survive in the record, leaving a published sha256 that no published byte answers to. The
    checksum file and the record itself are reserved for the same reason.
    """

    def __init__(self, reserved: list[str]) -> None:
        self._taken = {name: "reserved" for name in reserved}

    def claim(self, name: str, kind: str) -> str:
        held = self._taken.get(name)
        if held is not None:
            raise ConflictError(
                f"two assets want the name '{name}' ({held} and {kind}); a release set carries one file per name",
                asset=name,
            )
        self._taken[name] = kind
        return name


def _asset(name: str, kind: str, path: Path, repo: str, tag: str) -> dict[str, Any]:
    digests = net.hash_file(path)
    return {
        "name": name,
        "kind": kind,
        "sha256": digests["sha256"],
        "size": int(digests["size"]),
        "url": compat_record.download_url(repo, tag, name),
    }


def assemble_catalog(
    catalog: Catalog,
    *,
    connector: str,
    version: str,
    channel: str,
    tag: str,
    dist: Path,
    reports: Path | None,
    out: Path,
    repo: str,
    source_commit: str,
    dirty: bool,
    allow_dirty: bool,
    stamp: str,
) -> dict[str, Any]:
    """The full catalog-mode assembly, in the order that makes the first failure the real one."""
    game = catalog.game(connector)
    targets = catalog.selectable(connector, all_targets=True)
    rows, built_dirty = read_rows(
        dist, connector=connector, version=version, source_commit=source_commit, allow_dirty=allow_dirty
    )
    index = index_rows(rows, targets, version=version)
    report_index = read_reports(reports)

    evidence: dict[str, tuple[dict[str, Any], Path | None]] = {}
    verified_dirty = False
    for target in targets:
        verification, report_path, dirty_flag = evidence_for(
            target, index, report_index, source_commit=source_commit, allow_dirty=allow_dirty
        )
        evidence[target.id] = (verification, report_path)
        verified_dirty = verified_dirty or dirty_flag

    prepare_out(out)
    names = Names(["SHA256SUMS", compat_record.record_name(connector, version)])
    assets: list[dict[str, Any]] = []
    record_targets: dict[str, dict[str, Any]] = {}
    by_role_name: dict[tuple[str, str], str] = {}

    for target in targets:
        resolved = resolve(catalog, target)
        artifacts: list[dict[str, Any]] = []
        for component in target.record.get("components", []):
            role = str(component["role"])
            row = index[(target.id, role)]
            destination = out / names.claim(row.file, f"{target.id}/{role}")
            shutil.copy2(row.path, destination)
            by_role_name[(target.id, role)] = row.file
            asset = _asset(row.file, "artifact", destination, repo, tag)
            assets.append(asset)
            artifacts.append(
                {"role": role, "name": row.file, "sha256": asset["sha256"], "size": asset["size"], "url": asset["url"]}
            )
        verification, report_path = evidence[target.id]
        verification = dict(verification)
        if report_path is not None:
            name = names.claim(compat_record.report_name(connector, version, target.id), "verify-report")
            shutil.copy2(report_path, out / name)
            verification["report"] = name
            assets.append(_asset(name, "verify-report", out / name, repo, tag))
        container = target.record["runtime"]["container"]
        record_targets[target.id] = {
            "platform": target.platform,
            "revision": target.revision,
            "status": target.status,
            "fingerprint": target.fingerprint,
            "inputs": compat_record.input_entries(target.record, resolved["resolvedUrls"]),
            "runtime": {"image": container["image"], "tag": container["tag"], "digest": container["digest"]},
            "verification": verification,
            "artifacts": artifacts,
        }

    aliases, skipped = _write_aliases(
        game.record, out, by_role_name, version=version, repo=repo, tag=tag, assets=assets, names=names
    )

    record = compat_record.build(
        connector=connector,
        version=version,
        channel=channel,
        tag=tag,
        mode="catalog",
        repo=repo,
        source_commit=source_commit,
        source_tag=tag if channel == "stable" else None,
        dirty=dirty or built_dirty or verified_dirty,
        stamp=stamp,
        catalog={"game": game.id, "targetIds": [target.id for target in targets]},
        catalog_hash=compat_record.catalog_sha256(game.record, {target.id: target.record for target in targets}),
        targets=record_targets,
        aliases=aliases,
        assets=sorted(assets, key=lambda a: str(a["name"])),
    )
    return _finish(out, record, connector=connector, version=version, extra={"aliasesSkipped": skipped})


def _write_aliases(
    game_record: dict[str, Any],
    out: Path,
    by_role_name: dict[tuple[str, str], str],
    *,
    version: str,
    repo: str,
    tag: str,
    assets: list[dict[str, Any]],
    names: Names,
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Byte-identical copies under the names the old releases used, while consumers catch up."""
    aliases: dict[str, dict[str, Any]] = {}
    skipped: list[str] = []
    for pattern, reference in sorted((game_record.get("legacyAssetAliases") or {}).items()):
        target_id, _, role = str(reference).partition("/")
        source_name = by_role_name.get((target_id, role))
        if source_name is None:
            skipped.append(pattern)
            continue
        name = names.claim(_alias_name(pattern, version), "alias")
        shutil.copy2(out / source_name, out / name)
        asset = _asset(name, "alias", out / name, repo, tag)
        assets.append(asset)
        aliases[name] = {"target": target_id, "role": role, "of": source_name, "sha256": asset["sha256"]}
    return aliases, skipped


def _alias_name(pattern: str, version: str) -> str:
    """An alias is one file name in the release set, never a path into the file system.

    The catalog is a committed file rather than untrusted input, but it is the one assembly
    input nothing else checks, and ``out / name`` would happily follow ``../`` out of the
    directory or drop it entirely for an absolute key.
    """
    name = pattern.replace("{version}", version)
    if not name or name != Path(name).name or name in {".", ".."}:
        raise UsageError(
            f"legacyAssetAliases key '{pattern}' is not a plain file name; an alias cannot name a path",
            alias=pattern,
        )
    return name


def assemble_legacy(
    *,
    connector: str,
    version: str,
    channel: str,
    tag: str,
    dist: Path,
    out: Path,
    files: list[Path],
    repo: str,
    source_commit: str,
    dirty: bool,
    stamp: str,
) -> dict[str, Any]:
    """A connector that has no catalog yet: the build script names its files, we publish those."""
    chosen = files or _legacy_files(dist)
    if not chosen:
        raise ConflictError(f"no files to publish: {dist} holds no build output")
    for file in chosen:
        if not file.is_file():
            raise ConflictError(f"{file} is not a file")

    prepare_out(out)
    names = Names(["SHA256SUMS", compat_record.record_name(connector, version)])
    assets: list[dict[str, Any]] = []
    for file in sorted(chosen, key=lambda p: p.name):
        destination = out / names.claim(file.name, "artifact")
        shutil.copy2(file, destination)
        assets.append(_asset(file.name, "artifact", destination, repo, tag))

    record = compat_record.build(
        connector=connector,
        version=version,
        channel=channel,
        tag=tag,
        mode="legacy",
        repo=repo,
        source_commit=source_commit,
        source_tag=tag if channel == "stable" else None,
        dirty=dirty,
        stamp=stamp,
        catalog=None,
        catalog_hash=None,
        targets={},
        aliases={},
        assets=assets,
    )
    return _finish(out, record, connector=connector, version=version, extra={"aliasesSkipped": []})


def _legacy_files(dist: Path) -> list[Path]:
    if not dist.is_dir():
        raise ConflictError(f"--dist {dist} is not a directory")
    return [
        child
        for child in sorted(dist.iterdir())
        if child.is_file() and child.name not in _NON_ASSET_NAMES and not child.name.endswith(".meta.json")
    ]


def _finish(
    out: Path, record: dict[str, Any], *, connector: str, version: str, extra: dict[str, Any]
) -> dict[str, Any]:
    """Write the record, then the checksums over everything but themselves."""
    record_path = compat_record.write(out, record)
    write_checksums(out, [p for p in sorted(out.iterdir()) if p.is_file() and p.name != "SHA256SUMS"])
    targets_out = [
        {
            "id": target_id,
            "fingerprint": entry["fingerprint"],
            "roles": [artifact["role"] for artifact in entry["artifacts"]],
            "verification": entry["verification"],
        }
        for target_id, entry in sorted(record["targets"].items())
    ]
    return {
        "connector": connector,
        "version": version,
        "channel": record["channel"],
        "tag": record["tag"],
        "mode": record["mode"],
        "out": str(out),
        "sourceCommit": record["source"]["commit"],
        "dirty": record["source"]["dirty"],
        "targets": targets_out,
        "aliases": record["aliases"],
        "assets": [
            {"name": a["name"], "kind": a["kind"], "sha256": a["sha256"], "size": a["size"]} for a in record["assets"]
        ],
        "compatRecord": record_path.name,
        "checksums": "SHA256SUMS",
        **extra,
    }
