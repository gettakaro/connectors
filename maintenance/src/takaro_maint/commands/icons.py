"""``icons build`` and ``icons publish`` — Icon Packs for Takaro's item icons."""

from __future__ import annotations

import argparse
from pathlib import Path

from .. import output
from ..exit_codes import OK
from ..icons import build, publish


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
    parser.set_defaults(handler=None, op="icons")


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
