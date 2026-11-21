"""Minecraft's verification hooks: what the generic runner cannot know about this game.

The runner discovers this module by name and calls into it at three points of a local run
(after the protocol checks, after shutdown, and for the opt-in negative check) or hands the
whole run over for ``--takaro hosted``. Everything generic lives in ``verify.checks_lifecycle``;
what is here is Minecraft: which banners name a runtime, which target is a sibling of which,
what a hosted gameserver is called, and what a refusal looks like on these loaders.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import re
from pathlib import Path
from typing import Any

from ... import output, paths
from ...exit_codes import UpstreamUnavailable
from ...verify import checks, checks_lifecycle
from ...verify.hooks import GameHooks
from ...verify.report import build_report, write_report
from . import gameplay
from .fabric import TARGET_CHECK_PREFIX

CHECK_IDS = (*gameplay.CHECK_IDS, *checks_lifecycle.CHECK_IDS)

# The first log line that names the runtime, one marker per platform. Cheap to test for,
# so a multi-megabyte log is not run through three regexes line by line.
BANNER_MARKERS = (
    "with Fabric Loader",
    "This server is running Paper version",
    "NeoForge mod loading",
)

# Fabric Loader refuses a jar whose `fabric.mod.json` pins another game version before any
# Takaro class loads; the connector's own guard refuses later, on platforms that get that far.
REFUSAL = checks_lifecycle.Refusal(
    loader=re.compile(r"Incompatible mods found!|Mod resolution failed"),
    detail=re.compile(r"requires version \S+ of '"),
    guard=re.compile(re.escape(TARGET_CHECK_PREFIX) + r'.*"result"\s*:\s*"refuse"'),
)

HOSTED_SKIP = "hosted mode proves registration and reachability; this check is asserted against the local fake"
# `redact` already hides every *TOKEN*/*PASSWORD* environment value by name; these are the
# ones whose names do not give them away, and they go into the retained files' extra list.
HOSTED_ENV_KEYS = ("TAKARO_WS_URL", "TAKARO_HOST", "TAKARO_DOMAIN_ID", "TAKARO_USERNAME")


def scan_runtime_identity(adapter: Any, log_file: Path) -> dict[str, Any]:
    """The runtime identity from whichever platform banner this server wrote."""
    if not log_file.is_file():
        return {}
    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not any(marker in line for marker in BANNER_MARKERS):
                continue
            parsed = adapter.parse_runtime_identity(line)
            if parsed and parsed.get("gameVersion"):
                return dict(parsed)
    return {}


def sibling_target(catalog: Any, target: Any) -> Any | None:
    """The other Fabric target, whose jar this server must refuse. Only Fabric has one."""
    if target.platform != "fabric":
        return None
    others = [
        other
        for other in catalog.game(target.game).targets
        if other.id != target.id and other.platform == "fabric" and other.status != "retired"
    ]
    return sorted(others, key=lambda other: other.id)[0] if others else None


# Takaro stores an identity token in a 50-character column and answers anything longer with a
# bare 400 on identify, naming nothing. An identity is therefore built to fit rather than
# assembled and hoped for: `neoforge-1.21.11` plus a four-character run id is already 51.
IDENTITY_LIMIT = 50

# Length of `TargetRun.nonce` (``secrets.token_hex(3)``). The prefix reserves this much room so
# that a prefix and a nonce always fit, whatever the target id.
NONCE_CHARS = 6


def hosted_identity_prefix(target: Any) -> str:
    """Every identity a hosted verification of this target has ever registered under.

    The stale-registration sweep matches on exactly this string, so it must not depend on the
    run id — only on the target.
    """
    return f"takaro-maint-{target.game}-{target.id}-"[: IDENTITY_LIMIT - NONCE_CHARS]


def hosted_identity(target: Any, run_id: str, nonce: str) -> str:
    """The identity token this hosted run registers its gameserver under.

    It has to be new every time: Takaro answers a registration under an identity whose
    gameserver was deleted with 409, so a fixed identity works exactly once. The nonce is what
    guarantees that; the run id is only there to make a registration traceable, so it is the
    part that gives way when the whole thing would exceed ``IDENTITY_LIMIT``.
    """
    prefix = hosted_identity_prefix(target)
    room = IDENTITY_LIMIT - len(prefix) - len(nonce)
    return f"{prefix}{f'{run_id}-'[:room] if room > 0 else ''}{nonce}"


# --------------------------------------------------------------------------- local hooks


async def after_protocol(run: Any, fake: Any, alive: Any) -> None:
    await gameplay.run_gameplay(run, fake)
    if run.wanted("reconnect"):
        run.record(await checks_lifecycle.check_reconnect(run, fake, alive))
    else:
        run.skip("reconnect", "not selected by --checks")


async def after_shutdown(run: Any, fake: Any, ws_url: str, ledger_inputs: list[dict[str, Any]]) -> None:
    if not run.wanted("restart"):
        run.skip("restart", "not selected by --checks")
        return
    shutdown = next((result for result in run.results if result.id == "shutdown"), None)
    if shutdown is None or shutdown.status != "pass":
        run.skip("restart", "shutdown failed; no clean data dir to reboot")
        return
    run.record(await checks_lifecycle.check_restart(run, fake, ws_url, ledger_inputs))


async def negative(run: Any, fake: Any, ws_url: str, manifest: dict[str, Any]) -> None:
    if not run.wanted("negative-wrong-target"):
        run.skip("negative-wrong-target", "not selected by --checks")
        return
    sibling = sibling_target(run.catalog, run.target)
    if sibling is None:
        run.skip("negative-wrong-target", f"no sibling target on platform {run.target.platform}")
        return
    run.record(
        await checks_lifecycle.check_negative_wrong_target(
            run, fake, ws_url, manifest, sibling=sibling, refusal=REFUSAL
        )
    )


# --------------------------------------------------------------------------- hosted mode


async def run_hosted(
    run: Any, manifest: dict[str, Any], ledger_inputs: list[dict[str, Any]], started_at: str
) -> dict[str, Any]:
    """One boot per target against the real Takaro: it registers, is reachable, and shuts down.

    The catalogue, console and lifecycle checks stay the local fake's job, so a hosted report
    reaches ``startup`` by design. Every id and host is redacted before anything is retained.
    """
    identity = hosted_identity(run.target, run.options.run_id, run.nonce)
    prefix = hosted_identity_prefix(run.target)
    ws_url = os.environ["TAKARO_WS_URL"]
    registration_token = os.environ["TAKARO_REGISTRATION_TOKEN"]
    api_host = os.environ["TAKARO_HOST"]
    domain_id = os.environ["TAKARO_DOMAIN_ID"]
    takaro = checks_lifecycle.HostedTakaro(
        api_host, os.environ["TAKARO_USERNAME"], os.environ["TAKARO_PASSWORD"], domain_id
    )
    # Known before the first container starts, because every exit from here on redacts with it.
    extra = [
        *(os.environ[key] for key in HOSTED_ENV_KEYS),
        checks_lifecycle.host_of(ws_url),
        checks_lifecycle.host_of(api_host),
    ]
    runtime: dict[str, Any] = {}
    server_ids: list[str] = []
    try:
        # Nothing can be asserted if Takaro will not talk to us, so this is the run failing
        # to start rather than a check with a verdict.
        try:
            await asyncio.to_thread(takaro.login)
            stale_servers = await asyncio.to_thread(takaro.find_prefix, prefix)
        except checks_lifecycle.HostedApiError as exc:
            raise UpstreamUnavailable(f"the hosted Takaro did not accept this run: {exc}") from None
        for stale in stale_servers:
            output.info("deleting a gameserver an earlier verification of this target left behind")
            await asyncio.to_thread(takaro.delete, stale)

        if run.wanted("build"):
            run.record(checks.check_build(run.options.artifacts, manifest, run.target))
        else:
            run.skip("build", "not selected by --checks")

        container = run.boot(
            ws_url,
            extra_env={
                "TAKARO_WS_URL": ws_url,
                "TAKARO_IDENTITY_TOKEN": identity,
                "TAKARO_REGISTRATION_TOKEN": registration_token,
            },
        )
        alive = container.alive

        if run.wanted("startup"):
            run.record(
                await asyncio.to_thread(
                    checks.check_startup,
                    run.server_log,
                    run.startup_timeout,
                    alive,
                    run.data_dir,
                    ledger_inputs,
                )
            )
        else:
            run.skip("startup", "not selected by --checks")

        identity_scan = await asyncio.to_thread(scan_runtime_identity, run.adapter, run.server_log)
        runtime = {
            "gameVersion": identity_scan.get("gameVersion"),
            "loader": identity_scan.get("loader"),
            "loaderVersion": identity_scan.get("loaderVersion"),
            "java": run.target.record["runtime"]["java"],
        }

        if run.wanted("connector-load"):
            run.record(await asyncio.to_thread(checks.check_connector_load, run.server_log, run.target, 120, alive))
        else:
            run.skip("connector-load", "not selected by --checks")

        run.record(await _hosted_identify(run, alive))
        run.record(await _hosted_registration(takaro, identity, server_ids))
        run.record(await _hosted_heartbeat(takaro, server_ids))
        run.record(await _hosted_players(takaro, server_ids))

        for check_id in (
            "catalog-items",
            "catalog-entities",
            "console",
            *gameplay.CHECK_IDS,
            "reconnect",
            "negative-wrong-target",
        ):
            run.skip(check_id, HOSTED_SKIP)

        run.record(await _hosted_shutdown(takaro, container, server_ids))
        run.skip("restart", "hosted mode boots once")
    finally:
        with contextlib.suppress(checks_lifecycle.HostedApiError):
            server_ids.extend(await asyncio.to_thread(takaro.find, identity))
        for server_id in set(server_ids):
            await asyncio.to_thread(takaro.delete, server_id)
        run._record_missing_selected_checks()
        run.cleanup()
        # Redaction belongs in the `finally`: a run that dies of a boot failure, a docker
        # error or an interrupt keeps its logs just the same, and those are the files that
        # get banked. It also runs before the report is built, so `logs[].sha256` describes
        # the bytes that were kept; the report itself is redacted right after it is written.
        checks_lifecycle.redact_retained(run.out, extra)

    report = build_report(
        target=run.target,
        game_record=run.catalog.game(run.target.game).record,
        manifest=manifest,
        artifacts_dir=run.options.artifacts,
        runtime=runtime,
        checks=[result.as_dict() for result in run.results],
        started_at=started_at,
        logs=[run.server_log, run.docker_log, *run.extra_logs],
        repo_root=paths.repo_root(),
        takaro="hosted",
    )
    write_report(run.out, report)
    checks_lifecycle.redact_retained(run.out, extra, names={"report.json"})
    return report


async def _hosted_identify(run: Any, alive: Any) -> checks.CheckResult:
    with checks._Timer() as timer:
        found = await asyncio.to_thread(
            checks.wait_for_line,
            run.server_log,
            checks_lifecycle.IDENTIFIED_LINE,
            checks_lifecycle.IDENTIFY_BUDGET,
            alive,
        )
        problems = [] if found else ["the connector never logged 'Identified successfully' against Takaro"]
    return checks.CheckResult(
        "identify",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"serverId": "<redacted>", "problems": problems},
        {"file": run.server_log.name, "line": found[0]} if found else {"file": run.server_log.name},
    )


async def _hosted_registration(takaro: Any, identity: str, server_ids: list[str]) -> checks.CheckResult:
    """A newly identified connector registers its own gameserver under the identity token."""
    with checks._Timer() as timer:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + checks_lifecycle.REGISTRATION_BUDGET
        found: list[str] = []
        problems: list[str] = []
        while loop.time() < deadline:
            try:
                found = await asyncio.to_thread(takaro.find, identity)
            except checks_lifecycle.HostedApiError as exc:
                problems.append(str(exc))
                break
            if len(found) == 1:
                break
            await asyncio.sleep(3)
        server_ids.extend(found)
        if not problems and len(found) != 1:
            problems.append(
                f"{len(found)} gameserver(s) carry this identity after "
                f"{checks_lifecycle.REGISTRATION_BUDGET:.0f} s, expected exactly 1"
            )
    return checks.CheckResult(
        "hosted-registration",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "identity": identity,
            "gameServerId": "<redacted>" if found else None,
            "registered": len(found) == 1,
            "problems": problems,
        },
    )


async def _hosted_heartbeat(takaro: Any, server_ids: list[str]) -> checks.CheckResult:
    with checks._Timer() as timer:
        problems: list[str] = []
        connectable = False
        if not server_ids:
            problems.append("no gameserver was registered, so reachability could not be asked for")
        for attempt in range(3):
            try:
                connectable = await asyncio.to_thread(takaro.reachability, server_ids[0]) if server_ids else False
            except checks_lifecycle.HostedApiError as exc:
                problems.append(str(exc))
                break
            if connectable:
                break
            if attempt < 2:
                await asyncio.sleep(10)
        if server_ids and not connectable and not problems:
            problems.append("Takaro could not reach the connector in three attempts")
    return checks.CheckResult(
        "heartbeat",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {
            "connectable": connectable,
            "note": "hosted: Takaro's own reachability probe; no application heartbeat",
            "problems": problems,
        },
    )


async def _hosted_players(takaro: Any, server_ids: list[str]) -> checks.CheckResult:
    with checks._Timer() as timer:
        problems: list[str] = []
        players: list[Any] = []
        if not server_ids:
            problems.append("no gameserver was registered, so its players could not be listed")
        else:
            try:
                players = await asyncio.to_thread(takaro.players, server_ids[0])
            except checks_lifecycle.HostedApiError as exc:
                problems.append(str(exc))
            if players:
                problems.append(f"an empty server reported {len(players)} players")
    return checks.CheckResult(
        "players",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"players": len(players), "problems": problems},
    )


async def _hosted_shutdown(takaro: Any, container: Any, server_ids: list[str]) -> checks.CheckResult:
    with checks._Timer() as timer:
        problems: list[str] = []
        if server_ids:
            try:
                await asyncio.to_thread(takaro.shutdown, server_ids[0])
            except checks_lifecycle.HostedApiError as exc:
                problems.append(str(exc))
        else:
            problems.append("no gameserver was registered, so Takaro could not be asked to shut it down")
        code = await asyncio.to_thread(container.wait_for_exit, checks_lifecycle.SHUTDOWN_BUDGET)
        if code != 0:
            problems.append(f"the server container exited {code}, expected 0")
    return checks.CheckResult(
        "shutdown",
        "pass" if not problems else "fail",
        timer.elapsed_ms,
        {"exitCode": code, "note": "shutdown requested through the Takaro API", "problems": problems},
    )


#: What this game contributes to a verification run; the runner reads nothing else.
HOOKS = GameHooks(
    check_ids=CHECK_IDS,
    negative_check_ids=("negative-wrong-target",),
    hosted_check_ids=("hosted-registration",),
    after_protocol=after_protocol,
    after_shutdown=after_shutdown,
    negative=negative,
    run_hosted=run_hosted,
    scan_runtime_identity=scan_runtime_identity,
)
