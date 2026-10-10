"""The exact Steam install: the pinned manifest, or a clear refusal.

Every test drives the real ``takaro-maint`` command against the DepotDownloader stand-in,
so what is asserted is the behaviour a maintainer and CI get: exit codes, the JSON on
stdout, the bytes on disk and the arguments the tool was actually called with.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import zipfile
from pathlib import Path
from typing import Any

import pytest

import fake_depotdownloader as fake
from fake_upstream import FakeUpstream
from takaro_maint import paths
from takaro_maint.exit_codes import MaintError
from takaro_maint.steam import depotdownloader as dd

TARGET = fake.TARGET_ID
DECLARED = "7DaysToDieServer_Data/Managed/Assembly-CSharp.dll"


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    return fake.make_repo(tmp_path)


@pytest.fixture
def dd_log(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    log = tmp_path / "depotdownloader-argv.jsonl"
    for key, value in fake.environment(tmp_path, log).items():
        monkeypatch.setenv(key, value)
    return log


def install(run: Any, repo: Path, dest: Path, *extra: str) -> tuple[int, Any, str]:
    return run("install", "--game", "7d2d", "--target", TARGET, "--dest", str(dest), *extra, repo=repo)


def tree_state(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def test_tool_lock_pins_depotdownloader_and_refuses_other_bytes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = fake.make_repo(tmp_path)
    paths.set_repo_root(root)
    monkeypatch.delenv("TAKARO_MAINT_DEPOTDOWNLOADER", raising=False)
    payload = tmp_path / "DepotDownloader-linux-x64.zip"
    with zipfile.ZipFile(payload, "w") as archive:
        archive.writestr("DepotDownloader", "#!/bin/sh\nexit 0\n")
        archive.writestr("LICENSE", "MIT")
    body = payload.read_bytes()

    with FakeUpstream() as upstream:
        upstream.add("/DepotDownloader.zip", body)
        lock_path = root / "maintenance" / "tools.lock.json"
        lock = json.loads(lock_path.read_text())
        lock["tools"]["depotdownloader"]["url"] = upstream.base_url + "/DepotDownloader.zip"
        lock["tools"]["depotdownloader"]["size"] = len(body)
        lock["tools"]["depotdownloader"]["sha256"] = "0" * 64
        lock_path.write_text(json.dumps(lock))

        with pytest.raises(MaintError) as refused:
            dd.ensure(tmp_path / "cache")
        assert refused.value.code == 5

        lock["tools"]["depotdownloader"]["sha256"] = hashlib.sha256(body).hexdigest()
        lock_path.write_text(json.dumps(lock))
        executable = dd.ensure(tmp_path / "cache")
        assert executable.is_file()
        assert executable.stat().st_mode & 0o111
        locked_bytes = executable.read_bytes()

        # Already in the cache is not the guarantee the lock makes: an executable that has
        # been edited since is fetched again rather than run.
        executable.write_bytes(b"#!/bin/sh\nexfiltrate\n")
        again = dd.ensure(tmp_path / "cache")

    assert again.read_bytes() == locked_bytes


def test_install_selects_the_declared_manifest_per_depot(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    code, payload, _ = install(run, repo, tmp_path / "ServerFiles")

    assert code == 0, payload
    assert payload["status"] == "installed"
    calls = fake.argv_log(dd_log)
    assert len(calls) == 1, calls
    argv = calls[0]
    for flag, value in (
        ("-depot", fake.DEPOT),
        ("-manifest", fake.PINNED_MANIFEST),
        ("-os", "linux"),
        ("-osarch", "64"),
    ):
        assert argv[argv.index(flag) + 1] == value, argv
    assert "-validate" in argv
    recorded = {row["path"]: row for row in payload["inputs"]}
    assert set(recorded) == set(fake.read_target(repo)["inputs"]["server"]["files"])
    assert all(row["sha256"] and row["size"] for row in recorded.values())
    ledger = json.loads((tmp_path / "ServerFiles" / ".takaro" / "installed-target.json").read_text())
    assert ledger["target"] == TARGET
    assert (tmp_path / "ServerFiles" / "DONT_REMOVE.txt").is_file()


def test_an_unavailable_manifest_exits_four_with_no_fallback(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("FAKE_DD_UNAVAILABLE", fake.PINNED_MANIFEST)
    dest = tmp_path / "ServerFiles"

    code, payload, _ = install(run, repo, dest)

    assert code == 4
    assert "not falling back to branch head" in payload["error"]
    assert not dest.exists()
    assert not list(tmp_path.glob("ServerFiles.staging-*"))
    for argv in fake.argv_log(dd_log):
        assert "-manifest" in argv, argv


def test_install_never_uses_the_branch_head(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    head = json.loads((fake.DEPOTS / "head.json").read_text())
    assert head[fake.DEPOT] != fake.PINNED_MANIFEST

    code, payload, _ = install(run, repo, tmp_path / "ServerFiles")

    assert code == 0, payload
    for argv in fake.argv_log(dd_log):
        assert argv[argv.index("-manifest") + 1] == fake.PINNED_MANIFEST
    installed = (tmp_path / "ServerFiles" / DECLARED).read_bytes()
    assert b"branch head" not in installed


def test_a_wrong_reference_hash_exits_five_and_preserves_the_existing_install(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    before = tree_state(dest)

    record = fake.read_target(repo)
    record["inputs"]["server"]["files"][DECLARED]["sha256"] = "0" * 64
    fake.write_target(repo, record)

    calls = len(fake.argv_log(dd_log))
    cached = paths.cache_dir() / "steam" / str(fake.APP) / fake.DEPOT / fake.PINNED_MANIFEST

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert "the existing install at" in payload["error"]
    assert tree_state(dest) == before
    assert not list(tmp_path.glob("ServerFiles.staging-*"))
    # A record that disagrees with the depot is not a corrupt cache: nothing was
    # re-downloaded and the 17 GB already on disk is still there.
    assert len(fake.argv_log(dd_log)) == calls
    assert (cached / ".takaro" / "complete.json").is_file()


def test_a_stale_depot_cache_is_rejected_by_hash_and_refetched_once(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    cache = paths.cache_dir()
    cached = cache / "steam" / str(fake.APP) / fake.DEPOT / fake.PINNED_MANIFEST / DECLARED
    cached.write_bytes(b"someone edited the cache")
    shutil.rmtree(dest)

    code, payload, stderr = install(run, repo, dest)

    assert code == 0, payload
    assert "corrupt depot cache" in stderr
    assert len(fake.argv_log(dd_log)) == 2

    # The same corruption again, this time in what the tool serves: nothing is installed.
    cached.write_bytes(b"someone edited the cache")
    monkeypatch.setenv("FAKE_DD_CORRUPT", DECLARED)
    shutil.rmtree(dest)
    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert not dest.exists()


def test_a_second_install_is_a_no_op(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    before = tree_state(dest)
    calls = len(fake.argv_log(dd_log))

    code, payload, _ = install(run, repo, dest)

    assert code == 0, payload
    assert payload["status"] == "already-installed"
    assert tree_state(dest) == before
    assert len(fake.argv_log(dd_log)) == calls


def test_preserve_survives_an_upgrade_and_previous_is_kept(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    (dest / "sdtdserver.xml").write_text("<rig-edited-config/>")
    (dest / "Takaro").mkdir(exist_ok=True)
    (dest / "Takaro" / "Config.xml").write_text("<Takaro><Url>ws://rig/</Url></Takaro>")
    (dest / "Mods" / "Takaro").mkdir(parents=True, exist_ok=True)
    (dest / "Mods" / "Takaro" / "Takaro.dll").write_bytes(b"deployed connector")
    old = tree_state(dest)

    # A new build of the same game: the fingerprint changes, the install is replaced.
    record = fake.read_target(repo)
    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)

    code, payload, _ = install(run, repo, dest)

    assert code == 0, payload
    assert payload["status"] == "installed"
    previous = Path(payload["previous"])
    assert previous.is_dir()
    assert tree_state(previous) == old
    assert (dest / "sdtdserver.xml").read_text() == "<rig-edited-config/>"
    assert (dest / "Takaro" / "Config.xml").read_text() == "<Takaro><Url>ws://rig/</Url></Takaro>"
    assert (dest / "Mods" / "Takaro" / "Takaro.dll").read_bytes() == b"deployed connector"
    ledger = json.loads((dest / ".takaro" / "installed-target.json").read_text())
    assert (
        ledger["fingerprint"]
        == payload["fingerprint"]
        != json.loads((previous / ".takaro" / "installed-target.json").read_text())["fingerprint"]
    )


def test_an_upgrade_keeps_the_dragonwilds_plugin_ban_list(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    # The Dragonwilds plugin writes bans.json next to the server binary, inside the depot tree.
    # Installing the next build with that game's preserve list must carry it over.
    dragonwilds = json.loads(
        (fake.REPO_ROOT / "catalog" / "dragonwilds" / "targets" / "linux-25808123.json").read_text()
    )
    record = fake.read_target(repo)
    record["preserve"] = dragonwilds["preserve"]
    fake.write_target(repo, record)
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    bans = dest / "RSDragonwilds" / "Binaries" / "Linux" / "takaro" / "bans.json"
    bans.parent.mkdir(parents=True)
    bans.write_text('{"version":1,"bans":[{"gameId":"000204f6f3a444a99a44c995d80ff2a7"}]}')

    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)
    code, payload, _ = install(run, repo, dest)

    assert code == 0, payload
    assert payload["status"] == "installed"
    assert "RSDragonwilds/Binaries/Linux/takaro" in payload["preserved"]
    assert bans.read_text() == '{"version":1,"bans":[{"gameId":"000204f6f3a444a99a44c995d80ff2a7"}]}'


def test_an_upgrade_survives_a_file_name_that_is_not_utf8(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    # A crashing process can leave a file named after raw heap bytes in the game tree; hashing the
    # tree for the swap guard must not fall over on it.
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    stray = os.fsencode(dest) + b"/\xf0\xaf\xd3\xa9r\x7f"
    with open(stray, "wb") as handle:
        handle.write(b"hooks: restored 13 vtable slots\n")
    record = fake.read_target(repo)
    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)

    code, payload, err = install(run, repo, dest)

    assert code == 0, (payload, err)
    assert payload["status"] == "installed"
    assert os.path.exists(os.fsencode(payload["previous"]) + b"/\xf0\xaf\xd3\xa9r\x7f")


def test_rollback_restores_previous(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    first = tree_state(dest)
    record = fake.read_target(repo)
    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)
    assert install(run, repo, dest)[0] == 0

    code, payload, _ = install(run, repo, dest, "--rollback")

    assert code == 0, payload
    assert payload["status"] == "rolled-back"
    assert tree_state(dest) == first
    assert Path(payload["previous"]).is_dir()


def test_rollback_without_a_previous_install_exits_seven(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0

    code, payload, _ = install(run, repo, dest, "--rollback")

    assert code == 7, payload
    assert "nothing to roll back to" in payload["error"]


def test_dry_run_writes_nothing(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    dest = tmp_path / "ServerFiles"

    code, payload, _ = install(run, repo, dest, "--dry-run")

    assert code == 0, payload
    assert payload["status"] == "dry-run"
    assert payload["depots"][fake.DEPOT]["manifest"] == fake.PINNED_MANIFEST
    assert not dest.exists()
    assert fake.argv_log(dd_log) == []


def test_wrong_fingerprint_binaries_are_rejected_by_ledger_check(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    assert run("ledger", "check", "--game", "7d2d", "--target", TARGET, "--dest", str(dest), repo=repo)[0] == 0

    (dest / DECLARED).write_bytes(b"a different build's assembly")

    code, payload, _ = run("ledger", "check", "--game", "7d2d", "--target", TARGET, "--dest", str(dest), repo=repo)

    assert code == 7, payload
    assert any(DECLARED in reason for reason in payload["reasons"])


def test_credentials_come_from_env_and_are_redacted(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    record = fake.read_target(repo)
    record["inputs"]["server"]["credentials"] = {"branchPasswordEnv": "SEVEN_DAYS_BRANCH_PASSWORD"}
    fake.write_target(repo, record)
    dest = tmp_path / "ServerFiles"

    code, payload, _ = install(run, repo, dest)

    assert code == 2, payload
    assert "SEVEN_DAYS_BRANCH_PASSWORD" in payload["error"]

    monkeypatch.setenv("SEVEN_DAYS_BRANCH_PASSWORD", "a-secret-branch-password")
    code, payload, stderr = install(run, repo, dest)

    assert code == 0, payload
    argv = fake.argv_log(dd_log)[-1]
    assert argv[argv.index("-branchpassword") + 1] == "a-secret-branch-password"
    log_text = (paths.cache_dir() / "steam" / "logs" / f"7d2d-{TARGET}.log").read_text()
    assert "a-secret-branch-password" not in log_text
    assert "a-secret-branch-password" not in stderr + json.dumps(payload)


def test_a_tool_that_quietly_serves_the_branch_head_is_refused(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A zero exit is not proof: what is installed is checked against what was pinned."""
    monkeypatch.setenv("FAKE_DD_IGNORE_MANIFEST", "1")
    dest = tmp_path / "ServerFiles"

    code, payload, _ = install(run, repo, dest)

    assert code == 4, payload
    assert "not falling back to branch head" in payload["error"]
    assert fake.HEAD_MANIFEST in payload["error"]
    assert not dest.exists()
    # Nothing was cached under the pinned manifest's key either.
    cached = paths.cache_dir() / "steam" / str(fake.APP) / fake.DEPOT / fake.PINNED_MANIFEST
    assert not (cached / ".takaro" / "complete.json").is_file()


def test_a_failed_swap_puts_the_previous_install_back(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The window between moving the old install aside and the new one in is not a gap."""
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    before = tree_state(dest)

    record = fake.read_target(repo)
    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)

    real_replace = os.replace

    def refuse_the_second_rename(src: Any, dst: Any, *args: Any, **kwargs: Any) -> None:
        if str(dst) == str(dest) and ".staging-" in str(src):
            raise OSError(28, "No space left on device")
        real_replace(src, dst, *args, **kwargs)

    monkeypatch.setattr(os, "replace", refuse_the_second_rename)

    code, payload, _ = install(run, repo, dest)

    assert code != 0, payload
    assert tree_state(dest) == before
    assert not list(tmp_path.glob("ServerFiles.staging-*"))


def test_an_unwritable_ledger_puts_the_previous_install_back(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An install that cannot say what it is never goes into service."""
    from takaro_maint.steam import install as steam_install

    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    before = tree_state(dest)
    was = json.loads((dest / ".takaro" / "installed-target.json").read_text())

    # A different manifest, so the tree that would go live is not merely a relabelling of
    # the one already installed.
    fake.repin(repo, manifest=fake.HEAD_MANIFEST)

    def refuse(*args: Any, **kwargs: Any) -> None:
        raise OSError(28, "No space left on device")

    monkeypatch.setattr(steam_install, "write_ledger", refuse)

    code, payload, _ = install(run, repo, dest)

    assert code != 0, payload
    assert tree_state(dest) == before
    assert json.loads((dest / ".takaro" / "installed-target.json").read_text()) == was


def test_rollback_refuses_a_previous_install_that_no_longer_matches_its_ledger(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    record = fake.read_target(repo)
    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)
    assert install(run, repo, dest)[0] == 0
    current = tree_state(dest)

    previous = dest.with_name(dest.name + ".previous")
    (previous / DECLARED).write_bytes(b"rot in the install we would roll back to")

    code, payload, _ = install(run, repo, dest, "--rollback")

    assert code == 5, payload
    assert "no longer matches its own ledger" in payload["error"]
    assert tree_state(dest) == current


def test_a_short_credential_is_still_kept_out_of_the_logs(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Redaction is not a guess here: the record named the variable the value came from."""
    record = fake.read_target(repo)
    record["inputs"]["server"]["credentials"] = {"branchPasswordEnv": "SEVEN_DAYS_BRANCH_PASSWORD"}
    fake.write_target(repo, record)
    monkeypatch.setenv("SEVEN_DAYS_BRANCH_PASSWORD", "hunt2")
    dest = tmp_path / "ServerFiles"

    code, payload, stderr = install(run, repo, dest)

    assert code == 0, payload
    argv = fake.argv_log(dd_log)[-1]
    assert argv[argv.index("-branchpassword") + 1] == "hunt2"
    log_text = (paths.cache_dir() / "steam" / "logs" / f"7d2d-{TARGET}.log").read_text()
    assert "hunt2" not in log_text
    assert "hunt2" not in stderr + json.dumps(payload)


def test_a_preserved_entry_that_is_a_symlink_is_refused(run: Any, repo: Path, dd_log: Path, tmp_path: Path) -> None:
    """A link where a preserved file used to be would copy the host file it points at.

    `preserve` carries entries by copying, so whatever a link resolves to is read and
    written into the new install as a real file. The operator's own secrets are not this
    game's server data, however the link came to be there.
    """
    secret = tmp_path / "host-secret"
    secret.write_text("the operator's private key", encoding="utf-8")

    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    config = dest / "sdtdserver.xml"
    config.unlink(missing_ok=True)
    config.symlink_to(secret)
    before = tree_state(dest)

    record = fake.read_target(repo)
    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert "symlink" in json.dumps(payload)
    # The host file's bytes are nowhere under the tree, and the install is as it was.
    for path in dest.rglob("*"):
        if path.is_file() and not path.is_symlink():
            assert b"the operator's private key" not in path.read_bytes(), path
    assert tree_state(dest) == before


def test_a_preserved_entry_reached_through_a_symlinked_directory_is_refused(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path
) -> None:
    outside = tmp_path / "elsewhere"
    outside.mkdir()
    (outside / "Config.xml").write_text("the operator's private key", encoding="utf-8")

    dest = tmp_path / "ServerFiles"
    assert install(run, repo, dest)[0] == 0
    (dest / "Takaro").mkdir(exist_ok=True)
    shutil.rmtree(dest / "Takaro")
    (dest / "Takaro").symlink_to(outside, target_is_directory=True)

    record = fake.read_target(repo)
    record["inputs"]["server"]["buildid"] = record["inputs"]["server"]["buildid"] + 1
    fake.write_target(repo, record)

    code, payload, _ = install(run, repo, dest)

    assert code == 5, payload
    assert "symlink" in json.dumps(payload)


def test_a_fresh_install_that_cannot_write_its_ledger_leaves_nothing_behind(
    run: Any, repo: Path, dd_log: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """With no previous install there is nothing to restore, so the tree has to go.

    The ledger write is inside the swap's protected window. On an upgrade a failure there
    puts the retired install back; on a *fresh* install it used to leave the new tree live
    with no ledger -- a directory full of game files that cannot say which target it holds,
    which every later run reads as an unknown install rather than as the failure it is.
    """
    from takaro_maint.steam import install as steam_install

    def refuse(*args: Any, **kwargs: Any) -> None:
        raise OSError(28, "No space left on device")

    monkeypatch.setattr(steam_install, "write_ledger", refuse)

    dest = tmp_path / "ServerFiles"
    code, payload, err = install(run, repo, dest)

    assert code != 0, payload
    assert "No space left on device" in f"{err}{json.dumps(payload)}"
    assert not dest.exists(), sorted(p.name for p in dest.iterdir())
    assert sorted(p.name for p in tmp_path.iterdir() if p.name.startswith("ServerFiles")) == []
