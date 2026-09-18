"""One framework upstream breaks: the others still finish, and the run says so.

Sources are independent by construction — a failure is all-or-nothing for that source's
checkpoint and nothing at all for anyone else's. These scenarios break one source at a time,
in each of the ways it can actually break (a 503, malformed XML, a digest sidecar that is
not a digest) and check the three things that matter: the exit code, whose checkpoint moved,
and whose rows were written.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

import test_readiness_transitions as rt
import test_scan_support as support

FABRIC_KEY = rt.FABRIC_KEY
PAPER_KEY = rt.PAPER_KEY
NEOFORGE_KEY = rt.NEOFORGE_KEY
MOJANG_KEY = support.SOURCE_KEY


def _seen(harness: rt.Rig, source_key: str) -> list[str] | None:
    checkpoint = harness.checkpoint(source_key)
    return None if checkpoint is None else [str(entry[0]) for entry in checkpoint["seen"]]


def test_a_failing_paper_source_keeps_its_checkpoint_and_exits_four(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC10: Paper is down, Fabric and NeoForge advance, the run reports 4."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        rt._seed_support_issue(harness, "26.2")
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        paper_before = _seen(harness, PAPER_KEY)
        fabric_before = _seen(harness, FABRIC_KEY)
        last_success = harness.dashboard_state()["lastSuccess"]

        rt.add_fabric_api(harness.upstream, "0.161.0+26.3")
        rt.add_neoforge_version(harness.upstream, "26.2.0.89")
        harness.upstream.status_overrides[rt.PAPER_PROJECT_PATH] = 503

        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 4, stderr
        assert payload["sources"][PAPER_KEY]["status"] == "failed"
        assert "503" in str(payload["sources"][PAPER_KEY]["error"])
        assert payload["sources"][FABRIC_KEY]["status"] == "ok"
        assert payload["sources"][NEOFORGE_KEY]["status"] == "ok"

        assert _seen(harness, PAPER_KEY) == paper_before
        assert _seen(harness, FABRIC_KEY) != fabric_before
        assert "0.161.0+26.3" in (_seen(harness, FABRIC_KEY) or [])
        assert "26.2.0.89" in (_seen(harness, NEOFORGE_KEY) or [])

        sources = harness.dashboard_state()["sources"]
        assert "503" in str(sources[PAPER_KEY]["lastError"])
        assert sources[FABRIC_KEY]["lastError"] is None
        assert sources[NEOFORGE_KEY]["lastError"] is None
        assert harness.dashboard_state()["lastSuccess"] == last_success

        assert harness.rows(kind="support", rev="26.3")["fabric"].rev == "0.161.0+26.3"
        assert harness.rows(kind="support", rev="26.2")["neoforge"].rev == "26.2.0.89"


def test_a_failing_source_on_bootstrap_stays_uninitialised(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC10: a source that never got a checkpoint keeps `None`, it does not get a broken one."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        harness.upstream.add(rt.NEOFORGE_META_PATH, b"<metadata><versioning><versions>")

        code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 4, stderr
        assert payload["sources"][NEOFORGE_KEY]["status"] == "failed"
        assert "not maven metadata XML" in str(payload["sources"][NEOFORGE_KEY]["error"])
        assert harness.checkpoint(NEOFORGE_KEY) is None
        for key in (MOJANG_KEY, FABRIC_KEY, PAPER_KEY):
            assert harness.checkpoint(key) is not None, key

        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        assert rt.readiness_rows(body)["fabric"].status == "ready"
        assert rt.table_statuses(body)["neoforge"] == "missing"


def test_a_bad_digest_sidecar_fails_only_that_source(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """AC10: a sidecar that is not a digest is a failed source, not a row without a digest."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        harness.upstream.add(rt.fabric_api_path("0.160.7+26.3") + ".sha256", b"not-hex\n")

        code, payload, stderr = harness.scan(run, "--bootstrap", "--publish")

        assert code == 4, stderr
        assert payload["sources"][FABRIC_KEY]["status"] == "failed"
        assert "sidecar is not a sha256 digest" in str(payload["sources"][FABRIC_KEY]["error"])
        assert harness.checkpoint(FABRIC_KEY) is None
        assert payload["sources"][PAPER_KEY]["status"] == "ok"
        assert payload["sources"][NEOFORGE_KEY]["status"] == "ok"

        # The game issue is still filed, and Fabric claims nothing at all: a listing whose
        # source failed is withdrawn rather than rendered without the digest it promised.
        body = str(harness.issue_with(kind="support", rev="26.3")["body"])
        assert rt.readiness.parse_rows(body) is None
        assert rt.issues.READINESS_SENTENCE in body

        # A later run with a readable sidecar fills the row in.
        harness.upstream.add(
            rt.fabric_api_path("0.160.7+26.3") + ".sha256",
            b"1720e31ab65c62d4de6e963606d74db4d6063bf58b065f40296cdaa68b25759d\n",
        )
        code, _, stderr = harness.scan(run, "--bootstrap", "--publish")
        assert code == 0, stderr
        row = harness.rows(kind="support", rev="26.3")["fabric"]
        assert row.status == "ready"
        assert row.sha256 == "1720e31ab65c62d4de6e963606d74db4d6063bf58b065f40296cdaa68b25759d"


def test_read_only_partial_failure_writes_nothing(
    run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The same run without `--publish`: it still reports 4 and still writes nothing at all."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        rt.add_fabric_api(harness.upstream, "0.161.0+26.3")
        harness.upstream.status_overrides[rt.PAPER_PROJECT_PATH] = 503
        writes = harness.fake.writes

        code, payload, stderr = harness.scan(run)

        assert code == 4, stderr
        assert payload["mode"] == "read-only"
        assert harness.fake.writes == writes
        assert payload["applied"] == []
        assert any(entry["action"] == "update-issue" for entry in payload["plan"])


def test_every_source_is_retried_on_the_next_run(run: Any, catalog_copy: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """AC10: nothing is quarantined — once Paper answers again the run is green."""
    with rt.rig(catalog_copy, monkeypatch) as harness:
        rt._seed_support_issue(harness, "26.2")
        assert harness.scan(run, "--bootstrap", "--publish")[0] == 0
        harness.upstream.status_overrides[rt.PAPER_PROJECT_PATH] = 503
        rt.add_paper_build(harness.upstream, "26.2", 125, "STABLE", sha256="cc" * 32)
        assert harness.scan(run, "--publish")[0] == 4
        assert "26.2-125" not in (_seen(harness, PAPER_KEY) or [])

        del harness.upstream.status_overrides[rt.PAPER_PROJECT_PATH]
        code, payload, stderr = harness.scan(run, "--publish")

        assert code == 0, stderr
        assert payload["sources"][PAPER_KEY]["status"] == "ok"
        assert "26.2-125" in (_seen(harness, PAPER_KEY) or [])
        row = harness.rows(kind="support", rev="26.2")["paper"]
        assert row.rev == "26.2-125"
        assert row.sha256 == "cc" * 32
        assert harness.dashboard_state()["sources"][PAPER_KEY]["lastError"] is None
