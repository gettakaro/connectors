"""Audit every tracked entry point for floating selectors and every catalog game for registration.

The maintenance system only holds if nothing that installs, builds, deploys or runs a server picks
its input by a name that upstream can move. This module reads the repository the way a reviewer
would -- entry points only, prose and the tool's own sources excluded -- and fails on:

* a *floating selector*: ``:latest``, ``:stable``, ``-SNAPSHOT``, ``production_build``, a
  ``latest.download_url``, a ``beta`` NeoForge loader, a Modrinth project list or a bare
  ``app_update`` in a tracked entry point;
* a *rig image* that a dev-server compose file names literally instead of taking it from the
  resolved catalog target, and a *rig version* default someone typed by hand;
* a catalog game that is not registered end to end (sources, roles, targets, one default per
  platform, a verification policy, working legacy aliases and the shared release workflow), or
  a declared rig whose compose file does not exist.

Every knowingly-kept string is an :class:`Allow` with a reason naming a policy or a discovery, and
``test_every_allowlist_entry_still_matches_something`` deletes the list the moment it goes stale.
"""

from __future__ import annotations

import fnmatch
import json
import re
import subprocess
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
# These shipped target sets must not disappear when discovery-only games are added.
TARGETED_BASELINE = (
    "7d2d",
    "conan-exiles",
    "dune",
    "enshrouded",
    "minecraft",
    "rust",
    "terraria",
    "valheim",
    "zomboid",
)

#: The floating-selector grammar, plus the JSON spelling of a floating image tag.
SELECTOR = re.compile(
    r":latest\b|:stable\b|-SNAPSHOT|production_build|latest\.download_url"
    r"|NEOFORGE_VERSION: beta|MODRINTH_PROJECTS|app_update"
)

#: Tracked entry points: what installs, builds, deploys or runs a server. Docs, tests and the
#: tool's own sources are not entry points -- their strings explain or detect selectors rather
#: than select with them.
ENTRY_POINT_GLOBS = (
    ".github/workflows/*.yml",
    "dev-servers/compose/*.yml",
    "dev-servers/images/**",
    "dev-servers/lib/**",
    "dev-servers/scripts/**",
    "games/*/scripts/**",
    "games/*/docker-compose*.yml",
    "games/*/Dockerfile*",
    "games/*/server/**",
    "games/*/**/build.gradle.kts",
    "games/*/**/settings.gradle.kts",
    "games/*/mod/gradle/libs.versions.toml",
    "justfile",
    "maintenance/Dockerfile",
    "catalog/**/*.json",
)
EXCLUDED_GLOBS = ("**/*.md", "maintenance/tests/**", "games/*/tests/**", "maintenance/src/**", "maintenance/docs/**")

#: A line whose first non-blank characters are one of these is prose, not a selector.
COMMENT_PREFIXES = ("#", "//")

VERIFICATION_LEVELS = ("build", "contract", "startup", "protocol", "gameplay")


@dataclass(frozen=True)
class Allow:
    """One knowingly-kept selector."""

    path: str  # fnmatch glob, relative to the root; {a,b} alternatives are expanded
    pattern: str  # regex that must match the offending line
    reason: str  # one sentence; every entry names either a policy or a discovery id


ALLOWLIST: tuple[Allow, ...] = (
    Allow(
        "games/dragonwilds/**",
        r".",
        "Dragonwilds has discovery only; exact targets and rig migration remain a milestone-2 follow-up (D2)",
    ),
    Allow(
        "games/vein/**",
        r".",
        "VEIN has discovery only; exact targets and rig migration remain a milestone-2 follow-up (D2)",
    ),
    Allow(
        "dev-servers/compose/{dayz,dragonwilds,vein,palworld}.yml",
        r".",
        "rig without exact catalog targets (DayZ, Palworld, Dragonwilds, VEIN); not a maintained target",
    ),
    Allow(
        "dev-servers/images/{dayz,vein}/**",
        r".",
        "image for a rig without exact catalog targets; not a maintained target",
    ),
    Allow(
        "dev-servers/lib/common.sh",
        r"alpine:latest",
        "chown helper image, not a game input; pinning it belongs to the file's owner (D4)",
    ),
    Allow(
        "dev-servers/compose/minecraft.yml",
        r"image: itzg/minecraft-server:java21",
        "dead x-minecraft-common anchor: every service overrides image, so no container runs it (D3)",
    ),
    Allow(
        "dev-servers/compose/minecraft.yml",
        r'VERSION: "\$\{MINECRAFT_VERSION:-1\.21\.11\}"',
        "dead x-minecraft-common anchor: every service overrides VERSION, so no container runs it (D3)",
    ),
    Allow(
        "games/7d2d/scripts/test-contract.sh",
        r"-langversion:latest",
        "C# language-version flag of mcs, not an upstream selector",
    ),
    Allow(
        "games/{minecraft,zomboid}/mod/build.gradle.kts",
        r'"1\.0\.0-SNAPSHOT"',
        "the connector's own version fallback when version.txt is absent; not a game-coupled coordinate",
    ),
    Allow(
        "catalog/minecraft/targets/paper-1.21.11.json",
        r"paper-api:1\.21\.11-R0\.1-SNAPSHOT",
        "Paper's API coordinate is a snapshot by design; the record pins its resolvedCoordinate and sha256",
    ),
    Allow(
        "catalog/rust/game.json",
        r"production_build",
        "watched channel declared mutable: true, so the observation is pinned by the asset digest",
    ),
    Allow(
        "catalog/rust/targets/carbon-25353106.json",
        r"production_build",
        "the input pins sha256 and size; the tag is an address, not an identity (games/rust/DEVELOPMENT.md)",
    ),
    Allow(
        "catalog/rust/targets/carbon-25454815.json",
        r"production_build",
        "the input pins sha256 and size; the tag is an address, not an identity (games/rust/DEVELOPMENT.md)",
    ),
)


@dataclass(frozen=True)
class Hit:
    """One offending line."""

    path: str
    line_no: int
    text: str
    rule: str  # selector | rig-image | rig-version


def expand_braces(pattern: str) -> list[str]:
    """``a/{b,c}/d`` -> ``[a/b/d, a/c/d]``; fnmatch has no brace alternatives of its own."""
    match = re.search(r"\{([^{}]*)\}", pattern)
    if match is None:
        return [pattern]
    head, tail = pattern[: match.start()], pattern[match.end() :]
    expanded: list[str] = []
    for option in match.group(1).split(","):
        expanded.extend(expand_braces(head + option + tail))
    return expanded


def expanded_allowlist() -> list[Allow]:
    return [Allow(path, entry.pattern, entry.reason) for entry in ALLOWLIST for path in expand_braces(entry.path)]


def matches(relative: str, glob: str) -> bool:
    """Match a glob against the whole relative path; ``*`` deliberately crosses directories here."""
    if fnmatch.fnmatchcase(relative, glob):
        return True
    # A leading ``**/`` also has to match a file sitting at the root.
    return glob.startswith("**/") and fnmatch.fnmatchcase(relative, glob[3:])


def is_comment(line: str) -> bool:
    stripped = line.strip()
    return stripped.startswith(COMMENT_PREFIXES)


def tracked_files(root: Path) -> list[Path]:
    """Every tracked entry point under ``root``, in path order."""
    if (root / ".git").exists():  # a worktree's .git is a file, not a directory
        listing = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        candidates = [name for name in listing.split("\0") if name]
    else:
        candidates = [path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()]

    selected: list[Path] = []
    for relative in sorted(candidates):
        if any(matches(relative, glob) for glob in EXCLUDED_GLOBS):
            continue
        if not any(matches(relative, glob) for glob in ENTRY_POINT_GLOBS):
            continue
        path = root / relative
        if path.is_file():
            selected.append(path)
    return selected


_KEY = re.compile(r"^([A-Za-z0-9_.\-]+):(.*)$")
_VERSION_KEY = re.compile(r"^(?:[A-Z0-9]+(?:_[A-Z0-9]+)*_)?VERSION$")
_TYPED_DEFAULT = re.compile(r"\$\{[A-Z0-9_]+:-[0-9][0-9A-Za-z.+-]*\}")
_RESOLVED_IMAGE = re.compile(r'^"?\$\{')


@dataclass(frozen=True)
class _Entry:
    line_no: int
    block: str
    key: str
    value: str
    text: str


def _compose_entries(text: str) -> list[_Entry]:
    """Walk a compose file by indentation alone -- the tool has no YAML dependency and gains none.

    Tracks which top-level ``x-…`` anchor or ``services.<name>`` block each mapping key belongs to.
    """
    entries: list[_Entry] = []
    block = ""
    in_services = False
    for line_no, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip() or is_comment(raw):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        match = _KEY.match(raw.strip())
        if match is None:
            continue
        key, value = match.group(1), match.group(2).strip()
        if indent == 0:
            in_services = key == "services"
            block = "" if in_services else key
            continue
        if in_services and indent == 2:
            block = f"services.{key}"
            continue
        entries.append(_Entry(line_no, block, key, value, raw.strip()))
    return entries


def rig_compose_owners(root: Path) -> dict[str, str]:
    """``dev-servers/compose/<file>`` -> the catalog game whose ``devServers`` names it."""
    owners: dict[str, str] = {}
    for game_file in sorted((root / "catalog").glob("*/game.json")):
        try:
            record = json.loads(game_file.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        compose = (record.get("devServers") or {}).get("composeFile")
        if compose:
            owners[f"dev-servers/compose/{compose}"] = game_file.parent.name
    return owners


def _rig_hits(root: Path, paths: Iterable[Path]) -> list[Hit]:
    owners = rig_compose_owners(root)
    hits: list[Hit] = []
    for path in paths:
        relative = path.relative_to(root).as_posix()
        if relative not in owners:
            continue
        entries = _compose_entries(path.read_text(encoding="utf-8", errors="replace"))
        built = {entry.block for entry in entries if entry.key == "build"}
        for entry in entries:
            if entry.key == "image":
                resolved = (
                    not entry.value
                    or _RESOLVED_IMAGE.match(entry.value) is not None
                    or "@sha256:" in entry.value
                    or entry.block in built
                )
                if not resolved:
                    hits.append(Hit(relative, entry.line_no, entry.text, "rig-image"))
            elif _VERSION_KEY.match(entry.key) and _TYPED_DEFAULT.search(entry.value):
                hits.append(Hit(relative, entry.line_no, entry.text, "rig-version"))
    return hits


def audit(root: Path, files: Iterable[Path] | None = None) -> list[Hit]:
    """Every floating selector and unresolved rig input under ``root``, allowlist not applied."""
    paths = list(files) if files is not None else tracked_files(root)
    hits: list[Hit] = []
    for path in paths:
        relative = path.relative_to(root).as_posix()
        for line_no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            if is_comment(line):
                continue
            if SELECTOR.search(line):
                hits.append(Hit(relative, line_no, line.strip(), "selector"))
    hits.extend(_rig_hits(root, paths))
    return sorted(hits, key=lambda hit: (hit.path, hit.line_no, hit.rule))


def allowed(hit: Hit, allowlist: Iterable[Allow] | None = None) -> Allow | None:
    """The allowlist entry that keeps this hit, or ``None``."""
    for entry in expanded_allowlist() if allowlist is None else allowlist:
        if matches(hit.path, entry.path) and re.search(entry.pattern, hit.text):
            return entry
    return None


def explain(hits: Iterable[Hit]) -> str:
    return "\n".join(f"{hit.path}:{hit.line_no}: [{hit.rule}] {hit.text}" for hit in sorted(hits, key=_order))


def _order(hit: Hit) -> tuple[str, int, str]:
    return (hit.path, hit.line_no, hit.rule)


def _games() -> dict[str, dict]:
    return {
        path.parent.name: json.loads(path.read_text(encoding="utf-8"))
        for path in sorted((REPO_ROOT / "catalog").glob("*/game.json"))
        if any((path.parent / "targets").glob("*.json"))
    }


def _targets(game: str) -> list[dict]:
    return [
        json.loads(path.read_text(encoding="utf-8"))
        for path in sorted((REPO_ROOT / "catalog" / game / "targets").glob("*.json"))
    ]


# --------------------------------------------------------------------------------------------
# The audit itself
# --------------------------------------------------------------------------------------------


def test_the_audit_is_green_on_this_checkout() -> None:
    """No tracked entry point selects an input by a name upstream can move."""
    offenders = [hit for hit in audit(REPO_ROOT) if allowed(hit) is None]
    assert offenders == [], "floating selectors in tracked entry points:\n" + explain(offenders)


def test_every_allowlist_entry_still_matches_something() -> None:
    """An allowlist that outlives what it excused is a lie; this deletes it."""
    hits = audit(REPO_ROOT)
    stale = [
        entry
        for entry in expanded_allowlist()
        if not any(matches(hit.path, entry.path) and re.search(entry.pattern, hit.text) for hit in hits)
    ]
    assert stale == [], "allowlist entries that no longer match anything:\n" + "\n".join(
        f"{entry.path} ~ {entry.pattern} ({entry.reason})" for entry in stale
    )


def test_a_planted_floating_tag_is_reported(tmp_path: Path) -> None:
    """A floating tag planted in a rig and in a catalog record is named with its line."""
    compose = tmp_path / "dev-servers" / "compose" / "x.yml"
    compose.parent.mkdir(parents=True)
    compose.write_text("services:\n  x:\n    image: foo:latest\n", encoding="utf-8")
    record = tmp_path / "catalog" / "x" / "targets" / "t.json"
    record.parent.mkdir(parents=True)
    record.write_text('{"image": "foo:latest"}\n', encoding="utf-8")

    hits = audit(tmp_path)

    assert [(hit.path, hit.line_no, hit.rule) for hit in hits] == [
        ("catalog/x/targets/t.json", 1, "selector"),
        ("dev-servers/compose/x.yml", 3, "selector"),
    ], explain(hits)


def test_comment_lines_and_docs_are_not_selectors(tmp_path: Path) -> None:
    """Prose that mentions a selector is not a selector."""
    compose = tmp_path / "dev-servers" / "compose" / "x.yml"
    compose.parent.mkdir(parents=True)
    compose.write_text("services:\n  x:\n    # image: foo:latest\n    build: .\n", encoding="utf-8")
    (tmp_path / "README.md").write_text("image: foo:latest\n", encoding="utf-8")

    assert audit(tmp_path) == []


def test_an_undigested_rig_image_and_a_typed_version_default_are_reported(tmp_path: Path) -> None:
    """A rig takes its image and its game version from the resolved target, or it is reported."""
    (tmp_path / "catalog" / "g").mkdir(parents=True)
    (tmp_path / "catalog" / "g" / "game.json").write_text(
        json.dumps({"id": "g", "devServers": {"composeFile": "g.yml"}}), encoding="utf-8"
    )
    compose = tmp_path / "dev-servers" / "compose" / "g.yml"
    compose.parent.mkdir(parents=True)
    compose.write_text(
        "services:\n"
        "  server:\n"
        "    image: itzg/minecraft-server:java21\n"
        "    environment:\n"
        '      VERSION: "${MINECRAFT_VERSION:-1.21.11}"\n'
        "  sidecar:\n"
        "    build: ./sidecar\n"
        "    image: x:dev\n"
        "  resolved:\n"
        '    image: "${X_IMAGE:-target-not-resolved}"\n',
        encoding="utf-8",
    )

    hits = audit(tmp_path)

    assert [(hit.line_no, hit.rule) for hit in hits] == [(3, "rig-image"), (5, "rig-version")], explain(hits)


def test_the_catalog_rigs_take_their_images_from_the_resolved_target() -> None:
    """Only the known dead Minecraft anchor names an image and a version of its own (D3)."""
    rig_hits = [hit for hit in audit(REPO_ROOT) if hit.rule in ("rig-image", "rig-version")]

    # D3: dev-servers/compose/minecraft.yml's x-minecraft-common anchor still carries an image tag
    # and a hand-typed VERSION. Every service overrides both (${MC_*_IMAGE}, ${MC_*_VERSION:-}), so
    # no container ever runs them -- removing the two lines belongs to that file's owner, not here.
    anchor = [hit for hit in rig_hits if hit.path == "dev-servers/compose/minecraft.yml"]
    assert [(hit.line_no, hit.rule) for hit in anchor] == [(5, "rig-image"), (11, "rig-version")], explain(anchor)
    assert rig_hits == anchor, "a rig outside the known Minecraft anchor names its own image:\n" + explain(
        [hit for hit in rig_hits if hit not in anchor]
    )


# --------------------------------------------------------------------------------------------
# Registration
# --------------------------------------------------------------------------------------------


def test_every_catalog_game_registers_sources_roles_targets_and_declared_rigs() -> None:
    """Each connector is registered end to end; only the documented Enshrouded rig gap remains."""
    on_disk = {path.name for path in (REPO_ROOT / "catalog").iterdir() if path.is_dir() and path.name != "schema"}
    # Discovery-only games are covered by catalog check-maintenance. Audit every game
    # with exact targets dynamically, while protecting the existing rollout from target loss.
    assert set(TARGETED_BASELINE) <= on_disk
    assert set(TARGETED_BASELINE) <= _games().keys()
    missing_rigs: list[str] = []

    for game, record in _games().items():
        watched = [key for key, source in record["sources"].items() if source.get("watch")]
        assert watched, f"{game} watches no upstream source"
        assert record.get("componentRoles"), f"{game} declares no component role"

        targets = _targets(game)
        live = [target for target in targets if (target.get("support") or {}).get("status") != "retired"]
        assert live, f"{game} has no target that is not retired"

        for platform in record["platforms"]:
            defaults = [target for target in live if target["platform"] == platform and target.get("default")]
            assert len(defaults) == 1, f"{game}/{platform} has {len(defaults)} defaults, expected exactly one"

        for target in live:
            required = target["verification"]["required"]
            assert required in VERIFICATION_LEVELS, f"{game}/{target['id']} requires unknown level {required!r}"

        known = {f"{target['id']}/{role}" for target in targets for role in record["componentRoles"]}
        for alias, points_at in (record.get("legacyAssetAliases") or {}).items():
            assert points_at in known, f"{game} legacy alias {alias} points at unknown {points_at}"

        compose_name = (record.get("devServers") or {}).get("composeFile")
        if compose_name is None:
            missing_rigs.append(game)
        else:
            compose = REPO_ROOT / "dev-servers" / "compose" / compose_name
            assert compose.is_file(), f"{game} names a rig compose file that does not exist: {compose.name}"

    # Phase-2 follow-up F6 owns the missing Enshrouded dispatch. Keep that gap visible and make
    # any second missing rig fail instead of silently weakening the rollout audit.
    assert missing_rigs == ["enshrouded"]


def test_every_catalog_connector_releases_through_the_shared_workflow() -> None:
    """No connector has a release path of its own, and each is single-flight per tag."""
    for game, record in _games().items():
        connector = record["connector"]
        workflow = REPO_ROOT / ".github" / "workflows" / f"{connector}.yml"
        assert workflow.is_file(), f"{game} has no {connector}.yml workflow"
        text = workflow.read_text(encoding="utf-8")
        assert "uses: ./.github/workflows/connector-release.yml" in text, connector
        assert "group: " + connector + "-${{ github.ref }}-${{ inputs.tag || 'ci' }}" in text, connector


def test_the_scheduled_publisher_is_enabled_and_single_flight() -> None:
    """The six-hourly publisher is on, runs one at a time and is never cancelled mid-write."""
    schedule = (REPO_ROOT / "maintenance" / "config" / "schedule.yaml").read_text(encoding="utf-8").splitlines()
    assert "publish_schedule: enabled" in schedule
    assert 'cron: "17 */6 * * *"' in schedule

    workflow = (REPO_ROOT / ".github" / "workflows" / "maintenance.yml").read_text(encoding="utf-8")
    assert "cron: '17 */6 * * *'" in workflow
    assert "group: maintenance-publisher" in workflow
    assert "cancel-in-progress: false" in workflow

    match = re.search(r"grep -qxE '[^']+' maintenance/config/schedule\.yaml", workflow)
    assert match, "the workflow no longer gates scheduled publication on the tracked file"
    gate = subprocess.run(["bash", "-c", match.group(0)], cwd=REPO_ROOT, check=False)
    assert gate.returncode == 0, "the workflow's own gate does not open on this branch"


# --------------------------------------------------------------------------------------------
# Docs
# --------------------------------------------------------------------------------------------

ROLLOUT_DOCS = {
    "recovery.md": (
        "## What you see and what it means",
        "## The scheduled publisher",
        "## Running it by hand",
        "## Dashboard health",
        "## Pausing publication",
        "## Recovering a release",
        "## Recovering an installation",
        "## Issues a human touched",
        "## What is kept, and for how long",
    ),
    "support-policy.md": (
        "## Support states",
        "## What is maintained today",
        "## Upstream-only retention",
        "## Retiring a target",
        "## When upstream moves",
        "## Connectors outside the catalog",
    ),
    "example-source-to-release.md": (
        "## The example",
        "## 1. Observed",
        "## 2. Ready",
        "## 3. Implemented",
        "## 4. Awaiting release",
        "## 5. Released",
        "## 6. Closed",
        "## The same path for a Steam game",
    ),
}

#: A published document must carry no author's path, no identifier from a private system, and no
#: repository other than this one -- the trackers a rollout is proved against are private.
PRIVATE = re.compile(r"/home/|/srv/|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
FOREIGN_REPO = re.compile(r"github\.com/(?!gettakaro/connectors)[\w.-]+/[\w.-]+")


def test_the_docs_index_lists_the_rollout_documents() -> None:
    """The three rollout documents exist, are reachable from both READMEs and leak nothing."""
    index = (REPO_ROOT / "maintenance" / "README.md").read_text(encoding="utf-8")
    for name in ROLLOUT_DOCS:
        assert f"docs/{name}" in index, f"maintenance/README.md does not link docs/{name}"

    root = (REPO_ROOT / "README.md").read_text(encoding="utf-8")
    assert "## Maintenance" in root
    maintenance_section = root.split("## Maintenance", 1)[1].split("\n## ", 1)[0]
    assert "maintenance/README.md" in maintenance_section
    assert "catalog/README.md" in maintenance_section

    for name, headings in ROLLOUT_DOCS.items():
        path = REPO_ROOT / "maintenance" / "docs" / name
        assert path.is_file(), f"{name} is missing"
        text = path.read_text(encoding="utf-8")
        for heading in headings:
            assert heading in text, f"{name} has no {heading!r} section"
        leaked = [line for line in text.splitlines() if PRIVATE.search(line) or FOREIGN_REPO.search(line)]
        assert leaked == [], f"{name} leaks a private path, id or repository:\n" + "\n".join(leaked)
