"""``takaro-maint icons publish`` against an in-process S3 fake."""

from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path
from typing import Any

import pytest
from PIL import Image, ImageDraw

from conftest import FIXTURES
from fake_s3 import KEY_ID, SECRET, FakeS3, serving

FIXTURE = FIXTURES / "icons" / "fixture-846"
GAME = "fixture-846"
WEBP_CACHE = "public, max-age=31536000, immutable"
INDEX_CACHE = "public, max-age=300"


@pytest.fixture
def fake(monkeypatch: pytest.MonkeyPatch) -> Any:
    with serving(monkeypatch) as server:
        yield server


def _build(run: Any, tmp_path: Path, name: str = "out", recipes: Path | None = None) -> Path:
    out = tmp_path / name
    code, payload, _ = run("icons", "build", "--recipes", str(recipes or FIXTURE / "recipes.json"), "--out", str(out))
    assert code == 0, payload
    return out / "pack" / GAME


def _publish(run: Any, pack: Path, *extra: str, env: str = "staging") -> tuple[int, Any, str]:
    return run("icons", "publish", "--env", env, "--pack", str(pack), *extra)


def _local_index(pack: Path) -> dict[str, Any]:
    return json.loads((pack / "index.json").read_text())


def test_first_publish_uploads_objects_then_the_index(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    pack = _build(run, tmp_path)
    code, payload, _ = _publish(run, pack)
    assert code == 0 and payload["ok"] is True

    assert [(request.method, request.key) for request in fake.requests[:1]] == [("GET", f"{GAME}/index.json")]
    methods = [request.method for request in fake.requests]
    assert methods == ["GET"] + ["HEAD"] * 5 + ["PUT"] * 6 + ["GET"]
    assert fake.requests[-2].key == f"{GAME}/index.json"
    assert fake.requests[-1].key == f"{GAME}/index.json"
    for request in fake.puts()[:-1]:
        assert request.key.endswith(".webp")
        assert (request.content_type, request.cache_control) == ("image/webp", WEBP_CACHE)
    assert (fake.puts()[-1].content_type, fake.puts()[-1].cache_control) == ("application/json", INDEX_CACHE)
    assert fake.objects[f"{GAME}/index.json"][0] == (pack / "index.json").read_bytes()
    for webp in pack.glob("*.webp"):
        assert fake.objects[f"{GAME}/{webp.name}"][0] == webp.read_bytes()
    assert {request.signed_with for request in fake.requests} == {KEY_ID}
    assert len(payload["objects"]["uploaded"]) == 5 and payload["objects"]["skipped"] == []
    assert payload["index"]["action"] == "uploaded"


def test_dry_run_plans_without_writing(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    pack = _build(run, tmp_path)
    code, payload, _ = _publish(run, pack, "--dry-run")
    assert code == 0
    assert fake.puts() == [] and fake.objects == {}
    assert payload["dryRun"] is True
    assert len(payload["objects"]["wouldUpload"]) == 5
    assert payload["index"]["action"] == "would-upload"
    assert payload["index"]["added"] == sorted(_local_index(pack)["icons"])


def test_republishing_the_same_pack_changes_nothing(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    pack = _build(run, tmp_path)
    assert _publish(run, pack)[0] == 0
    before = len(fake.puts())
    code, payload, _ = _publish(run, pack)
    assert code == 0
    assert len(fake.puts()) == before
    assert len(payload["objects"]["skipped"]) == 5 and payload["objects"]["uploaded"] == []
    assert payload["index"]["action"] == "unchanged"


def _synthetic_png(path: Path) -> None:
    image = Image.new("RGBA", (48, 48), (0, 0, 0, 0))
    ImageDraw.Draw(image).polygon([(24, 2), (46, 46), (2, 46)], fill=(10, 200, 90, 255))
    image.save(path, format="PNG")


def test_rolling_merge_keeps_earlier_codes(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    v1 = _build(run, tmp_path, "v1")
    assert _publish(run, v1)[0] == 0
    v1_icons = _local_index(v1)["icons"]

    copy = tmp_path / "v2-recipes"
    shutil.copytree(FIXTURE, copy)
    _synthetic_png(copy / "images" / "new-48.png")
    document = json.loads((copy / "recipes.json").read_text())
    document["recipes"] = [recipe for recipe in document["recipes"] if recipe["code"] != "fixture-pebble"]
    for recipe in document["recipes"]:
        if recipe["code"] == "fixture-banner":
            recipe["layers"][0]["image"] = "images/crate-64.png"
    document["recipes"].append({"code": "fixture-new", "layers": [{"image": "images/new-48.png"}]})
    (copy / "recipes.json").write_text(json.dumps(document))
    v2 = _build(run, tmp_path, "v2", recipes=copy / "recipes.json")
    v2_icons = _local_index(v2)["icons"]

    fake.requests.clear()
    code, payload, _ = _publish(run, v2)
    assert code == 0
    live = json.loads(fake.objects[f"{GAME}/index.json"][0])
    assert live["icons"]["fixture-pebble"] == v1_icons["fixture-pebble"]
    assert live["icons"]["fixture-banner"] == v2_icons["fixture-banner"] == v2_icons["fixture-crate"]
    assert live["icons"]["fixture-new"] == v2_icons["fixture-new"]
    assert payload["index"]["added"] == ["fixture-new"]
    assert payload["index"]["kept"] == ["fixture-pebble"]
    assert payload["index"]["repointed"] == [
        {"code": "fixture-banner", "from": v1_icons["fixture-banner"], "to": v2_icons["fixture-banner"]}
    ]
    assert f"{GAME}/{v1_icons['fixture-pebble']}" in fake.objects
    assert [request.key for request in fake.puts() if request.key.endswith(".webp")] == [
        f"{GAME}/{v2_icons['fixture-new']}"
    ]
    assert list(live) == ["game", "enabled", "icons"] and list(live["icons"]) == sorted(live["icons"])


def test_a_disabled_live_index_stays_disabled(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    pack = _build(run, tmp_path)
    live = json.dumps({"game": GAME, "enabled": False, "icons": {}}).encode()
    fake.seed(f"{GAME}/index.json", live, "application/json")
    code, payload, stderr = _publish(run, pack)
    assert code == 0
    assert json.loads(fake.objects[f"{GAME}/index.json"][0])["enabled"] is False
    assert payload["index"]["enabled"] is False
    assert "warning" in stderr and "disabled" in stderr


def test_store_keys_are_refused_before_any_network_call(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    built = _build(run, tmp_path)
    store = tmp_path / "_store"
    shutil.copytree(built, store)
    index = _local_index(store)
    index["game"] = "_store"
    (store / "index.json").write_text(json.dumps(index))
    code, payload, _ = _publish(run, store)
    assert code == 2 and payload["ok"] is False
    assert "_store/" in payload["error"]
    assert fake.requests == []


def test_production_is_refused(run: Any, tmp_path: Path, fake: FakeS3, monkeypatch: pytest.MonkeyPatch) -> None:
    pack = _build(run, tmp_path)
    for name in ("PROD_R2_ACCESS_KEY_ID", "PROD_R2_SECRET_ACCESS_KEY", "PROD_R2_ENDPOINT", "PROD_R2_BUCKET"):
        monkeypatch.setenv(name, "production-value-should-never-be-read")
    code, payload, _ = _publish(run, pack, env="production")
    assert code == 2 and "icons promote" in payload["error"]
    assert fake.requests == []
    for env in ("prod", "Staging"):
        assert _publish(run, pack, env=env)[0] == 2
    assert fake.requests == []


@pytest.mark.parametrize(
    "variable", ["STG_R2_ACCESS_KEY_ID", "STG_R2_SECRET_ACCESS_KEY", "STG_R2_ENDPOINT", "STG_R2_BUCKET"]
)
def test_missing_credentials_name_the_variable(
    run: Any, tmp_path: Path, fake: FakeS3, monkeypatch: pytest.MonkeyPatch, variable: str
) -> None:
    pack = _build(run, tmp_path)
    monkeypatch.delenv(variable)
    code, payload, _ = _publish(run, pack)
    assert code == 2 and variable in payload["error"]
    assert fake.requests == []


def test_endpoint_must_be_https(run: Any, tmp_path: Path, fake: FakeS3, monkeypatch: pytest.MonkeyPatch) -> None:
    pack = _build(run, tmp_path)
    monkeypatch.setenv("STG_R2_ENDPOINT", "http://example.invalid")
    code, payload, _ = _publish(run, pack)
    assert code == 2 and "https" in payload["error"]
    assert fake.requests == []


def _rename_a_webp(pack: Path) -> None:
    webp = sorted(pack.glob("*.webp"))[0]
    index = _local_index(pack)
    wrong = "0" * 64 + ".webp"
    webp.rename(pack / wrong)
    index["icons"] = {code: wrong if name == webp.name else name for code, name in index["icons"].items()}
    (pack / "index.json").write_text(json.dumps(index))


def _wrong_game(pack: Path) -> None:
    index = _local_index(pack)
    index["game"] = "other-game"
    (pack / "index.json").write_text(json.dumps(index))


def _oversized_webp(pack: Path) -> None:
    index = _local_index(pack)
    target = index["icons"]["fixture-pebble"]
    big = tmp_webp(pack.parent / "big.webp", (200, 200))
    name = hashlib.sha256(big).hexdigest() + ".webp"
    (pack / target).unlink()
    (pack / name).write_bytes(big)
    index["icons"]["fixture-pebble"] = name
    (pack / "index.json").write_text(json.dumps(index))


def tmp_webp(path: Path, size: tuple[int, int]) -> bytes:
    Image.new("RGBA", size, (255, 0, 0, 255)).save(path, format="WEBP", lossless=True)
    return path.read_bytes()


@pytest.mark.parametrize("break_pack", [_rename_a_webp, _wrong_game, _oversized_webp])
def test_local_contract_violations_stop_before_the_network(
    run: Any, tmp_path: Path, fake: FakeS3, break_pack: Any
) -> None:
    pack = _build(run, tmp_path)
    break_pack(pack)
    code, payload, _ = _publish(run, pack)
    assert code == 7 and payload["ok"] is False and payload["problems"]
    assert fake.requests == []


def test_an_extra_file_in_the_pack_is_a_contract_violation(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    pack = _build(run, tmp_path)
    (pack / "notes.txt").write_text("not part of a pack")
    assert _publish(run, pack)[0] == 7
    assert fake.requests == []


@pytest.mark.parametrize(
    "live",
    [json.dumps({"game": "other", "enabled": True, "icons": {}}).encode(), b"{not json"],
    ids=["game-mismatch", "invalid-json"],
)
def test_an_invalid_live_index_is_never_overwritten(run: Any, tmp_path: Path, fake: FakeS3, live: bytes) -> None:
    pack = _build(run, tmp_path)
    fake.seed(f"{GAME}/index.json", live, "application/json")
    code, _, _ = _publish(run, pack)
    assert code == 7
    assert fake.puts() == []
    assert fake.objects[f"{GAME}/index.json"][0] == live


def test_a_failed_upload_never_publishes_the_index_and_a_rerun_resumes(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    pack = _build(run, tmp_path)
    third = sorted(path.name for path in pack.glob("*.webp"))[2]
    fake.fail_on("PUT", rf"{third}$")
    code, payload, _ = _publish(run, pack)
    assert code == 10 and payload["ok"] is False
    assert f"{GAME}/index.json" not in fake.objects
    assert len([key for key in fake.objects if key.endswith(".webp")]) == 2

    fake.clear_failures()
    fake.requests.clear()
    code, payload, _ = _publish(run, pack)
    assert code == 0
    assert len(payload["objects"]["skipped"]) == 2 and len(payload["objects"]["uploaded"]) == 3
    assert fake.puts()[-1].key == f"{GAME}/index.json"
    assert len(fake.puts()) == 4


def test_access_denied_on_the_live_index_is_an_upload_failure(run: Any, tmp_path: Path, fake: FakeS3) -> None:
    pack = _build(run, tmp_path)
    fake.fail_on("GET", r"index\.json$")
    code, _, _ = _publish(run, pack)
    assert code == 10
    assert fake.puts() == []


def test_a_read_back_that_differs_is_an_integrity_failure(
    run: Any, tmp_path: Path, fake: FakeS3, monkeypatch: pytest.MonkeyPatch
) -> None:
    pack = _build(run, tmp_path)
    from takaro_maint.icons import bucket

    original = bucket.Bucket.get
    calls: list[str] = []

    def tampered(self: Any, key: str) -> bytes | None:
        calls.append(key)
        result = original(self, key)
        return result if len(calls) == 1 else b"tampered"

    monkeypatch.setattr(bucket.Bucket, "get", tampered)
    code, _, _ = _publish(run, pack)
    assert code == 5


def test_no_credential_value_reaches_the_output(run: Any, tmp_path: Path, fake: FakeS3, capsys: Any) -> None:
    pack = _build(run, tmp_path)
    secrets = [KEY_ID, SECRET, fake.endpoint, fake.bucket]
    third = sorted(path.name for path in pack.glob("*.webp"))[2]
    outputs: list[str] = []
    for extra in (["--dry-run"], []):
        _, payload, stderr = _publish(run, pack, *extra)
        outputs += [json.dumps(payload), stderr]
    fake.objects.clear()
    fake.fail_on("PUT", rf"{third}$")
    _, payload, stderr = _publish(run, pack)
    outputs += [json.dumps(payload), stderr]
    joined = "\n".join(outputs)
    for secret in secrets:
        assert secret not in joined
