"""Decoder containers are reproducible by construction: pinned bases, locked packages, hash-checked downloads."""

from __future__ import annotations

import json
import re

from conftest import REPO_ROOT

DECODERS = REPO_ROOT / "maintenance" / "decoders"
DOCKERFILES = sorted(DECODERS.glob("*/Dockerfile"))
DOWNLOADERS = re.compile(r"\b(curl|wget|git clone|Invoke-WebRequest)\b")
HASH_CHECK = re.compile(r"sha256sum|sha256|shasum")


def _logical_lines(text: str) -> list[str]:
    return [line.strip() for line in text.replace("\\\n", " ").splitlines() if line.strip()]


def test_there_is_a_decoder_to_check() -> None:
    assert DOCKERFILES


def test_every_base_image_is_pinned_by_digest() -> None:
    for dockerfile in DOCKERFILES:
        froms = [line for line in _logical_lines(dockerfile.read_text()) if line.upper().startswith("FROM ")]
        assert froms, dockerfile
        for line in froms:
            assert "@sha256:" in line, f"{dockerfile}: {line}"


def test_packages_are_restored_from_the_committed_lock() -> None:
    for dockerfile in DOCKERFILES:
        directory = dockerfile.parent
        text = dockerfile.read_text()
        if list(directory.glob("*.csproj")):
            assert (directory / "packages.lock.json").is_file(), directory
            assert "dotnet restore --locked-mode" in text, dockerfile
            assert "packages.lock.json" in text


def test_python_packages_are_installed_from_a_hash_locked_file() -> None:
    locked = [dockerfile for dockerfile in DOCKERFILES if (dockerfile.parent / "requirements.lock").is_file()]
    assert locked
    for dockerfile in locked:
        text = dockerfile.read_text()
        for flag in ("--require-hashes", "--no-deps", "--only-binary=:all:", "requirements.lock"):
            assert flag in text, f"{dockerfile}: {flag}"
        requirements = [
            entry
            for entry in (dockerfile.parent / "requirements.lock").read_text().replace("\\\n", " ").splitlines()
            if entry.strip() and not entry.lstrip().startswith("#")
        ]
        assert requirements
        for entry in requirements:
            assert re.match(
                r"^[A-Za-z0-9_.-]+==[0-9][A-Za-z0-9_.!+-]*\s+(--hash=sha256:[0-9a-f]{64}\s*)+$", entry.strip()
            ), entry


def test_npm_packages_are_installed_from_the_integrity_locked_file() -> None:
    locked = [dockerfile for dockerfile in DOCKERFILES if (dockerfile.parent / "package.json").is_file()]
    assert locked
    for dockerfile in locked:
        text = dockerfile.read_text()
        for flag in ("npm ci", "--omit=dev", "--ignore-scripts", "package-lock.json"):
            assert flag in text, f"{dockerfile}: {flag}"
        package = json.loads((dockerfile.parent / "package.json").read_text())
        assert package["private"] is True and len(package["dependencies"]) == 1
        for name, version in package["dependencies"].items():
            assert re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?", version), f"{name}@{version} is not exact"
        lock = json.loads((dockerfile.parent / "package-lock.json").read_text())
        assert lock["lockfileVersion"] == 3
        entries = {name: entry for name, entry in lock["packages"].items() if name}
        assert entries
        for name, entry in entries.items():
            assert entry["resolved"].startswith("https://registry.npmjs.org/"), name
            assert re.fullmatch(r"sha512-[A-Za-z0-9+/]{86}==", entry["integrity"]), name


def test_nothing_is_downloaded_without_a_hash_check() -> None:
    for dockerfile in DOCKERFILES:
        for line in _logical_lines(dockerfile.read_text()):
            if line.upper().startswith("RUN ") and DOWNLOADERS.search(line):
                assert HASH_CHECK.search(line), f"{dockerfile}: {line}"


def test_the_oodle_library_is_pinned_by_hash() -> None:
    lock = json.loads((REPO_ROOT / "maintenance" / "tools.lock.json").read_text())["tools"]["oodle"]
    assert re.fullmatch(r"[0-9a-f]{64}", lock["sha256"])
    assert lock["size"] > 0 and lock["url"].startswith("https://")
    assert lock["hashOrigin"] == "self-recorded"


def test_build_artefacts_stay_out_of_the_image_context() -> None:
    ignored = (REPO_ROOT / "maintenance" / ".dockerignore").read_text().splitlines()
    assert "decoders/**/bin" in ignored and "decoders/**/obj" in ignored
    assert "decoders/**/node_modules" in ignored


def test_the_image_holds_no_oodle() -> None:
    for dockerfile in DOCKERFILES:
        assert "oo2core" not in dockerfile.read_text().lower().replace("liboo2corelinux64.so.9", "")
        assert not list(dockerfile.parent.rglob("*.so*"))
