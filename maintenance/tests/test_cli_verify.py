"""`verify`: the fake Takaro speaks the real protocol, and the runner never opens a port."""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any

import pytest
import websockets

from conftest import REPO_ROOT
from fake_docker import artifacts_for, install_docker_stub, process_is_gone

PROTOCOL = json.loads((REPO_ROOT / "games/7d2d/tests/fixtures/generic-protocol.json").read_text())


# --------------------------------------------------------------------------- protocol


def function_fixture(name: str) -> dict[str, Any]:
    return next(entry for entry in PROTOCOL["functions"] if entry["name"] == name)


async def connector_client(url: str, answers: dict[str, Any], seen: list[dict[str, Any]]) -> None:
    """Play the connector side of the protocol against the fake."""
    async with websockets.connect(url) as socket:
        await socket.send(
            json.dumps(
                {
                    "type": "identify",
                    "payload": {"identityToken": "an-identity", "registrationToken": "a-registration-token"},
                }
            )
        )
        async for raw in socket:
            frame = json.loads(raw)
            seen.append(frame)
            if frame.get("type") == "identifyResponse":
                continue
            if frame.get("type") == "request":
                action = frame["payload"]["action"]
                await socket.send(
                    json.dumps(
                        {
                            "type": "response",
                            "requestId": frame["requestId"],
                            "payload": answers.get(action),
                        }
                    )
                )


def test_the_fake_speaks_the_pinned_wire_envelope(tmp_path: Path) -> None:
    from takaro_maint.verify.fake_takaro import FakeTakaro

    seen: list[dict[str, Any]] = []
    answers = {entry["name"]: entry["response"]["payload"] for entry in PROTOCOL["functions"]}
    results: dict[str, Any] = {}

    async def scenario() -> None:
        fake = FakeTakaro(host="127.0.0.1", log_path=tmp_path / "fake-takaro.log")
        await fake.start()
        client = asyncio.create_task(connector_client(fake.url, answers, seen))
        identify = await fake.wait_for_identify(10)
        results["identify"] = identify
        results["gameServerId"] = fake.game_server_id
        results["players"] = await fake.request("getPlayers", {})
        results["items"] = await fake.request("listItems", {})
        results["reachability"] = await fake.request("testReachability", {})
        results["ping"] = await fake.ping(timeout=5)
        client.cancel()
        await fake.stop()

    asyncio.run(scenario())

    assert set(results["identify"]) == {"identityToken", "registrationToken"}
    identify_response = next(frame for frame in seen if frame["type"] == "identifyResponse")
    assert identify_response["payload"]["status"] == "authenticated"
    assert identify_response["payload"]["gameServerId"] == results["gameServerId"]

    request = next(frame for frame in seen if frame.get("type") == "request")
    envelope = PROTOCOL["wireEnvelope"]["request"]
    assert set(request) == set(envelope)
    assert set(request["payload"]) == set(envelope["payload"])
    assert isinstance(request["payload"]["args"], str)
    assert json.loads(request["payload"]["args"]) == {}

    assert results["players"] == function_fixture("getPlayers")["response"]["payload"]
    assert results["items"] == function_fixture("listItems")["response"]["payload"]
    assert results["reachability"] == {"connectable": True}
    assert results["ping"] >= 0


def test_every_frame_is_logged_as_jsonl(tmp_path: Path) -> None:
    from takaro_maint.verify.fake_takaro import FakeTakaro

    log = tmp_path / "fake-takaro.log"

    async def scenario() -> None:
        fake = FakeTakaro(host="127.0.0.1", log_path=log)
        await fake.start()
        client = asyncio.create_task(connector_client(fake.url, {"getPlayers": []}, []))
        await fake.wait_for_identify(10)
        await fake.request("getPlayers", {})
        client.cancel()
        await fake.stop()

    asyncio.run(scenario())

    lines = [json.loads(line) for line in log.read_text().splitlines()]
    directions = [line["direction"] for line in lines]
    kinds = [line["frame"]["type"] for line in lines]
    assert directions[:2] == ["in", "out"]
    assert kinds[:2] == ["identify", "identifyResponse"]
    assert "request" in kinds and "response" in kinds


def test_game_events_are_recorded(tmp_path: Path) -> None:
    from takaro_maint.verify.fake_takaro import FakeTakaro

    event = PROTOCOL["events"][0]["message"]
    recorded: dict[str, Any] = {}

    async def scenario() -> None:
        fake = FakeTakaro(host="127.0.0.1", log_path=tmp_path / "log.jsonl")
        port = await fake.start()
        async with websockets.connect(f"ws://127.0.0.1:{port}/") as socket:
            await socket.send(json.dumps({"type": "identify", "payload": {}}))
            await socket.recv()
            await socket.send(json.dumps(event))
            await asyncio.sleep(0.2)
        recorded["events"] = list(fake.events)
        await fake.stop()

    asyncio.run(scenario())

    assert recorded["events"] == [event["payload"]]


# --------------------------------------------------------------------------- checks


def test_the_catalogue_check_rejects_registry_ids() -> None:
    from takaro_maint.verify.checks import _catalogue_problems

    problems, detail = _catalogue_problems(
        [{"code": "minecraft:diamond_sword", "name": "minecraft:diamond_sword"}],
        ("minecraft:diamond_sword", "Diamond Sword"),
    )

    assert any("name equals code" in problem for problem in problems)
    assert detail["count"] == 1


def test_the_catalogue_check_rejects_translation_keys() -> None:
    from takaro_maint.verify.checks import _catalogue_problems

    problems, _ = _catalogue_problems(
        [{"code": "minecraft:zombie", "name": "entity.minecraft.zombie"}], ("minecraft:zombie", "Zombie")
    )

    assert any("translation key" in problem for problem in problems)


def test_the_catalogue_check_accepts_display_names() -> None:
    from takaro_maint.verify.checks import _catalogue_problems

    problems, detail = _catalogue_problems(
        [
            {"code": "minecraft:diamond_sword", "name": "Diamond Sword"},
            {"code": "minecraft:stone", "name": "Stone"},
        ],
        ("minecraft:diamond_sword", "Diamond Sword"),
    )

    assert problems == []
    assert detail["spotCheck"]["actual"] == "Diamond Sword"


def test_an_empty_catalogue_fails() -> None:
    from takaro_maint.verify.checks import _catalogue_problems

    problems, _ = _catalogue_problems([], ("minecraft:diamond_sword", "Diamond Sword"))

    assert problems


def test_the_level_is_the_highest_fully_passed_class() -> None:
    from takaro_maint.verify.report import level_for

    all_pass = [
        {"id": name, "status": "pass"}
        for name in (
            "build",
            "startup",
            "connector-load",
            "identify",
            "heartbeat",
            "players",
            "catalog-items",
            "catalog-entities",
            "console",
            "shutdown",
        )
    ]
    assert level_for(all_pass) == "protocol"

    no_console = [row if row["id"] != "console" else {"id": "console", "status": "fail"} for row in all_pass]
    assert level_for(no_console) == "startup"

    no_startup = [row if row["id"] != "startup" else {"id": "startup", "status": "fail"} for row in all_pass]
    assert level_for(no_startup) == "contract"

    assert level_for([{"id": "build", "status": "fail"}]) == "none"


def test_an_unobserved_class_is_not_reported_as_reached() -> None:
    from takaro_maint.verify.report import level_for, outcome_for

    # `verify --checks players`: one check ran, everything the level names did not.
    partial = [
        {"id": name, "status": "skip" if name != "players" else "pass"}
        for name in ("build", "startup", "players", "console")
    ]
    assert level_for(partial) == "none"
    assert outcome_for(partial) == "pass"


def test_a_run_that_observed_nothing_is_not_a_pass() -> None:
    from takaro_maint.verify.report import level_for, outcome_for

    skipped = [{"id": name, "status": "skip"} for name in ("build", "startup", "console")]
    assert level_for(skipped) == "none"
    assert outcome_for(skipped) == "fail"
    assert outcome_for([{"id": "build", "status": "pass"}, {"id": "startup", "status": "fail"}]) == "fail"


# --------------------------------------------------------------------------- runner


@pytest.fixture
def docker_stub(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, request: pytest.FixtureRequest) -> Path:
    return install_docker_stub(tmp_path, monkeypatch, request)


def test_a_full_stubbed_run_reaches_protocol_level(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, repo_root: Path
) -> None:
    from jsonschema import Draft202012Validator

    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--target",
        "fabric-26.2",
        "--artifacts",
        str(artifacts),
        "--out",
        str(out),
        "--run-id",
        "t1",
        "--label",
        "tm.issue=149",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )

    assert code == 0, payload
    report = json.loads((out / "fabric-26.2" / "report.json").read_text())
    schema = json.loads((repo_root / "catalog/schema/v1/verify-report.schema.json").read_text())
    assert list(Draft202012Validator(schema).iter_errors(report)) == []
    assert report["level"] == "protocol"
    assert report["outcome"] == "pass"
    assert {check["id"] for check in report["checks"] if check["status"] == "pass"} >= {
        "build",
        "startup",
        "connector-load",
        "identify",
        "heartbeat",
        "players",
        "catalog-items",
        "catalog-entities",
        "console",
        "shutdown",
    }
    items = next(check for check in report["checks"] if check["id"] == "catalog-items")
    assert items["detail"]["spotCheck"] == {
        "code": "minecraft:diamond_sword",
        "expected": "Diamond Sword",
        "actual": "Diamond Sword",
    }


def test_verify_runs_install_and_deploy_against_the_catalog_it_was_asked_about(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The sub-commands resolve the target from ``--repo-root``, not from the real catalog.

    Without the root passed on, ``install`` resolved ``fabric-26.2`` from the repository
    catalog and downloaded the real 61 MB inputs; the fake upstream this test wired up was
    never asked for a byte.
    """
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"
    cache = Path(os.environ["TAKARO_MAINT_CACHE"])

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--target",
        "fabric-26.2",
        "--artifacts",
        str(artifacts),
        "--out",
        str(out),
        "--run-id",
        "t1",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )
    assert code == 0, payload

    installed = json.loads((out / "fabric-26.2" / "install.json").read_text())
    _, wired_target, _ = run("targets", "resolve", "--game", "minecraft", "--target", "fabric-26.2", repo=wired.root)
    _, real_target, _ = run("targets", "resolve", "--game", "minecraft", "--target", "fabric-26.2", repo=REPO_ROOT)
    assert installed["target"] == "fabric-26.2"
    assert installed["fingerprint"] == wired_target["fingerprint"]
    assert installed["fingerprint"] != real_target["fingerprint"]

    oversized = [path for path in cache.rglob("*") if path.is_file() and path.stat().st_size > 1024 * 1024]
    assert oversized == [], f"a real upstream download reached the test cache: {oversized}"


def test_the_container_publishes_no_host_ports(run: Any, wired: Any, tmp_path: Path, docker_stub: Path) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(tmp_path / "reports"),
        "--run-id",
        "t1",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )

    calls = [json.loads(line) for line in (docker_stub / "argv.jsonl").read_text().splitlines()]
    run_call = next(call for call in calls if call[:1] == ["run"])
    assert "-p" not in run_call
    assert "--publish" not in run_call
    assert "--publish-all" not in run_call


def test_the_container_carries_the_run_labels(run: Any, wired: Any, tmp_path: Path, docker_stub: Path) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(tmp_path / "reports"),
        "--run-id",
        "t1",
        "--label",
        "tm.issue=149",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )

    calls = [json.loads(line) for line in (docker_stub / "argv.jsonl").read_text().splitlines()]
    run_call = next(call for call in calls if call[:1] == ["run"])
    labels = [run_call[i + 1] for i, item in enumerate(run_call) if item == "--label"]
    assert "tm.run=t1" in labels
    assert "tm.issue=149" in labels
    assert any(label.startswith("tm.ttl=") for label in labels)


@pytest.mark.parametrize("label", ["tm.run=p1", "tm.ttl=0"])
def test_a_label_the_harness_owns_is_refused_before_anything_starts(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, label: str
) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(tmp_path / "reports"),
        "--run-id",
        "t1",
        "--label",
        label,
        repo=wired.root,
    )

    assert code == 2
    assert "--run-id" in payload["error"]
    assert not (docker_stub / "argv.jsonl").exists()


def test_the_container_is_always_removed(run: Any, wired: Any, tmp_path: Path, docker_stub: Path) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(tmp_path / "reports"),
        "--run-id",
        "t1",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )

    assert (docker_stub / "removed").is_file()
    assert "rm -f" in (docker_stub / "removed").read_text().replace("-f ", "-f ")
    # `rm` means the container is gone, not merely that `rm` was called.
    pid = int((docker_stub / "takaro-verify-minecraft-fabric-26.2-t1" / "pid").read_text().strip())
    assert process_is_gone(pid)


def test_a_failed_deploy_leaves_no_data_directory_behind(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    monkeypatch.setenv("TMPDIR", str(scratch))
    monkeypatch.setattr(tempfile, "tempdir", None)
    artifacts = artifacts_for(run, wired, tmp_path)
    next(artifacts.glob("*.jar")).unlink()  # the manifest still names it, so deploy fails

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(tmp_path / "reports"),
        "--run-id",
        "t1",
        repo=wired.root,
    )

    assert code != 0
    assert "deploy" in payload["error"]
    assert list(scratch.glob("takaro-verify-*")) == []
    assert not (docker_stub / "argv.jsonl").exists()


def test_report_dirtiness_is_scoped_to_the_connector_paths(tmp_path: Path) -> None:
    from takaro_maint.verify.report import repo_identity

    root = tmp_path / "repo"
    (root / "catalog" / "minecraft").mkdir(parents=True)
    (root / "catalog" / "minecraft" / "game.json").write_text("{}\n")
    subprocess.run(["git", "init", "-q", "-b", "main", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@example.invalid",
            "commit",
            "-qm",
            "x",
        ],
        check=True,
    )

    watched = ["games/minecraft", "catalog/minecraft"]
    (root / "maintenance" / "tests" / "__pycache__").mkdir(parents=True)
    (root / "maintenance" / "tests" / "__pycache__" / "x.pyc").write_bytes(b"")
    assert repo_identity(root, watched)[2] is False

    (root / "catalog" / "minecraft" / "stray.json").write_text("{}\n")
    assert repo_identity(root, watched)[2] is True


def test_a_failing_check_exits_eight_and_still_writes_a_report(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("STUB_FAIL", "connector-load")
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(out),
        "--run-id",
        "t1",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )

    assert code == 8
    report = json.loads((out / "fabric-26.2" / "report.json").read_text())
    assert report["outcome"] == "fail"
    assert report["level"] == "startup"
    assert (docker_stub / "removed").is_file()


def test_a_failed_startup_skips_the_rest_of_the_ladder(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("STUB_FAIL", "hang")
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(out),
        "--run-id",
        "t1",
        "--startup-timeout",
        "0.2",
        repo=wired.root,
    )

    assert code == 8, payload
    report = json.loads((out / "fabric-26.2" / "report.json").read_text())
    failures = [check for check in report["checks"] if check["status"] == "fail"]
    assert [check["id"] for check in failures] == ["startup"]
    later = [check for check in report["checks"] if check["id"] not in {"build", "startup"}]
    assert later
    assert all(check["status"] == "skip" for check in later)
    assert {check["detail"]["reason"] for check in later} == {"startup did not complete; see the startup check"}
    assert report["outcome"] == "fail"


def test_a_subset_of_checks_marks_the_rest_skipped(run: Any, wired: Any, tmp_path: Path, docker_stub: Path) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, _, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(out),
        "--checks",
        "build",
        "--run-id",
        "t1",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )

    assert code == 0
    report = json.loads((out / "fabric-26.2" / "report.json").read_text())
    statuses = {check["id"]: check["status"] for check in report["checks"]}
    assert statuses["build"] == "pass"
    assert statuses["console"] == "skip"
    assert report["level"] == "contract"


def test_a_subset_that_skips_the_build_check_reports_no_level(
    run: Any, wired: Any, tmp_path: Path, docker_stub: Path, repo_root: Path
) -> None:
    from jsonschema import Draft202012Validator

    artifacts = artifacts_for(run, wired, tmp_path)
    out = tmp_path / "reports"

    code, _, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(out),
        "--checks",
        "startup",
        "--run-id",
        "t1",
        "--startup-timeout",
        "60",
        repo=wired.root,
    )

    assert code == 0
    report = json.loads((out / "fabric-26.2" / "report.json").read_text())
    schema = json.loads((repo_root / "catalog/schema/v1/verify-report.schema.json").read_text())
    assert list(Draft202012Validator(schema).iter_errors(report)) == []
    statuses = {check["id"]: check["status"] for check in report["checks"]}
    assert statuses["startup"] == "pass"
    assert statuses["build"] == "skip"
    # The artifact was never checked, so the report may not claim the build class.
    assert report["level"] == "none"


def test_an_unknown_check_exits_two(run: Any, wired: Any, tmp_path: Path) -> None:
    artifacts = artifacts_for(run, wired, tmp_path)

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(artifacts),
        "--out",
        str(tmp_path / "reports"),
        "--checks",
        "nonsense",
        repo=wired.root,
    )

    assert code == 2
    assert "nonsense" in payload["error"]


def test_a_missing_build_manifest_exits_two(run: Any, wired: Any, tmp_path: Path) -> None:
    empty = tmp_path / "nothing"
    empty.mkdir()

    code, payload, _ = run(
        "verify",
        "--game",
        "minecraft",
        "--artifacts",
        str(empty),
        "--out",
        str(tmp_path / "reports"),
        repo=wired.root,
    )

    assert code == 2
    assert "build-manifest.json" in payload["error"]
