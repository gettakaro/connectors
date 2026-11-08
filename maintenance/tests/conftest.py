"""Shared fixtures.

Every test drives the real command through ``cli.main`` (or a subprocess), so what is
asserted is observable behaviour — exit codes and the JSON on stdout — not helper shapes.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import zipfile
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from takaro_maint import net, paths, redact  # noqa: E402
from takaro_maint.cli import main  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures"


@pytest.fixture(autouse=True)
def _isolated_environment(tmp_path_factory: pytest.TempPathFactory, monkeypatch: pytest.MonkeyPatch) -> None:
    """No test may touch the developer's cache, repo root or credentials."""
    monkeypatch.setenv("TAKARO_MAINT_CACHE", str(tmp_path_factory.mktemp("cache")))
    monkeypatch.delenv("GH_TOKEN", raising=False)
    for name in list(os.environ):
        if name.startswith(("STG_R2_", "PROD_")) or name == "STG_ICON_HOST":
            monkeypatch.delenv(name)
    paths.set_repo_root(None)
    redact.forget()
    yield
    paths.set_repo_root(None)
    redact.forget()


_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}


class _LoopbackOnly:
    """Every byte a test downloads comes from a fake on this host, never from the internet."""

    def __init__(self, inner: net.Transport) -> None:
        self.inner = inner

    def open(self, url: str, headers: dict[str, str]) -> Any:
        if urlsplit(url).hostname not in _LOOPBACK_HOSTS:
            raise RuntimeError(f"a test reached outside its fake upstream: {url}")
        return self.inner.open(url, headers)


@pytest.fixture(autouse=True)
def _no_public_internet() -> Any:
    """A test that downloads from the real internet is a bug in the test, not a slow test."""
    previous = net.get_transport()
    net.set_transport(_LoopbackOnly(previous))
    yield
    net.set_transport(previous)


@pytest.fixture
def repo_root() -> Path:
    return REPO_ROOT


@pytest.fixture
def run(capsys: pytest.CaptureFixture[str]) -> Any:
    """Run the command and return ``(exit_code, stdout_json_or_text, stderr)``."""

    def _run(*argv: str, repo: Path | None = None) -> tuple[int, Any, str]:
        args = list(argv)
        if repo is not None:
            args = ["--repo-root", str(repo), *args]
        code = main(args)
        captured = capsys.readouterr()
        try:
            payload: Any = json.loads(captured.out)
        except json.JSONDecodeError:
            payload = captured.out
        return code, payload, captured.err

    return _run


@pytest.fixture
def catalog_copy(tmp_path: Path) -> Path:
    """A writable copy of the whole repository catalog, plus the files validation reads."""
    root = tmp_path / "repo"
    (root / "catalog").mkdir(parents=True)
    shutil.copytree(REPO_ROOT / "catalog", root / "catalog", dirs_exist_ok=True)
    for target_file in (root / "catalog").glob("*/targets/*.json"):
        target = json.loads(target_file.read_text(encoding="utf-8"))
        script = target.get("build", {}).get("script")
        if not script:
            continue
        source = REPO_ROOT / script
        if source.is_file():
            destination = root / script
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        lockfile = target.get("build", {}).get("lockfile", {}).get("path")
        if lockfile:
            source = REPO_ROOT / lockfile
            destination = root / lockfile
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
    for game_file in (root / "catalog").glob("*/game.json"):
        data = json.loads(game_file.read_text(encoding="utf-8")).get("icons", {}).get("data", {})
        for name in data.get("files", []) if data.get("source") == "connector" else []:
            destination = root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / name, destination)
    mod = root / "games" / "minecraft" / "mod"
    (mod / "gradle").mkdir(parents=True)
    shutil.copy2(REPO_ROOT / "games/minecraft/mod/gradle/libs.versions.toml", mod / "gradle")
    for target_file in (root / "catalog" / "minecraft" / "targets").glob("*.json"):
        project = json.loads(target_file.read_text())["build"]["gradleProject"]
        (mod / "targets" / project).mkdir(parents=True, exist_ok=True)
        (mod / "targets" / project / "build.gradle.kts").write_text('plugins { id("takaro.fabric-target") }\n')
    (root / "games" / "minecraft" / "README.md").write_text(
        "# Minecraft\n\n<!-- takaro-maint:targets:begin -->\n<!-- takaro-maint:targets:end -->\n"
    )
    return root


def read_target(root: Path, target_id: str = "fabric-26.2") -> dict[str, Any]:
    return json.loads((root / "catalog/minecraft/targets" / f"{target_id}.json").read_text())


def write_target(root: Path, record: dict[str, Any], target_id: str = "fabric-26.2") -> Path:
    path = root / "catalog/minecraft/targets" / f"{target_id}.json"
    path.write_text(json.dumps(record, indent=2) + "\n")
    return path


def point_at(root: Path, base_url: str) -> None:
    """Rewrite every source in the copied game record to a fake upstream."""
    game_file = root / "catalog/minecraft/game.json"
    game = json.loads(game_file.read_text())
    for source in game["sources"].values():
        source["baseUrl"] = base_url
    game_file.write_text(json.dumps(game, indent=2) + "\n")


@pytest.fixture
def fake_target_bytes(tmp_path: Path) -> dict[str, bytes]:
    """Tiny stand-ins for the three downloads a Fabric install performs."""
    return {
        "server": (FIXTURES / "upstream/mojang/26.2/server.jar").read_bytes(),
        "launcher": (FIXTURES / "upstream/fabric/26.2/launcher.jar").read_bytes(),
        "fabric-api": (FIXTURES / "upstream/fabric/26.2/fabric-api-0.160.0+26.2.jar").read_bytes(),
    }


def sha1(payload: bytes) -> str:
    return hashlib.sha1(payload).hexdigest()


def sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def make_jar(
    path: Path,
    *,
    target: str,
    fingerprint: str,
    revision: str,
    version: str = "0.1.1",
    source_revision: str = "deadbeef",
    stamp: bool = True,
    attributes: dict[str, str] | None = None,
) -> Path:
    """Build a jar that carries (or deliberately fails to carry) a target's identity."""
    manifest_attributes = {
        "Manifest-Version": "1.0",
        "Takaro-Target": target,
        "Takaro-Target-Fingerprint": fingerprint,
        "Takaro-Connector-Version": version,
        "Takaro-Source-Revision": source_revision,
        "Takaro-Game-Version": revision,
        "Takaro-Java-Release": "25",
    }
    manifest_attributes.update(attributes or {})
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(
            "META-INF/MANIFEST.MF",
            "".join(f"{key}: {value}\n" for key, value in manifest_attributes.items()) + "\n",
        )
        if stamp:
            archive.writestr(
                "META-INF/takaro-target.json",
                json.dumps(
                    {
                        "target": target,
                        "fingerprint": fingerprint,
                        "game": "minecraft",
                        "platform": "fabric",
                        "revision": revision,
                        "connectorVersion": version,
                        "sourceRevision": source_revision,
                    }
                ),
            )
        archive.writestr("io/takaro/minecraft/fabric/TakaroFabricMod.class", b"\xca\xfe\xba\xbe")
    return path


class Wired:
    """A catalog copy whose declared ``served`` targets point at a fake upstream."""

    def __init__(
        self,
        root: Path,
        upstream: Any,
        *,
        served: tuple[str, ...] = (),
        unserved: dict[str, str] | None = None,
    ) -> None:
        self.root = root
        self.upstream = upstream
        self.served = served
        self.unserved = unserved or {}

    def target(self, target_id: str = "fabric-26.2") -> dict[str, Any]:
        return read_target(self.root, target_id)

    def save(self, record: dict[str, Any], target_id: str = "fabric-26.2") -> None:
        write_target(self.root, record, target_id)


def _fabric_fixture_dirs(record: dict[str, Any]) -> tuple[list[Path], dict[str, Any]] | None:
    """The stand-in files a Fabric target needs, or ``None`` when this copy has none for it."""
    revision = record["revision"]
    mojang_dir = FIXTURES / "upstream/mojang" / revision
    fabric_dir = FIXTURES / "upstream/fabric" / revision
    loader_version = record["inputs"]["loader"]["loaderVersion"]
    api_name = Path(record["inputs"]["fabricApi"]["path"]).name
    needed = [
        mojang_dir / "server.jar",
        mojang_dir / f"{revision}.json",
        fabric_dir / "launcher.jar",
        fabric_dir / api_name,
        fabric_dir / f"fabric-loader-{loader_version}.jar",
    ]
    if not all(path.is_file() for path in needed):
        return None
    return needed, {"loader_version": loader_version}


def _repin_fabric(record: dict[str, Any], upstream: Any) -> bool:
    """Point one Fabric target's downloads at the fake upstream; False when it cannot be served."""
    found = _fabric_fixture_dirs(record)
    if found is None:
        return False
    needed, extra = found
    loader_version = extra["loader_version"]
    server_bytes, manifest_source, launcher_bytes, api_bytes, loader_bytes = (
        needed[0].read_bytes(),
        needed[1],
        needed[2].read_bytes(),
        needed[3].read_bytes(),
        needed[4].read_bytes(),
    )

    manifest = json.loads(manifest_source.read_text())
    manifest["downloads"]["server"] = {
        "sha1": sha1(server_bytes),
        "size": len(server_bytes),
        "url": upstream.base_url + record["inputs"]["game"]["server"]["path"],
    }
    manifest_bytes = json.dumps(manifest).encode("utf-8")

    revision = record["revision"]
    game = record["inputs"]["game"]
    game["manifest"]["path"] = f"/v1/packages/{sha1(manifest_bytes)}/{revision}.json"
    game["manifest"]["sha1"] = sha1(manifest_bytes)
    game["server"]["sha1"] = sha1(server_bytes)
    game["server"]["size"] = len(server_bytes)
    record["inputs"]["loader"]["sha256"] = sha256(launcher_bytes)
    record["inputs"]["fabricApi"]["sha256"] = sha256(api_bytes)
    record["build"]["deps"]["fabric-api"]["sha256"] = sha256(api_bytes)
    record["build"]["deps"]["fabric-loader"]["sha256"] = sha256(loader_bytes)

    upstream.add(game["manifest"]["path"], manifest_bytes)
    upstream.add(game["server"]["path"], server_bytes)
    upstream.add(record["inputs"]["loader"]["path"], launcher_bytes)
    upstream.add(record["inputs"]["fabricApi"]["path"], api_bytes)
    upstream.add(
        f"/net/fabricmc/fabric-loader/{loader_version}/fabric-loader-{loader_version}.jar",
        loader_bytes,
    )
    return True


# One re-pinner per platform, so a new platform is served by adding a function here and never
# by teaching the fixture below about it.
_REPINNERS: dict[str, Any] = {"fabric": _repin_fabric}


def drop_unbuildable_icons(root: Path) -> None:
    """Drop the icons block of a public-download route whose pinned target revisions are no longer in the copy."""
    revisions = {json.loads(path.read_text())["revision"] for path in (root / "catalog").glob("*/targets/*.json")}
    for game_file in (root / "catalog").glob("*/game.json"):
        game = json.loads(game_file.read_text())
        downloads = (game.get("icons") or {}).get("pixels", {}).get("downloads")
        if downloads is not None and not revisions & set(downloads):
            del game["icons"]
            game_file.write_text(json.dumps(game))


@pytest.fixture
def wired(catalog_copy: Path) -> Any:
    """Serve and re-pin targets with a fake upstream, declaring the unsupported boundary.

    ``catalog validate --online`` walks every target left in the copy. A target without a
    repinner is unlinked because a half-pinned target must not be served, and ``unserved``
    records exactly which coverage was excluded and why.
    """
    from fake_upstream import FakeUpstream

    target_files = sorted((catalog_copy / "catalog").glob("*/targets/*.json"))
    assert target_files, "the copied catalog has no targets"

    with FakeUpstream() as upstream:
        served: list[str] = []
        unserved: dict[str, str] = {}
        for target_file in target_files:
            record = json.loads(target_file.read_text())
            repinner = _REPINNERS.get(record["platform"])
            if repinner is not None and repinner(record, upstream):
                write_target(catalog_copy, record, record["id"])
                served.append(str(record["id"]))
                continue
            if repinner is None:
                unserved[str(record["id"])] = f"no repinner for platform '{record['platform']}'"
            else:
                unserved[str(record["id"])] = "the repinner has no fixture for this target"
            target_file.unlink()

        # An alias must name a live target (legacy-alias-single-build), so drop the ones whose
        # target this copy no longer has.
        for game_file in (catalog_copy / "catalog").glob("*/game.json"):
            game = json.loads(game_file.read_text())
            aliases = game.get("legacyAssetAliases") or {}
            kept = {name: ref for name, ref in aliases.items() if str(ref).partition("/")[0] in served}
            if kept != aliases:
                game["legacyAssetAliases"] = kept
                game_file.write_text(json.dumps(game))

        drop_unbuildable_icons(catalog_copy)

        assert served, "no target could be re-pinned at the fake upstream"
        point_at(catalog_copy, upstream.base_url)
        yield Wired(catalog_copy, upstream, served=tuple(sorted(served)), unserved=dict(sorted(unserved.items())))
