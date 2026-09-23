# Target support, retention and retirement

What "supported" means for a server target, what this repository keeps and what only upstream
holds, and how a target stops being supported. The records themselves are described in
[the catalog](../../catalog/README.md); this page is the policy around them.

## Support states

Every target record carries `support.status`. There are three, and a target only ever moves
between them through a reviewed pull request.

| State | What it claims | How it is entered |
|---|---|---|
| `candidate` | The record exists, its inputs resolve and install, and it builds. Its `verification.required` has been met at least at `build` or `contract` level on a rig, but it has not been proven at runtime in CI. | A catalog pull request adding the record, with `catalog validate --online` green. |
| `maintained` | The target is proven at or above its own `verification.required`, and upstream moves against it are tracked and filed. | A catalog pull request promoting it, citing the verification report in `support.evidence`. |
| `retired` | The record is kept so old reports and compatibility records still resolve, but nothing builds, ships or hash-checks it any more. | A catalog pull request setting the status; see [retiring a target](#retiring-a-target). |

`verification.required` is the level the evidence has to reach before the target may ship in a
stable release: `build`, `contract`, `startup`, `protocol` or `gameplay`, in that order. A report
below the required level, a failing report, or no report at all stops `release assemble` with
exit 8 — see [what blocks a release](release.md#what-blocks-a-release) and
[levels and outcome are not the same thing](verify.md#levels-and-outcome-are-not-the-same-thing).

Neither promotion nor retirement happens automatically. A scheduled run observes, files and
reports; it never edits a record.

## What is maintained today

A snapshot, not the source of truth. `takaro-maint targets list --format table` prints the live
answer, and the record is the authority.

| Game | Target | Platform | Game version | Status | Verification required |
|---|---|---|---|---|---|
| 7 Days to Die | `linux-3.2.0.b10` | linux | 3.2.0.b10 | candidate | contract |
| Conan Exiles | `linux-25356024` | linux | 25356024 | candidate | contract |
| Enshrouded | `proton-1024233` | proton | 1024233 | candidate | contract |
| Minecraft | `fabric-26.1.2` | fabric | 26.1.2 | maintained | protocol |
| Minecraft | `fabric-26.2` | fabric | 26.2 | maintained | protocol |
| Minecraft | `neoforge-1.21.11` | neoforge | 1.21.11 | maintained | protocol |
| Minecraft | `paper-1.21.11` | paper | 1.21.11 | maintained | protocol |
| Rust | `carbon-25353106` | carbon | 25353106 | candidate | build |
| Terraria | `tshock-v6.1.0` | tshock | v6.1.0 | candidate | contract |
| Valheim | `linux-1.0.15` | linux | 1.0.15 | candidate | contract |
| Project Zomboid | `linux-42.20.4` | linux | 42.20.4 | candidate | contract |

Exactly one target per (game, platform) is the default, and that is the one a command with no
`--target` resolves to.

## Upstream-only retention

The repository is a set of *pins*, not a mirror. That is a deliberate limit with consequences
worth stating plainly:

* **No upstream bytes are kept here.** No game binaries, no depot manifests, no installer jars,
  no framework packages, no container images. A record names an input and its sha256; every
  install fetches from upstream and verifies by hash.
* **When upstream withdraws an input, the target becomes uninstallable.** A purged Steam manifest,
  a deleted release asset or a removed image tag makes `install` exit 4, and it **never** falls
  back to a branch head or a "latest" of anything. The response is a new, reviewed target and the
  retirement of the old one — not a mirror and not a looser pin.
* **Observation history is only what the dashboard holds.** The checkpoint lives in the dashboard
  issue body, which is capped, so history is bounded by design. Steam sources are heads-only:
  Steam publishes no history of a branch, so nothing can reconstruct what a branch pointed at
  last month.
* **A declined issue is never re-filed.** An issue closed as not planned is terminal, however
  many times the head is observed again.
* **Release assets live only on GitHub Releases.** Stable releases stay; rolling releases are
  replaced atomically; PR builds are deleted when the pull request closes. Legacy asset aliases
  are kept for two stable releases and then dropped by a catalog pull request.
* **Workflow diagnostics are kept 14 days**, and that is the only run-level history there is.
* **No credential is retained anywhere.** Credentials are read from the environment for the
  duration of a process and redacted from everything the command writes.

If a deployment needs an input to outlive upstream, it has to keep that copy itself; the
[integration contract](integration.md) says how to consume the pins without inventing new ones.

## Retiring a target

**Triggers.** A target is retired when any of these is true:

* upstream withdrew an input it pins and it can no longer be installed from a clean machine;
* a newer maintained target on the same platform has superseded it, *and* that successor has
  shipped in two stable releases;
* there is a security or licence reason not to ship it.

**Mechanics.** Set `support.status: "retired"` in a reviewed pull request. The record stays, so a
report or compatibility record that cites it still resolves. The build project it names is removed
later, once nothing builds it. Any `legacyAssetAliases` entry pointing at it is dropped once its
successor has shipped in two stable releases. The full mechanics are
[retiring a target](../../catalog/README.md#retiring-a-target).

**What consumers see.** The compat record of an old release keeps its `status` as it was when that
release shipped. From the next release on, the target is absent from build matrices, from the
generated README table and from online hash checks, and the lifecycle reports `<id> is retired on
main` rather than treating the issue as unfinished work.

## When upstream moves

A new upstream head is an observation, then an issue, then — when the work is done — a closed
issue. That issue's lifecycle is the whole commitment surface:
`detected → blocked-upstream | ready-for-agent → implementation-pr → awaiting-release → released`,
each state backed by a fact anybody can check (see [lifecycle](lifecycle.md)).

There is no time-bound promise attached to any of it. "Supported" means an exact, reviewed target
exists and is proven; it does not mean that the day upstream ships a new build there is a target
for it. What is promised is that the move is *visible*: filed within one scheduled run, with the
readiness of every framework it depends on written into the issue.

## Connectors outside the catalog

Every released connector has game-release discovery. Some still lack exact catalog targets
and use their existing build/release path.

| Connector | Today | Plan |
|---|---|---|
| RuneScape: Dragonwilds | Steam release discovery enabled; releases in legacy mode with its own rig tags | Exact targets, a game adapter and a rig driven by the resolved target remain a milestone-2 follow-up |
| VEIN | Steam release discovery enabled; releases in legacy mode | Same follow-up |
| DayZ | A `dev-servers/` rig only — no catalog record, no release path through the catalog | Not scheduled |
| Palworld | A `dev-servers/` rig only | Not scheduled |

Until their exact targets and rigs are migrated, these four are the only places a floating selector is knowingly allowed
in a tracked entry point, and each one is listed with its reason in the selector audit
(`maintenance/tests/test_selector_audit.py`). Migrating a connector to exact targets removes its allowlist entry;
that is the acceptance criterion the follow-up carries.
