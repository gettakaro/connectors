"""``icons`` commands: build Icon Packs for Takaro's item icons, publish them and control them."""

from __future__ import annotations

import argparse
from pathlib import Path

from .. import output
from ..catalog import load as load_catalog
from ..exit_codes import OK, UsageError
from ..icons import build, catalog_route, controls, pipeline, promote, publish


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("icons", help="build and publish Icon Packs")
    inner = parser.add_subparsers(dest="icons_command", metavar="<subcommand>")

    inputs_parser = inner.add_parser("inputs", help="list the inputs a game's catalog-driven build needs (no network)")
    _add_route_selection(inputs_parser)
    inputs_parser.set_defaults(handler=_inputs, op="icons inputs")

    build_parser = inner.add_parser("build", help="build a pack and its reports from a recipe document or the catalog")
    build_parser.add_argument("--recipes", help="recipe document (JSON); excludes --game")
    _add_route_selection(build_parser, required=False)
    build_parser.add_argument("--client", help="installed game client (or a mirror of its files); needs --game")
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


def _add_route_selection(parser: argparse.ArgumentParser, *, required: bool = True) -> None:
    parser.add_argument("--game", required=required, help="catalog game id, e.g. zomboid")
    parser.add_argument("--target", default=None, help="target id; beats the declared default")
    parser.add_argument("--platform", default=None, help="restrict the default lookup to one platform")


def _add_target(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--env", required=True, help="'staging' or 'production'")
    parser.add_argument("--game", required=True, help="game identifier")
    parser.add_argument("--dry-run", action="store_true", help="plan the change without writing anything")


def _inputs(args: argparse.Namespace) -> int:
    route = catalog_route.route_for(load_catalog(), args.game, args.target, args.platform)
    output.emit("icons inputs", True, **catalog_route.inputs_document(route))
    return OK


def _build(args: argparse.Namespace) -> int:
    if (args.recipes is None) == (args.game is None):
        raise UsageError("icons build takes exactly one of --recipes FILE or --game GAME")
    if args.recipes is not None:
        if args.client or args.target or args.platform:
            raise UsageError("--client, --target and --platform go with --game, not --recipes")
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
    client = Path(args.client) if args.client else None
    report = pipeline.build_from_catalog(args.game, args.target, args.platform, client, Path(args.out))
    output.emit("icons build", True, **report)
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
