"""Require release discovery when a connector is added, independently of build targets."""

from __future__ import annotations

import json
from pathlib import Path

from ..channels import enabled_channels
from ..exit_codes import UsageError
from ..providers import provider_for
from ..providers.base import Provider
from . import schema
from .loader import load
from .validate import ValidationResult


def check_maintenance(root: Path) -> ValidationResult:
    result = ValidationResult()
    catalog = load(root / "catalog")
    packages = json.loads((root / "release-please-config.json").read_text())["packages"]
    # Either registration counts, so forgetting the release package cannot bypass this check.
    projects = {path.parent.relative_to(root).as_posix() for path in (root / "games").glob("*/connector.json")}
    projects.update(path for path in packages if path.startswith("games/"))
    for project in sorted(projects):
        matches = [
            g
            for g in catalog.games.values()
            if Path(g.record.get("build", {}).get("projectDir", "")).is_relative_to(project)
        ]
        result.add(
            "connector-maintenance-catalog",
            len(matches) == 1,
            f"{project}: expected one catalog game, found {len(matches)}; see maintenance/docs/adding-a-game.md",
            project,
        )
        if len(matches) != 1:
            continue
        game = matches[0]
        file = game.path.relative_to(root).as_posix()
        errors = schema.errors_for("game.schema.json", game.record)
        result.add("game-schema", not errors, "; ".join(errors) or "valid game record", file)
        if errors:
            continue
        component = packages.get(project, {}).get("component", Path(project).name)
        result.add(
            "connector-maintenance-component",
            game.record["connector"] == component,
            f"{project}: catalog connector must match release component {component}",
            file,
        )
        watches = []
        for source in game.record["sources"].values():
            watch = source.get("watch") or {}
            if watch.get("kind") != "game" or watch.get("component") != game.id:
                continue
            try:
                provider = provider_for(source["provider"])
                if type(provider).observe is Provider.observe or not enabled_channels(watch):
                    continue
            except (UsageError, AttributeError, TypeError):
                continue
            watches.append(watch)
        result.add(
            "connector-maintenance-watch",
            bool(watches),
            f"{project}: requires a game watcher for {game.id} with a supported provider and an enabled channel",
            file,
        )
    return result
