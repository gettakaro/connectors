"""``catalog validate`` and ``catalog record-hash``."""

from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path
from typing import Any

from .. import net, output, paths
from ..catalog import ids, load, validate_catalog
from ..exit_codes import OK, IntegrityError, UsageError


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("catalog", help="validate the catalog and record upstream hashes")
    inner = parser.add_subparsers(dest="catalog_command", metavar="<subcommand>")

    validate = inner.add_parser("validate", help="check every catalog invariant")
    validate.add_argument(
        "--online",
        action="store_true",
        help="also re-fetch every pinned input and compare its hash with the record",
    )
    validate.set_defaults(handler=_validate, op="catalog validate")

    coverage = inner.add_parser("check-maintenance", help="require release watchers for every connector")
    coverage.set_defaults(handler=_check_maintenance, op="catalog check-maintenance")

    record = inner.add_parser(
        "record-hash",
        help="download an input twice and record the sha256 both downloads agree on",
    )
    record.add_argument("--game", required=True)
    record.add_argument("--target", required=True)
    record.add_argument(
        "--field",
        required=True,
        help="dotted path of the hash to write, e.g. inputs.loader.sha256",
    )
    record.set_defaults(handler=_record_hash, op="catalog record-hash")

    parser.set_defaults(handler=None, op="catalog")


def _check_maintenance(args: Any) -> int:
    from ..catalog.coverage import check_maintenance

    result = check_maintenance(paths.repo_root())
    output.emit(
        "catalog check-maintenance",
        result.ok,
        checks=[check.as_dict() for check in result.checks],
        failures=[check.as_dict() for check in result.failures()],
    )
    return result.exit_code


def _validate(args: Any) -> int:
    catalog = load()
    result = validate_catalog(catalog, online=args.online)
    output.emit(
        "catalog validate",
        result.ok,
        online=bool(args.online),
        catalog=str(catalog.root.name),
        checks=[check.as_dict() for check in result.checks],
        failures=[check.as_dict() for check in result.failures()],
    )
    return result.exit_code


def _url_for_field(game_record: dict[str, Any], target_record: dict[str, Any], field: str) -> str:
    parts = field.split(".")
    if parts[:1] == ["inputs"] and len(parts) == 3 and parts[2] == "sha256":
        spec = target_record["inputs"].get(parts[1])
        if not spec or "path" not in spec or "sha256" not in spec:
            raise UsageError(
                f"--field '{field}' names no single-URL input with a sha256 "
                f"(inputs.{parts[1]} is {spec.get('kind') if spec else 'missing'})"
            )
        return ids.resolved_url(game_record, spec["source"], spec["path"])
    if parts[:2] == ["build", "deps"] and len(parts) == 4 and parts[3] == "sha256":
        dep = target_record["build"]["deps"].get(parts[2])
        if not dep:
            raise UsageError(f"--field '{field}' names no build dependency")
        group, artifact, version = dep["coordinate"].split(":")
        source = "fabric-maven" if group.startswith("net.fabricmc") else None
        if source is None:
            raise UsageError(f"cannot derive a download URL for {dep['coordinate']}")
        return ids.resolved_url(game_record, source, ids.maven_path(group, artifact, version))
    raise UsageError(
        f"--field '{field}' is not a recordable hash; use inputs.<name>.sha256 or build.deps.<name>.sha256"
    )


def _write_field(path: Path, field: str, value: str) -> None:
    """Replace one ``"sha256": …`` in place, keeping key order and 2-space indentation."""
    text = path.read_text(encoding="utf-8")
    parts = field.split(".")
    # Anchor on the owning block so the right sha256 is replaced even when several exist.
    owner = parts[1] if parts[0] == "inputs" else parts[2]
    pattern = re.compile(
        r'("' + re.escape(owner) + r'"\s*:\s*\{.*?"sha256"\s*:\s*)(null|"[0-9a-f]{64}")',
        re.DOTALL,
    )
    new_text, count = pattern.subn(lambda m: m.group(1) + f'"{value}"', text, count=1)
    if count != 1:
        raise UsageError(f"could not locate {field} in {path}")
    path.write_text(new_text, encoding="utf-8")


def _download_once(url: str, dest: Path) -> tuple[str, int]:
    digests = net.fetch(url, dest, net.Expectation(), no_cache=True)
    return str(digests["sha256"]), int(digests["size"])


def _record_hash(args: Any) -> int:
    catalog = load()
    target = catalog.select(args.game, target_id=args.target)
    game_record = catalog.game(args.game).record
    url = _url_for_field(game_record, target.record, args.field)

    with tempfile.TemporaryDirectory(prefix="takaro-maint-tofu-") as tmp:
        first_path = Path(tmp) / "first"
        second_path = Path(tmp) / "second"
        first, first_size = _download_once(url, first_path)
        output.info(f"download 1: {url} -> {first_size} bytes sha256 {first}")
        second, second_size = _download_once(url, second_path)
        output.info(f"download 2: {url} -> {second_size} bytes sha256 {second}")

    if first != second or first_size != second_size:
        raise IntegrityError(
            f"{url}: the two downloads disagree (sha256 {first} vs {second}); {args.field} left untouched",
            url=url,
            first=first,
            second=second,
        )

    _write_field(target.path, args.field, first)
    output.info(f"recorded {args.field} = {first} in {target.path.relative_to(paths.repo_root()).as_posix()}")
    output.emit(
        "catalog record-hash",
        True,
        game=args.game,
        target=target.id,
        field=args.field,
        url=url,
        sha256=first,
        size=first_size,
        downloads=2,
    )
    return OK
