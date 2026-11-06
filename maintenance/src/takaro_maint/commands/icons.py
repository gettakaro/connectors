"""``icons`` commands: build Icon Packs for Takaro's item icons, publish them and control them."""

from __future__ import annotations

import argparse
from pathlib import Path

from .. import output
from ..exit_codes import OK
from ..icons import build, controls, promote, publish


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("icons", help="build and publish Icon Packs")
    inner = parser.add_subparsers(dest="icons_command", metavar="<subcommand>")

    build_parser = inner.add_parser("build", help="render a recipe document into a pack and its reports")
    build_parser.add_argument("--recipes", required=True, help="recipe document (JSON)")
    build_parser.add_argument("--out", required=True, help="output directory; must be absent or empty")
    build_parser.set_defaults(handler=_build, op="icons build")

    publish_parser = inner.add_parser("publish", help="merge a pack into the live bucket and upload it")
    publish_parser.add_argument("--env", required=True, help="target environment (only 'staging' is accepted)")
    publish_parser.add_argument("--pack", required=True, help="pack directory, named for its game (pack/<game>)")
    publish_parser.add_argument("--dry-run", action="store_true", help="plan the upload without writing anything")
    publish_parser.set_defaults(handler=_publish, op="icons publish")

    promote_parser = inner.add_parser("promote", help="copy a game's pack from staging to production")
    promote_parser.add_argument("--game", required=True, help="game identifier")
    promote_parser.add_argument("--dry-run", action="store_true", help="plan the promotion without writing anything")
    promote_parser.set_defaults(handler=_promote, op="icons promote")

    enabled_parser = inner.add_parser("set-enabled", help="flip a game's enabled flag without touching its icons")
    _add_target(enabled_parser)
    enabled_parser.add_argument("--enabled", required=True, help="'true' or 'false'")
    enabled_parser.set_defaults(handler=_set_enabled, op="icons set-enabled")

    takedown_parser = inner.add_parser("takedown", help="remove codes or objects from a game's pack")
    _add_target(takedown_parser)
    takedown_parser.add_argument("--code", nargs="+", action="extend", default=[], help="item codes to remove")
    takedown_parser.add_argument(
        "--object", nargs="+", action="extend", default=[], help="<sha256>.webp objects to delete, with their codes"
    )
    takedown_parser.set_defaults(handler=_takedown, op="icons takedown")
    parser.set_defaults(handler=None, op="icons")


def _add_target(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--env", required=True, help="'staging' or 'production'")
    parser.add_argument("--game", required=True, help="game identifier")
    parser.add_argument("--dry-run", action="store_true", help="plan the change without writing anything")


def _build(args: argparse.Namespace) -> int:
    result = build.build_from_file(Path(args.recipes), Path(args.out))
    output.emit(
        "icons build",
        True,
        game=result.game,
        pack=str(result.pack_dir.resolve()),
        index=str(result.index_path.resolve()),
        codes=result.codes,
        objects=result.objects,
        coverage=result.coverage,
        reports={name: str(path.resolve()) for name, path in result.reports.items()},
        encoder=result.encoder,
    )
    return OK


def _publish(args: argparse.Namespace) -> int:
    report = publish.publish_pack(args.env, Path(args.pack), dry_run=args.dry_run)
    output.emit("icons publish", True, **report)
    return OK


def _promote(args: argparse.Namespace) -> int:
    output.emit("icons promote", True, **promote.promote_pack(args.game, dry_run=args.dry_run))
    return OK


def _set_enabled(args: argparse.Namespace) -> int:
    report = controls.set_enabled(args.env, args.game, args.enabled, dry_run=args.dry_run)
    output.emit("icons set-enabled", True, **report)
    return OK


def _takedown(args: argparse.Namespace) -> int:
    report = controls.takedown(args.env, args.game, args.code, args.object, dry_run=args.dry_run)
    output.emit("icons takedown", True, **report)
    return OK
