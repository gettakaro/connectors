"""A new connector must be discoverable before it can pass CI."""

from __future__ import annotations

import json
import shutil
import subprocess
import textwrap
from pathlib import Path
from typing import Any

import pytest

from conftest import REPO_ROOT


@pytest.fixture
def registered_catalog(catalog_copy: Path) -> Path:
    shutil.copy2(REPO_ROOT / "release-please-config.json", catalog_copy)
    for source in (REPO_ROOT / "games").glob("*/connector.json"):
        dest = catalog_copy / source.relative_to(REPO_ROOT)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
    return catalog_copy


def test_all_shipped_connectors_have_release_discovery(run: Any) -> None:
    code, payload, stderr = run("catalog", "check-maintenance")
    assert code == 0, (payload, stderr)


@pytest.mark.parametrize("registration", ["connector", "release"])
def test_a_new_connector_cannot_omit_maintenance(run: Any, registered_catalog: Path, registration: str) -> None:
    root = registered_catalog
    if registration == "connector":
        path = root / "games/new-game/connector.json"
        path.parent.mkdir()
        path.write_text("{}\n")
    else:
        path = root / "release-please-config.json"
        config = json.loads(path.read_text())
        config["packages"]["games/new-game"] = {"component": "new-game"}
        path.write_text(json.dumps(config))
    code, payload, _ = run("catalog", "check-maintenance", repo=root)
    assert code == 2
    assert any(
        c["id"] == "connector-maintenance-catalog" and c["file"] == "games/new-game" for c in payload["failures"]
    )


@pytest.mark.parametrize("defect", ["missing", "disabled", "framework", "provider", "component", "release-component"])
def test_an_unusable_watcher_fails_ci(run: Any, registered_catalog: Path, defect: str) -> None:
    path = registered_catalog / "catalog/dune/game.json"
    game = json.loads(path.read_text())
    source = game["sources"]["steam"]
    if defect == "missing":
        source.pop("watch")
    elif defect == "disabled":
        source["watch"]["channels"]["public"]["enabled"] = False
    elif defect == "framework":
        source["watch"]["kind"] = "framework"
    elif defect == "provider":
        source["provider"] = "unknown"
    elif defect == "component":
        source["watch"]["component"] = "wrong-game"
    else:
        game["connector"] = "wrong-release"
    path.write_text(json.dumps(game))
    code, payload, _ = run("catalog", "check-maintenance", repo=registered_catalog)
    assert code == 2
    assert payload["failures"]


@pytest.mark.parametrize("event", ["schedule", "push", "workflow_dispatch"])
@pytest.mark.parametrize("enabled", [True, False])
def test_automatic_runs_bootstrap_and_respect_the_publication_switch(tmp_path: Path, event: str, enabled: bool) -> None:
    workflow = (REPO_ROOT / ".github/workflows/maintenance.yml").read_text()
    script = textwrap.dedent(
        workflow.split("      - name: Decide what this run does\n", 1)[1]
        .split("        run: |\n", 1)[1]
        .split("\n      #", 1)[0]
    )
    config = tmp_path / "maintenance/config/schedule.yaml"
    config.parent.mkdir(parents=True)
    config.write_text(f"publish_schedule: {'enabled' if enabled else 'disabled'}\n")
    output = tmp_path / "output"
    result = subprocess.run(
        ["bash", "-eu", "-c", script],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        env={"EVENT": event, "MODE": "read-only", "GITHUB_OUTPUT": str(output), "PATH": "/usr/bin:/bin"},
    )
    assert result.returncode == 0, result.stderr
    values = dict(line.split("=", 1) for line in output.read_text().splitlines())
    if event == "workflow_dispatch":
        assert values == {"skip": "false", "mode": "read-only"}
    elif enabled:
        assert values == {"skip": "false", "mode": "publish", "bootstrap": "true"}
    else:
        assert values == {"skip": "true"}
