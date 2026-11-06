"""``steam pin`` and ``steam references`` — re-pin a Steam target, and fetch what the build needs.

Pinning and building are the two things a maintainer does with a Steam-delivered server
that are not an install: ask Steam what the branch head is now (and record it), and fetch
the handful of assemblies the connector compiles against without downloading the whole
17 GB depot.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import tempfile
from pathlib import Path
from typing import Any

from .. import channels, net, output, paths
from ..exit_codes import OK, ConflictError, IntegrityError, UpstreamUnavailable, UsageError
from ..steam import depotdownloader as dd
from ..steam import steamcmd
from ..steam.install import SteamInput
from ..steam.selectors import selects
from . import add_selection_arguments, load_catalog, select_one

REFERENCES_MARKER = Path(".takaro") / "references.json"


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("steam", help="pin and fetch Steam-delivered server files")
    inner = parser.add_subparsers(dest="steam_command", metavar="<subcommand>")

    pin = inner.add_parser("pin", help="read the current depot manifests and compare them with the pin")
    add_selection_arguments(pin)
    pin.add_argument("--branch", default=None, help="branch to read (default: the target's)")
    pin.add_argument("--buildid", type=int, default=None, help="the build id these manifests belong to")
    pin.add_argument("--depot", action="append", default=None, help="depot id; repeatable (default: the target's)")
    pin.add_argument(
        "--record-files",
        action="append",
        default=None,
        help="download and hash this declared file so --write can record it; repeatable",
    )
    pin.add_argument(
        "--metadata",
        action="store_true",
        help="read the build id from Steam's app metadata and cross-check it against the depots",
    )
    pin.add_argument("--write", action="store_true", help="write the result back into the target record")
    pin.set_defaults(handler=_pin, op="steam pin")

    branches = inner.add_parser("branches", help="list the branches Steam publishes for a game's app")
    branches.add_argument("--game", required=True, help="catalog game id, e.g. 7d2d")
    branches.add_argument("--app", type=int, default=None, help="Steam app id (default: the watch block's)")
    branches.add_argument("--os", default=None, help="linux | windows | macos (default: the watch block's)")
    branches.add_argument("--depot", action="append", default=None, help="depot id; repeatable")
    branches.set_defaults(handler=_branches, op="steam branches")

    references = inner.add_parser("references", help="fetch the build's reference assemblies from the pinned manifests")
    add_selection_arguments(references)
    references.add_argument("--dest", required=True, help="the flat directory the build compiles against")
    references.add_argument("--force", action="store_true", help="replace a directory pinned to another fingerprint")
    references.set_defaults(handler=_references, op="steam references")

    parser.set_defaults(handler=None, op="steam")


def _log_path(cache: Path, game: str, target_id: str) -> Path:
    return cache / "steam" / "logs" / f"{game}-{target_id}.log"


def _app_info_log(cache: Path, app: int) -> Path:
    """steamcmd's own log, beside DepotDownloader's."""
    return steamcmd.log_path(cache, app)


def _steam_watch(game_id: str) -> dict[str, Any]:
    """The watch block of a game's Steam source, or an empty one when it has none yet."""
    game = load_catalog().game(game_id)
    for _, record in sorted((game.record.get("sources") or {}).items()):
        if record.get("provider") == "steam":
            return dict(record.get("watch") or {})
    raise UsageError(f"game '{game_id}' declares no source with provider 'steam'")


def _known(watch: dict[str, Any]) -> list[str]:
    return [str(entry) for entry in watch.get("knownBranches") or []]


def _classify(label: str, watch: dict[str, Any]) -> str:
    """What this run already knows about an upstream branch label.

    ``watched`` and ``declared`` come from the catalog's own channels — the difference is
    ``enabled`` — ``known`` is a label a ``knownBranches`` selector covers, and anything
    left is ``unfamiliar``: a branch nobody has decided about yet.
    """
    if label in {channels.channel_label(channel, key) for key, channel in channels.enabled_channels(watch).items()}:
        return "watched"
    if label in channels.declared_labels(watch):
        return "declared"
    return "known" if selects(label, _known(watch)) else "unfamiliar"


def _branches(args: Any) -> int:
    watch = _steam_watch(args.game)
    app = args.app if args.app is not None else watch.get("app")
    if app is None:
        raise UsageError(f"game '{args.game}' declares no app in its Steam watch block; pass --app <id>")
    app = int(app)
    wanted = [str(depot) for depot in (args.depot or watch.get("depots") or [])]

    info = steamcmd.app_info(app, log=_app_info_log(paths.cache_dir(), app))
    depots = [depot for depot in info.depots.values() if not wanted or depot.id in wanted]
    declared = channels.declared_labels(watch)

    rows: list[dict[str, Any]] = []
    for label in sorted(info.branches):
        branch = info.branches[label]
        manifests = {
            depot.id: {
                "gid": depot.manifests[label].gid,
                "size": depot.manifests[label].size,
                "download": depot.manifests[label].download,
            }
            for depot in depots
            if label in depot.manifests
        }
        rows.append(
            {
                "label": label,
                "branch": declared.get(label),
                "buildid": branch.buildid,
                "timeupdated": steamcmd.iso(branch.timeupdated) if branch.timeupdated is not None else None,
                "timebuildupdated": (
                    steamcmd.iso(branch.timebuildupdated) if branch.timebuildupdated is not None else None
                ),
                "description": branch.description,
                "pwdrequired": branch.pwdrequired,
                "classification": _classify(label, watch),
                "manifests": manifests or None,
                "encrypted": sorted(depot.id for depot in depots if label in depot.encrypted),
            }
        )

    output.emit(
        "steam branches",
        True,
        app=app,
        name=info.name,
        os=args.os or watch.get("os"),
        changeNumber=info.change_number,
        lastChange=info.last_change,
        privateBranches=info.private_branches,
        branches=rows,
        depots=[{"id": depot.id, "oslist": depot.oslist} for depot in sorted(depots, key=lambda d: d.id)],
    )
    return OK


def _metadata(spec: SteamInput, branch: str, observed: dict[str, dict[str, Any]], cache: Path) -> dict[str, Any]:
    """Steam's own view of the branch, cross-checked against the depots just read.

    The build id is not in a depot manifest — DepotDownloader never sees one — so it has to
    come from the app metadata, and the two have to be read a moment apart. If the depot
    head and the metadata head disagree, a publish is in flight between the two reads, and
    recording that pair would pin a build id to manifests that never shipped under it.
    That is a retry, not a record.
    """
    info = steamcmd.app_info(spec.app, log=_app_info_log(cache, spec.app))
    published = info.branches.get(branch)
    if published is None:
        listed = ", ".join(sorted(info.branches)) or "<none>"
        raise UpstreamUnavailable(
            f"app {spec.app} does not list a branch '{branch}'"
            + ("; privatebranches=1: it may be password-protected" if info.private_branches else "")
            + f". Listed: {listed}"
        )
    disagree = []
    compared = []
    for depot, entry in sorted(observed.items()):
        found = info.depots.get(depot)
        manifest = found.manifests.get(branch) if found is not None else None
        if manifest is None:
            # A protected branch publishes its manifest ids encrypted, so the metadata has
            # nothing to compare against. That is not a disagreement -- but it is also not
            # the cross-check this function is named for, so the caller is told which
            # depots it actually got rather than being left to assume all of them.
            continue
        compared.append(depot)
        if manifest.gid != str(entry["manifest"]):
            disagree.append(f"depot {depot}: metadata {manifest.gid}, depot {entry['manifest']}")
    if disagree:
        raise UpstreamUnavailable(
            f"Steam metadata and the depot disagree on the head of '{branch}' ({'; '.join(disagree)}); "
            "a publish is in flight, retry"
        )
    return {
        "buildid": published.buildid,
        "timeupdated": steamcmd.iso(published.timeupdated) if published.timeupdated is not None else None,
        "description": published.description,
        "changeNumber": info.change_number,
        "crossChecked": compared,
    }


def _pin(args: Any) -> int:
    _, target = select_one(args)
    spec = SteamInput.of(target.record)
    cache = paths.cache_dir()
    log = _log_path(cache, target.game, target.id)
    branch = args.branch or spec.branch
    depots = [str(depot) for depot in (args.depot or sorted(spec.depots))]
    metadata = getattr(args, "metadata", False)
    if metadata and args.buildid is not None:
        raise UsageError("--metadata reads the build id from Steam; pass one or the other, not both")
    if args.write and branch != spec.branch and args.depot:
        # A record pins one branch for all of its depots, and the depots this run would
        # skip still carry manifests from the old branch.
        raise UsageError("a record pins one branch; re-pin every depot when changing branch")
    if args.write and branch != spec.branch and args.buildid is None and not metadata:
        raise UsageError(f"re-pinning onto branch {branch} needs that head's build id: pass --buildid or --metadata")

    observed: dict[str, dict[str, Any]] = {}
    changed: list[str] = []
    with tempfile.TemporaryDirectory(prefix="takaro-steam-pin-") as tmp:
        for depot in depots:
            info = dd.manifest_only(
                spec.app,
                depot,
                branch,
                manifest=None,
                os_=spec.os_,
                arch=spec.arch,
                out=Path(tmp),
                cache=cache,
                log=log,
                credentials=spec.credentials,
            )
            observed[depot] = {
                "manifest": info.manifest,
                "size": info.bytes_on_disk,
                "files": info.files,
                "date": info.date,
            }
            pinned = str(spec.depots.get(depot, {}).get("manifest", ""))
            if pinned != info.manifest:
                changed.append(depot)

        recorded = _record_files(args, spec, observed, branch, cache, log) if args.record_files else {}

    published = _metadata(spec, branch, observed, cache) if metadata else None
    buildid = int(published["buildid"]) if published is not None else args.buildid
    snippet = _snippet(spec, observed, buildid, recorded, branch=branch)
    stale = _stale_files(spec, observed, recorded)
    written = None
    if args.write:
        if stale:
            raise ConflictError(
                "these declared files changed in the new manifest and no hash was recorded for them: "
                + ", ".join(stale)
                + "; re-run with --record-files for each before --write",
                files=stale,
            )
        if changed and buildid is None:
            raise UsageError("writing changed Steam manifests needs that head's build id: pass --buildid or --metadata")
        record = dict(target.record)
        record["inputs"] = {**record["inputs"], spec.name: snippet}
        target.path.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        written = str(target.path.relative_to(paths.repo_root()))
        output.info(f"wrote inputs.{spec.name} into {written}")

    output.emit(
        "steam pin",
        True,
        game=target.game,
        target=target.id,
        app=spec.app,
        branch=branch,
        buildid=buildid,
        metadata=published,
        depots=observed,
        files=recorded,
        changed=sorted(changed),
        staleFiles=stale,
        written=written,
        snippet=snippet,
    )
    return OK


def _record_files(
    args: Any, spec: SteamInput, observed: dict[str, dict[str, Any]], branch: str, cache: Path, log: Path
) -> dict[str, dict[str, Any]]:
    """Download the named declared files from the manifests just read, and hash them.

    They come from the manifests this pin observed, never from the ones the record still
    holds: recording the old bytes under a new manifest id is exactly the drift this
    command exists to prevent.
    """
    wanted = [str(path) for path in args.record_files]
    unknown = [path for path in wanted if path not in spec.files]
    if unknown:
        raise UsageError(f"--record-files names paths this target does not declare: {', '.join(unknown)}")
    recorded: dict[str, dict[str, Any]] = {}
    with tempfile.TemporaryDirectory(prefix="takaro-steam-record-") as tmp:
        for depot, entry in sorted(observed.items()):
            out = Path(tmp) / depot
            dd.download(
                spec.app,
                depot,
                str(entry["manifest"]),
                branch=branch,
                os_=spec.os_,
                arch=spec.arch,
                dir=out,
                cache=cache,
                log=log,
                filelist=wanted,
                validate=True,
                credentials=spec.credentials,
            )
            for relative in wanted:
                candidate = out / relative
                if candidate.is_file():
                    digests = net.hash_file(candidate)
                    recorded[relative] = {"sha256": digests["sha256"], "size": int(digests["size"])}
    missing = [path for path in wanted if path not in recorded]
    if missing:
        raise IntegrityError(f"the depot served none of: {', '.join(missing)}")
    return recorded


def _snippet(
    spec: SteamInput,
    observed: dict[str, dict[str, Any]],
    buildid: int | None,
    recorded: dict[str, dict[str, Any]],
    *,
    branch: str,
) -> dict[str, Any]:
    """The ``inputs.<name>`` object this pin would write.

    ``--depot`` narrows what was *observed*, never what the record holds: a depot this run
    did not read keeps the row it is pinned to, or a `--write` of a single-depot re-pin
    would silently drop every other depot from the install.
    """
    depots = {
        **{depot: dict(row) for depot, row in spec.depots.items()},
        **{
            depot: {"manifest": entry["manifest"], "size": entry["size"], "files": entry["files"]}
            for depot, entry in observed.items()
        },
    }
    depots = {depot: depots[depot] for depot in sorted(depots)}
    files = {path: dict(entry) for path, entry in sorted(spec.files.items())}
    for path, entry in recorded.items():
        files[path] = dict(entry)
    return {
        "kind": "steam-depots",
        "source": "steam",
        "hashOrigin": "self-recorded",
        "app": spec.app,
        "branch": branch,
        "buildid": int(buildid) if buildid is not None else spec.buildid,
        "os": spec.os_,
        "arch": spec.arch,
        "depots": depots,
        "files": files,
        "credentials": spec.credentials,
    }


def _stale_files(
    spec: SteamInput, observed: dict[str, dict[str, Any]], recorded: dict[str, dict[str, Any]]
) -> list[str]:
    """Declared files whose recorded hash cannot survive this pin."""
    if not any(
        str(spec.depots.get(depot, {}).get("manifest", "")) != entry["manifest"] for depot, entry in observed.items()
    ):
        return []
    return sorted(path for path in spec.files if path not in recorded)


def _references(args: Any) -> int:
    catalog, target = select_one(args)
    del catalog
    spec = SteamInput.of(target.record)
    selectors = [str(selector) for selector in target.record["build"].get("references", [])]
    if not selectors:
        raise UsageError(f"target '{target.id}' declares no build.references to fetch")
    dest = Path(args.dest).expanduser().resolve()
    cache = paths.cache_dir()
    log = _log_path(cache, target.game, target.id)

    existing = _read_marker(dest)
    if existing is not None and existing.get("fingerprint") != target.fingerprint and not args.force:
        raise ConflictError(
            f"{dest} was built for fingerprint {existing.get('fingerprint', '?')[:16]}; "
            f"this target is {target.fp16} — stale reference cache. Pass --force to replace it.",
            dest=str(dest),
        )
    if existing is not None and existing.get("fingerprint") == target.fingerprint:
        problems = _verify_marker(dest, existing)
        if not problems:
            output.info(f"{dest} already holds the references for {target.id} ({target.fp16})")
            output.emit(
                "steam references",
                True,
                status="up-to-date",
                game=target.game,
                target=target.id,
                fingerprint=target.fingerprint,
                dest=str(dest),
                files=existing.get("files", []),
            )
            return OK
        raise IntegrityError(
            f"{dest} no longer matches its own references.json: " + "; ".join(problems),
            dest=str(dest),
        )

    dest.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="takaro-steam-refs-", dir=str(dest.parent)) as tmp:
        subset = Path(tmp) / "subset"
        for depot in sorted(spec.depots):
            dd.download(
                spec.app,
                depot,
                str(spec.depots[depot]["manifest"]),
                branch=spec.branch,
                os_=spec.os_,
                arch=spec.arch,
                dir=subset,
                cache=cache,
                log=log,
                filelist=selectors,
                validate=True,
                credentials=spec.credentials,
            )
        flat = Path(tmp) / "flat"
        flat.mkdir(parents=True, exist_ok=True)
        seen: dict[str, str] = {}
        for source in sorted(path for path in subset.rglob("*") if path.is_file()):
            relative = source.relative_to(subset).as_posix()
            if not selects(relative, selectors):
                continue
            if source.name in seen:
                raise ConflictError(
                    f"the reference subset holds two files called {source.name} "
                    f"({seen[source.name]} and {relative}); the build directory is flat"
                )
            seen[source.name] = relative
            digests = net.hash_file(source)
            expected = spec.files.get(relative)
            if expected and expected.get("sha256") and digests["sha256"] != expected["sha256"]:
                raise IntegrityError(f"{relative} hashes {digests['sha256']}, the target declares {expected['sha256']}")
            shutil.copy2(source, flat / source.name)
            rows.append(
                {
                    "path": source.name,
                    "from": relative,
                    "sha256": digests["sha256"],
                    "size": int(digests["size"]),
                }
            )
        if not rows:
            raise IntegrityError(f"the pinned manifests served no file matching {selectors}")
        shutil.rmtree(dest, ignore_errors=True)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(flat), str(dest))

    marker = dest / REFERENCES_MARKER
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "game": target.game,
                "target": target.id,
                "fingerprint": target.fingerprint,
                "fp16": target.fp16,
                "createdAt": dt.datetime.now(dt.UTC).isoformat().replace("+00:00", "Z"),
                "files": rows,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    output.emit(
        "steam references",
        True,
        status="fetched",
        game=target.game,
        target=target.id,
        fingerprint=target.fingerprint,
        dest=str(dest),
        files=rows,
    )
    return OK


def _read_marker(dest: Path) -> dict[str, Any] | None:
    marker = dest / REFERENCES_MARKER
    if not marker.is_file():
        return None
    try:
        return json.loads(marker.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ConflictError(f"{marker} is not valid JSON ({exc}); delete the directory and fetch again") from exc


def _verify_marker(dest: Path, marker: dict[str, Any]) -> list[str]:
    problems: list[str] = []
    for row in marker.get("files", []):
        path = dest / str(row["path"])
        if not path.is_file():
            problems.append(f"{row['path']} is missing")
            continue
        digests = net.hash_file(path)
        if digests["sha256"] != row["sha256"]:
            problems.append(f"{row['path']} sha256 {digests['sha256']} != recorded {row['sha256']}")
    return problems
