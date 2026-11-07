"""The catalog invariants. Each one is a named check with its own test."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .. import net, output, paths
from ..exit_codes import INTEGRITY, USAGE, MaintError, UsageError
from ..icons import contract as icon_contract
from ..icons import mappers as icon_mappers
from . import ids, schema
from .loader import Catalog, Target

FLOATING_WORDS = ("latest", "stable", "beta", "SNAPSHOT")


@dataclass
class Check:
    id: str
    status: str
    detail: str
    file: str

    def as_dict(self) -> dict[str, str]:
        return {"id": self.id, "status": self.status, "detail": self.detail, "file": self.file}


@dataclass
class ValidationResult:
    checks: list[Check] = field(default_factory=list)
    exit_code: int = 0

    def add(self, check_id: str, ok: bool, detail: str, file: str, *, code: int = USAGE) -> None:
        self.checks.append(Check(check_id, "pass" if ok else "fail", detail, file))
        if not ok and self.exit_code == 0:
            self.exit_code = code

    @property
    def ok(self) -> bool:
        return self.exit_code == 0

    def failures(self) -> list[Check]:
        return [c for c in self.checks if c.status == "fail"]


def _relative(path: Path) -> str:
    try:
        return path.relative_to(paths.repo_root()).as_posix()
    except ValueError:
        return path.as_posix()


def _strings_under(value: Any) -> list[str]:
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        return [s for v in value.values() for s in _strings_under(v)]
    if isinstance(value, list):
        return [s for v in value for s in _strings_under(v)]
    return []


def _check_game(result: ValidationResult, catalog: Catalog) -> None:
    for game in catalog.games.values():
        file = _relative(game.path)
        errors = schema.errors_for("game.schema.json", game.record)
        result.add("game-schema", not errors, "; ".join(errors) or "valid against game.schema.json", file)


def _check_legacy_aliases(result: ValidationResult, catalog: Catalog) -> None:
    """An asset name without the game build in it may only exist while it cannot be the wrong build.

    The 7D2D generic zip was the V 3.2 build long after V 3.3 and V 3.4 targets shipped, and
    admins on the newer builds installed it and silently lost inventory. So an alias must point
    at a live target that is the only non-retired target on its platform; as soon as a second
    build appears, the alias has to go.
    """
    for game_id, game in sorted(catalog.games.items()):
        file = _relative(game.path)
        active: dict[str, list[str]] = {}
        known: dict[str, Target] = {}
        for candidate in catalog.all_targets():
            if candidate.game != game_id:
                continue
            known[candidate.id] = candidate
            if candidate.status != "retired":
                active.setdefault(candidate.platform, []).append(candidate.id)
        for name, reference in sorted((game.record.get("legacyAssetAliases") or {}).items()):
            target_id, _, role = str(reference).partition("/")
            target = known.get(target_id)
            if target is None:
                # Publishing skips it (aliasesSkipped); the selector audit catches a typo in the
                # real catalog, and trimmed test catalogs drop targets on purpose.
                continue
            roles = [c.get("role") for c in target.record.get("components", [])]
            if role not in roles:
                result.add(
                    "legacy-alias-single-build",
                    False,
                    f"{name} points at {reference}, but {target_id} builds no '{role}' "
                    f"(roles: {roles}); remove the alias",
                    file,
                )
                continue
            if target.status == "retired":
                result.add(
                    "legacy-alias-single-build",
                    False,
                    f"{name} points at retired target {target_id}; remove the alias",
                    file,
                )
                continue
            siblings = sorted(active.get(target.platform, []))
            result.add(
                "legacy-alias-single-build",
                siblings == [target_id],
                f"{name} -> {reference}; active {target.platform} targets: {siblings}"
                + (
                    ""
                    if siblings == [target_id]
                    else " - a name without the game build hides which one it is; remove the alias"
                ),
                file,
            )


def _check_target_schema(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    errors = schema.errors_for("target.schema.json", target.record)
    result.add("target-schema", not errors, "; ".join(errors) or "valid against target.schema.json", file)


def _check_ids(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    expected = ids.expected_target_id(target.record)
    stem = target.path.stem
    ok = target.id == expected == stem
    result.add(
        "id-matches-stem",
        ok,
        f"id={target.id} expected={expected} stem={stem}",
        file,
    )


def _check_platform_declared(result: ValidationResult, catalog: Catalog, target: Target) -> None:
    file = _relative(target.path)
    platforms = catalog.game(target.game).record.get("platforms", [])
    ok = target.platform in platforms
    result.add(
        "platform-declared",
        ok,
        f"platform={target.platform} declared={platforms}",
        file,
    )


def _check_single_default(result: ValidationResult, catalog: Catalog) -> None:
    grouped: dict[tuple[str, str], list[Target]] = {}
    for target in catalog.all_targets():
        grouped.setdefault((target.game, target.platform), []).append(target)
    for (game_id, platform), targets in sorted(grouped.items()):
        defaults = [t.id for t in targets if t.is_default]
        file = _relative(targets[0].path.parent)
        result.add(
            "single-default",
            len(defaults) == 1,
            f"{game_id}/{platform} has {len(defaults)} default target(s): {defaults or '[]'}",
            file,
        )


def _check_default_platform(result: ValidationResult, catalog: Catalog) -> None:
    """``defaultPlatform`` is what ``--game`` alone resolves to, so it has to name a real platform."""
    for game_id, game in sorted(catalog.games.items()):
        declared = game.default_platform
        file = _relative(game.path)
        if declared is None:
            defaults = {t.platform for t in game.targets if t.is_default}
            result.add(
                "default-platform",
                len(defaults) <= 1,
                f"{game_id} declares default targets on {sorted(defaults)} and no defaultPlatform",
                file,
            )
            continue
        result.add(
            "default-platform",
            declared in game.platforms,
            f"{game_id} defaultPlatform={declared} platforms={game.platforms}",
            file,
        )


def _check_input_kinds(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    for name, spec in target.record.get("inputs", {}).items():
        kind = spec.get("kind")
        if not isinstance(kind, str):
            result.add("input-kind-schema", False, f"input '{name}' has no kind", file)
            continue
        schema_name = f"inputs/{kind}.schema.json"
        if not schema.has_schema(schema_name):
            # The target schema does not enumerate input kinds, so this is the only gate on
            # them: an unknown kind must fail here rather than pass unchecked.
            result.add(
                "input-kind-schema",
                False,
                f"input '{name}' has unknown kind '{kind}': no catalog/schema/v1/{schema_name}",
                file,
            )
            continue
        errors = schema.errors_for(schema_name, spec)
        result.add(
            "input-kind-schema",
            not errors,
            f"input '{name}' ({kind}): " + ("; ".join(errors) or "valid"),
            file,
        )


def _check_build_system(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    system = target.record.get("build", {}).get("system")
    if not isinstance(system, str):
        result.add("build-system-schema", False, "the build declares no system", file)
        return
    schema_name = f"build-systems/{system}.schema.json"
    if not schema.has_schema(schema_name):
        # The target schema does not enumerate the build systems, so this is the only
        # gate on them: an unknown system must fail here rather than pass unchecked.
        result.add(
            "build-system-schema",
            False,
            f"unknown build system '{system}': no catalog/schema/v1/{schema_name}",
            file,
        )
        return
    errors = schema.errors_for(schema_name, target.record["build"])
    result.add("build-system-schema", not errors, f"build ({system}): " + ("; ".join(errors) or "valid"), file)


def _check_no_null_hash(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    if target.status == "retired":
        result.add("no-null-hash", True, "retired target; hashes not required", file)
        return
    missing: list[str] = []
    for name, spec in target.record.get("inputs", {}).items():
        if spec["kind"] == "mojang-version":
            continue
        files = spec.get("files")
        if isinstance(files, dict):
            # An input that pins a set of files has no hash of its own; every file it
            # declares carries one instead.
            missing += [
                f"inputs.{name}.files.{path}.sha256" for path, entry in sorted(files.items()) if not entry.get("sha256")
            ]
            continue
        if not spec.get("sha256"):
            missing.append(f"inputs.{name}.sha256")
    for name, dep in target.record.get("build", {}).get("deps", {}).items():
        if name == "minecraft":
            continue  # resolved by Loom from the pinned game version, never downloaded directly
        if not dep.get("sha256"):
            missing.append(f"build.deps.{name}.sha256")
    result.add(
        "no-null-hash",
        not missing,
        f"unhashed: {missing}" if missing else "every required hash is recorded",
        file,
    )


def _check_immutable_images(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    images = {"runtime.container": target.record["runtime"]["container"]}
    toolchain = target.record["build"].get("toolchain")
    if toolchain is not None:
        images["build.toolchain"] = toolchain
    for where, image in images.items():
        errors = schema.errors_for("inputs/container-image.schema.json", image)
        result.add(
            "immutable-tag-and-digest",
            not errors,
            f"{where} {image.get('image')}:{image.get('tag')}: " + ("; ".join(errors) or "immutable tag + digest"),
            file,
        )


def _check_no_floating_words(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    offenders: list[str] = []
    for section in ("inputs", "runtime", "build"):
        block = target.record.get(section, {})
        if section == "build":
            block = {k: v for k, v in block.items() if k != "deps"}
            for name, dep in target.record["build"].get("deps", {}).items():
                if "resolvedCoordinate" in dep:
                    continue
                for text in _strings_under(dep):
                    offenders += [f"build.deps.{name}:{text}" for w in FLOATING_WORDS if w in text]
        for text in _strings_under(block):
            offenders += [f"{section}:{text}" for w in FLOATING_WORDS if w in text]
    result.add(
        "no-floating-words",
        not offenders,
        f"floating version words found: {sorted(set(offenders))}" if offenders else "no floating version words",
        file,
    )


def _check_java_chain(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    game_input = target.record["inputs"].get("game")
    if not game_input or game_input.get("kind") != "mojang-version":
        result.add("minecraft-java-chain", True, "not a Mojang-based target", file)
        return
    java_major = game_input["javaMajor"]
    runtime_java = target.record["runtime"]["java"]
    java_release = target.record["build"]["javaRelease"]
    gradle_jvm = target.record["build"]["gradleJvm"]
    ok = java_major == runtime_java == java_release and java_release <= gradle_jvm
    result.add(
        "minecraft-java-chain",
        ok,
        f"javaMajor={java_major} runtime.java={runtime_java} javaRelease={java_release} gradleJvm={gradle_jvm}",
        file,
    )


def _check_plugins_match_toml(result: ValidationResult, catalog: Catalog, target: Target) -> None:
    file = _relative(target.path)
    game = catalog.game(target.game)
    build = game.record.get("build", {})
    catalog_file = build.get("versionCatalog")
    if not catalog_file:
        result.add("plugins-match-toml", True, "game declares no version catalog", file)
        return
    toml_path = paths.repo_root() / build["projectDir"] / catalog_file
    if not toml_path.is_file():
        result.add("plugins-match-toml", False, f"missing {_relative(toml_path)}", file)
        return
    versions = tomllib.loads(toml_path.read_text(encoding="utf-8")).get("versions", {})
    mismatches = [
        f"{key}: catalog={value} toml={versions.get(key)}"
        for key, value in target.record["build"]["plugins"].items()
        if versions.get(key) != value
    ]
    result.add(
        "plugins-match-toml",
        not mismatches,
        f"mismatched against {_relative(toml_path)}: {mismatches}" if mismatches else "plugin versions agree",
        file,
    )


def _check_build_paths_exist(result: ValidationResult, catalog: Catalog, target: Target) -> None:
    """Whatever the build names in this repository has to be there.

    Which field names a path is the build system's business, so this reads the fields the
    record actually carries instead of branching on the system name.
    """
    file = _relative(target.path)
    build = target.record["build"]
    project = build.get("gradleProject")
    script = build.get("script")
    if target.status == "retired":
        check_id = "gradle-project-exists" if project is not None else "build-script-exists"
        result.add(check_id, True, "retired target; the build's paths are not required", file)
        return
    if project is not None:
        project_dir = catalog.game(target.game).record["build"]["projectDir"]
        build_file = paths.repo_root() / project_dir / "targets" / project / "build.gradle.kts"
        result.add(
            "gradle-project-exists",
            build_file.is_file(),
            f"{_relative(build_file)} " + ("exists" if build_file.is_file() else "is missing"),
            file,
        )
    if script is not None:
        try:
            relative = paths.safe_relative(str(script), field="build.script")
        except UsageError as exc:
            result.add("build-script-exists", False, exc.message, file)
        else:
            script_file = paths.repo_root() / relative
            result.add(
                "build-script-exists",
                script_file.is_file(),
                f"{_relative(script_file)} " + ("exists" if script_file.is_file() else "is missing"),
                file,
            )
    if project is None and script is None:
        result.add("build-script-exists", True, "the build names no path in this repository", file)


def _check_lockfile_pin(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    lockfile = target.record["build"].get("lockfile")
    if lockfile is None:
        result.add("lockfile-pinned", True, "the build declares no lockfile", file)
        return
    try:
        relative = paths.safe_relative(str(lockfile["path"]), field="build.lockfile.path")
    except UsageError as exc:
        result.add("lockfile-pinned", False, exc.message, file)
        return
    path = paths.repo_root() / relative
    if not path.is_file():
        result.add("lockfile-pinned", False, f"{_relative(path)} is missing", file)
        return
    expected = str(lockfile["sha256"])
    actual = net.sha256_file(path)
    result.add(
        "lockfile-pinned",
        actual == expected,
        f"{_relative(path)} expected {expected}, actual {actual}",
        file,
    )


def _check_deps_consistent(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    problems: list[str] = []
    deps = target.record["build"]["deps"]
    inputs = target.record["inputs"]
    api_input = inputs.get("fabricApi")
    api_dep = deps.get("fabric-api")
    if api_input and api_dep:
        if api_dep.get("sha256") != api_input.get("sha256"):
            problems.append("build.deps.fabric-api.sha256 != inputs.fabricApi.sha256")
        expected = f"{api_input['group']}:{api_input['artifact']}:{api_input['version']}"
        if api_dep.get("coordinate") != expected:
            problems.append(f"build.deps.fabric-api.coordinate != {expected}")
    loader_input = inputs.get("loader")
    loader_dep = deps.get("fabric-loader")
    if loader_input and loader_dep:
        expected = f"net.fabricmc:fabric-loader:{loader_input['loaderVersion']}"
        if loader_dep.get("coordinate") != expected:
            problems.append(f"build.deps.fabric-loader.coordinate != {expected}")
    game_input = inputs.get("game")
    mc_dep = deps.get("minecraft")
    if game_input and mc_dep:
        expected = f"com.mojang:minecraft:{game_input['version']}"
        if mc_dep.get("coordinate") != expected:
            problems.append(f"build.deps.minecraft.coordinate != {expected}")
    result.add(
        "deps-consistent",
        not problems,
        f"{problems}" if problems else "build deps agree with the pinned inputs",
        file,
    )


def _check_maven_path(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    problems: list[str] = []
    for name, spec in target.record["inputs"].items():
        if spec["kind"] != "maven-artifact":
            continue
        expected = ids.maven_path(spec["group"], spec["artifact"], spec["version"])
        if spec["path"] != expected:
            problems.append(f"inputs.{name}.path={spec['path']} expected={expected}")
    result.add(
        "maven-path-derivable",
        not problems,
        f"{problems}" if problems else "every maven path is derivable from its coordinate",
        file,
    )


def _check_launcher_path(result: ValidationResult, target: Target) -> None:
    file = _relative(target.path)
    problems: list[str] = []
    for name, spec in target.record["inputs"].items():
        if spec["kind"] != "fabric-launcher":
            continue
        expected = (
            f"/v2/versions/loader/{spec['gameVersion']}/{spec['loaderVersion']}/{spec['launcherVersion']}/server/jar"
        )
        if spec["path"] != expected:
            problems.append(f"inputs.{name}.path={spec['path']} expected={expected}")
    result.add(
        "launcher-path-derivable",
        not problems,
        f"{problems}" if problems else "every fabric launcher path is derivable",
        file,
    )


def _check_separate_names(result: ValidationResult, target: Target) -> None:
    """Every separately claimed proof names a check the verifier can actually run."""
    from ..verify.runner import check_ids

    file = _relative(target.path)
    declared = target.record.get("verification", {}).get("separate", [])
    unknown = sorted(set(declared) - set(check_ids(target.game)))
    result.add(
        "separate-names-checks",
        not unknown,
        f"unknown verification.separate entries: {unknown}" if unknown else "every separate entry names a check",
        file,
    )


def _check_icons_unreal(result: ValidationResult, block: dict[str, Any], file: str) -> None:
    """The decoder settings, the key variable and the IoStore file list of an ``icons`` block."""
    pixels = block.get("pixels") or {}
    uses_unreal = (block.get("data") or {}).get("decoder") == "unreal" or pixels.get("decoder") == "unreal"
    result.add(
        "icons-unreal-settings",
        uses_unreal == ("unreal" in block),
        "icons.unreal must be present exactly when data.decoder or pixels.decoder is 'unreal'",
        file,
    )
    key_env = (block.get("unreal") or {}).get("aesKeyEnv")
    result.add(
        "icons-aes-key-declared",
        key_env is None or key_env in (block.get("secrets") or []),
        f"icons.unreal.aesKeyEnv {key_env!r} must be listed in icons.secrets",
        file,
    )
    files = pixels.get("files") or []
    problems = []
    if pixels.get("access") == "iostore-ranges":
        problems = [name for name in files if not str(name).endswith(".utoc")]
        problems += [f"{name} (listed twice)" for name in sorted({n for n in files if files.count(n) > 1})]
    result.add(
        "icons-iostore-files",
        not problems,
        f"icons.pixels.files with access 'iostore-ranges' must be distinct .utoc paths: {problems}",
        file,
    )


def _check_icons(result: ValidationResult, catalog: Catalog) -> None:
    """The optional ``icons`` block: what it names must exist, without asking the network."""
    for game in sorted(catalog.games.values(), key=lambda game: game.id):
        block = game.record.get("icons")
        if block is None:
            continue
        file = _relative(game.path)
        pack_game = block.get("game")
        result.add(
            "icons-game-identifier",
            icon_contract.is_game_identifier(pack_game) and pack_game != "_store",
            f"icons.game {pack_game!r} must be a pack game identifier and not _store",
            file,
        )
        known = icon_mappers.names()
        result.add(
            "icons-mapper-known",
            block.get("mapper") in known,
            f"icons.mapper {block.get('mapper')!r}; registered mappers: {known}",
            file,
        )
        _check_icons_unreal(result, block, file)
        if not game.targets:
            # Trimmed test catalogs keep a game record and drop its targets; there is nothing to check against.
            continue
        data = block.get("data") or {}
        problems = []
        default_platform = game.record.get("defaultPlatform")
        for target in game.targets:
            # Only the default target of the default platform: the data files of a server depot are
            # platform-specific (Unreal's `-LinuxServer` containers), so another platform needs its own route.
            if not target.is_default or (
                data.get("decoder") == "unreal" and default_platform and target.platform != default_platform
            ):
                continue
            spec = target.record["inputs"].get(data.get("input"))
            if spec is None or spec.get("kind") != "steam-depots":
                problems.append(f"{target.id} has no steam-depots input named {data.get('input')!r}")
                continue
            absent = sorted(set(data.get("depots") or []) - set(spec["depots"]))
            if absent:
                problems.append(f"{target.id}: icons.data.depots {absent} are not depots of input {data['input']!r}")
        result.add(
            "icons-data-input",
            not problems,
            "; ".join(problems) or f"the default target has the steam-depots input {data.get('input')!r}",
            file,
        )
        revisions = {target.revision for target in game.targets}
        stray = sorted(set((block.get("pixels") or {}).get("clientBuilds") or {}) - revisions)
        result.add(
            "icons-client-builds-revisions",
            not stray,
            f"icons.pixels.clientBuilds names revisions no target has: {stray}" if stray else "every revision exists",
            file,
        )


def _guarded(result: ValidationResult, check_id: str, file: str, run: Any) -> None:
    """A malformed record must fail its check, not crash the validator."""
    try:
        run()
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        result.add(check_id, False, f"the record is too malformed to check: {exc!r}", file)


def validate_catalog(catalog: Catalog, *, online: bool = False, cache: Path | None = None) -> ValidationResult:
    """Run every invariant over the whole catalog tree."""
    result = ValidationResult()
    _guarded(result, "game-schema", "catalog", lambda: _check_game(result, catalog))
    _guarded(result, "single-default", "catalog", lambda: _check_single_default(result, catalog))
    _guarded(result, "default-platform", "catalog", lambda: _check_default_platform(result, catalog))
    _guarded(result, "legacy-alias-single-build", "catalog", lambda: _check_legacy_aliases(result, catalog))
    _guarded(result, "icons-block", "catalog", lambda: _check_icons(result, catalog))
    per_target = (
        ("target-schema", _check_target_schema),
        ("id-matches-stem", _check_ids),
        ("platform-declared", lambda r, t: _check_platform_declared(r, catalog, t)),
        ("input-kind-schema", _check_input_kinds),
        ("build-system-schema", _check_build_system),
        ("no-null-hash", _check_no_null_hash),
        ("immutable-tag-and-digest", _check_immutable_images),
        ("no-floating-words", _check_no_floating_words),
        ("minecraft-java-chain", _check_java_chain),
        ("plugins-match-toml", lambda r, t: _check_plugins_match_toml(r, catalog, t)),
        ("build-script-exists", lambda r, t: _check_build_paths_exist(r, catalog, t)),
        ("lockfile-pinned", _check_lockfile_pin),
        ("deps-consistent", _check_deps_consistent),
        ("maven-path-derivable", _check_maven_path),
        ("launcher-path-derivable", _check_launcher_path),
        ("separate-names-checks", _check_separate_names),
    )
    for target in catalog.all_targets():
        file = _relative(target.path)
        for check_id, check in per_target:
            _guarded(result, check_id, file, lambda check=check, target=target: check(result, target))
    if online:
        _guarded(result, "online", "catalog", lambda: _check_online(result, catalog, cache or paths.cache_dir()))
    return result


def _check_online(result: ValidationResult, catalog: Catalog, cache: Path) -> None:
    """Confirm every pinned hash still describes what upstream serves."""
    import json
    import tempfile

    for target in catalog.all_targets():
        file = _relative(target.path)
        if target.status == "retired":
            continue
        game = catalog.game(target.game).record
        with tempfile.TemporaryDirectory(prefix="takaro-maint-online-") as tmp:
            tmpdir = Path(tmp)
            for name, spec in target.record["inputs"].items():
                kind = spec["kind"]
                if kind == "mojang-version":
                    url = ids.resolved_url(game, spec["source"], spec["manifest"]["path"])
                    dest = tmpdir / f"{name}.json"
                    try:
                        net.fetch(url, dest, net.Expectation(sha1=spec["manifest"]["sha1"]), no_cache=True)
                    except MaintError as exc:
                        result.add("online-manifest", False, f"{name}: {exc.message}", file, code=exc.code)
                        continue
                    manifest = json.loads(dest.read_text(encoding="utf-8"))
                    server = manifest["downloads"]["server"]
                    problems = []
                    if server["sha1"] != spec["server"]["sha1"]:
                        problems.append(f"server sha1 upstream={server['sha1']} catalog={spec['server']['sha1']}")
                    if int(server["size"]) != int(spec["server"]["size"]):
                        problems.append(f"server size upstream={server['size']} catalog={spec['server']['size']}")
                    upstream_java = int(manifest["javaVersion"]["majorVersion"])
                    if upstream_java != int(spec["javaMajor"]):
                        problems.append(f"javaMajor upstream={upstream_java} catalog={spec['javaMajor']}")
                    result.add(
                        "online-manifest",
                        not problems,
                        f"{name}: " + ("; ".join(problems) if problems else f"{url} matches the record"),
                        file,
                        code=INTEGRITY,
                    )
                    continue
                fetchable = ids.input_url(game, spec)
                if fetchable is None or not fetchable.startswith(("http://", "https://")):
                    result.add(
                        "online-not-http",
                        True,
                        f"{name}: {kind} inputs are not fetched over HTTP; their bytes are checked on install",
                        file,
                    )
                    continue
                dest = tmpdir / f"{name}.bin"
                try:
                    net.fetch(fetchable, dest, net.Expectation(sha256=spec["sha256"]), no_cache=True)
                except MaintError as exc:
                    result.add("online-hash", False, f"{name}: {exc.message}", file, code=exc.code)
                    continue
                output.debug(f"{name}: {fetchable} still hashes to {spec['sha256']}")
                result.add("online-hash", True, f"{name}: {fetchable} still hashes to the recorded sha256", file)
