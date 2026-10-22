# Runtime verification: proving a target works before anything ships

`takaro-maint verify` takes a catalog target and a built artifact and answers one question
with evidence: does this connector actually work on this exact server? It installs the
target's pinned inputs into a throwaway directory, deploys the artifact the build manifest
names, boots the pinned container against a Generic-Connector-Protocol endpoint, asks the
connector a fixed list of questions, and writes a report that ties every answer back to the
source revision, the target fingerprint and the artifact hashes it used.

It is the same command everywhere. A maintainer runs it on a laptop, the release workflow
runs it on a runner, and only the run id, the labels and the timeouts differ.

## The command

```
takaro-maint verify --game G [--target ID … | --all-targets] [--platform P] \
                    --artifacts DIR --out DIR \
                    [--checks a,b] [--parallel 1] [--startup-timeout SECONDS] \
                    [--takaro local|hosted] [--run-id X] [--label k=v]… \
                    [--keep-on-failure] [--cleanup-orphans] [--negative]
```

| Flag | Effect |
|---|---|
| `--target`, `--all-targets`, `--platform` | Which targets to verify. A game with several `default: true` records has no unambiguous default, so pass `--all-targets` or explicit `--target`s. |
| `--artifacts DIR` | A build output directory holding `build-manifest.json`. The manifest decides which file is deployed. |
| `--out DIR` | Where `<target>/report.json` and the logs are written. |
| `--checks a,b` | Run a subset. Unknown ids are a usage error; the ids a game adds count as known. |
| `--startup-timeout` | How long the server gets to reach its ready line. Defaults to the game's declared budget (900 s for Terraria), or 300 s when the game declares none; CI may override it explicitly. |
| `--negative` | Also boot the sibling target's artifact and require it to be refused (see below). |
| `--takaro hosted` | Register against a real Takaro instead of the local fake (see below). |
| `--run-id`, `--label` | The container-name suffix and the extra docker labels this run's containers carry. `tm.run` and `tm.ttl` are set by the harness (from `--run-id` and the clock) and are refused as `--label` keys, so a cleanup can always find the run. |
| `--keep-on-failure` | Keep the throwaway data directory when a check failed, for a post-mortem. |
| `--cleanup-orphans` | Remove containers a previous run with the same run id left behind, before starting. |
| `--parallel` | Reserved; only `1` is accepted. Two invocations give real parallelism. |

stdout is one JSON document:

```json
{"schemaVersion": 1, "op": "verify", "ok": true, "takaro": "local", "runId": "local",
 "out": "/…/reports", "reports": [{"target": "fabric-26.2", "level": "protocol",
 "outcome": "pass", "checks": [{"id": "build", "status": "pass"}]}]}
```

| Exit | Meaning |
|---|---|
| 0 | every selected check passed |
| 2 | usage: an unknown check id, a missing build manifest, or `--takaro hosted` without its environment (only the variable **names** are printed) |
| 3 | no such target, or an ambiguous default |
| 4 | install, deploy or docker failed before verification could start |
| 8 | a check failed — the report says which, per target |
| 130 | interrupted; every container and data directory was removed first |

## What one run does

1. **Install** the target's pinned inputs into `mktemp -d` and write the install ledger.
2. **Deploy** the artifact the build manifest names for this target into that directory.
3. **Start a fake Takaro** on the docker bridge gateway, on an ephemeral port.
4. **Boot** the pinned container on that data directory. No host port is ever published,
   the world is named `takaro-verify`, memory is capped, and the container carries
   `tm.run=<run id>`, `tm.ttl=<now + 3 h>` and any `--label` extras.
5. **Ask the questions** below, in order.
6. **Clean up** — containers removed, data directory deleted — on success, on failure, and
   on `SIGINT`/`SIGTERM`.
7. **Write** `<out>/<target>/report.json` plus `server.log`, `server-restart.log`,
   `server-negative.log` (when they ran), `fake-takaro.log`, `docker.log`, `install.json`
   and `deploy.json`.

## The checks

| id | Passes when | Budget |
|---|---|---|
| `build` | The manifest has this target's artifact, its fingerprint matches the catalog, and the file carries that identity. | — |
| `startup` | The server logs `Done (…)! For help, type "help"` and every pinned input is still byte-identical afterwards. | `--startup-timeout` |
| `connector-load` | The connector logs one `Takaro target-check: {…}` line with `result: "ok"`, and the runtime it reports matches the target: game version = the target's revision, loader version = the declared minimum loader, Java release as pinned. | 120 s |
| `identify` | An `identify` frame arrives and the server logs `Identified successfully`. | 180 s |
| `heartbeat` | Three websocket ping/pongs answer, and `testReachability` returns `connectable: true`. There is no application-level heartbeat; this is what the evidence is. | 30 s |
| `players` | `getPlayers` on an empty server returns `[]`. | 30 s |
| `catalog-items` | `listItems` returns a non-empty list, every entry's `name` differs from its `code`, no name is a registry id (`minecraft:`, `item.`, `entity.`, `block.`), and the spot check resolves `minecraft:diamond_sword` to `Diamond Sword`. | 60 s |
| `catalog-entities` | The same for `listEntities`, spot-checking `minecraft:zombie` → `Zombie`. | 60 s |
| `console` | `executeConsoleCommand` with `say takaro-verify-<run id>` is echoed in the server log. | 60 s |
| `reconnect` | Takaro closes the socket with code 1001; the connector identifies again, and the new socket answers a ping and `testReachability`. The log shows the close and a second `Identified successfully`. | 20 s to re-identify |
| `shutdown` | A `shutdown` request stops the container with exit code 0. | 120 s |
| `restart` | The same data directory boots a second time, the install ledger is byte-identical, the pinned inputs are intact, and the second server shuts down cleanly too. Skipped when `shutdown` failed — there is no clean data directory to reboot. | 180 s to identify |
| `negative-wrong-target` | Only with `--negative`. See below. | `--startup-timeout` |
| `hosted-registration` | Only with `--takaro hosted`. See below. | 60 s |

### What a game contributes: `HOOKS` and `UNSUPPORTED_CHECKS`

The table above is the base ladder, and it was written for the Minecraft connector. A game
contributes to a run through exactly one object: `HOOKS = GameHooks(...)` at the end of its
`games/<game>/verify.py`. `GameHooks` is a frozen dataclass whose every field is declared
and defaulted -- the ready line, the `check_ids` this game adds to the ladder,
`unsupported_checks`, the opt-in `negative_check_ids` and `hosted_check_ids`, and the
callables `before_boot`, `after_boot`, `after_protocol`,
`after_shutdown`, `negative`, `run_hosted` and `scan_runtime_identity`. A game that ships
no hooks module verifies with the base checks alone; a module that ships hooks but never
assembles them into `HOOKS` is a usage error rather than a game silently running without
them. The adapter's own run-time hooks (`install`, `after_deploy`, `container_mounts`,
`container_options`, `container_command`) come from `games.base.BaseAdapter`, which every
adapter subclasses, so a hook is a real method with a checked signature.

`unsupported_checks` maps a base check id to why this game cannot pass it and which of the
game's own checks stands in for it. A run that names no `--checks` is narrowed to
`check_ids(game)` minus those keys and any negative/hosted checks outside their mode, once,
in the runner before anything is installed. Each unsupported row is announced as
`not running <id>: <reason>`. An explicit `--checks` is taken literally, including a check
the game is known to fail or an opt-in negative check: naming a check is asking for it.

### Levels and outcome are not the same thing

`level` is how far the run got through the classes `build < contract < startup < protocol`,
and its membership is fixed: `protocol` means `connector-load`, `identify`, `heartbeat`,
`players`, both catalogues, `console` and `shutdown` all passed. The lifecycle rows
(`reconnect`, `restart`, `negative-wrong-target`, `hosted-registration`) deliberately do
**not** belong to a level — a connector that cannot reconnect still reached the protocol —
but they do gate `outcome`, and so the exit code. A failed lifecycle row gives
`level: "protocol"`, `outcome: "fail"` and exit 8.

A skipped check is never a pass. A run in which everything was skipped reports `fail`.

## `--negative`: the wrong jar must be refused

A connector jar is pinned to one game version. `--negative` proves the pin is real rather
than decorative: the deployed jar is moved aside, the **sibling** target's jar is copied in
its place, and the server is booted again.

- The sibling is the other non-retired target on the same platform. Only Fabric has one
  today, so Paper and NeoForge rows are `skip` with the reason.
- The sibling's artifact has to be in the build manifest and on disk. A CI leg only
  downloads its own target's `dist`, so there the row is `skip` — this check is for a local
  run over several targets.
- The row passes when the server refused the jar **and** no identify frame arrived in the
  window. A refusal is either Fabric Loader's (`Incompatible mods found!` /
  `Mod resolution failed`, with the `requires version … of '…'` detail line) or the
  connector's own guard (`Takaro target-check: …"result":"refuse"`). `refusedBy` records
  which, and `exitCode` the code the refused container exited with.
- A jar that identifies is a failure, not a curiosity.

Afterwards the wrong jar is deleted and the real one moved back.

## `--takaro hosted`: one real registration per target

`--takaro hosted` replaces the local fake with a real Takaro. It reads six environment
variables — `TAKARO_WS_URL`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_HOST`, `TAKARO_USERNAME`,
`TAKARO_PASSWORD`, `TAKARO_DOMAIN_ID` — and if any is missing it prints the **names** and
exits 2. It is never run in CI and never given to a workflow.

What it asserts, and nothing beyond it:

- `hosted-registration`: the connector registers its **own** gameserver under the identity
  `takaro-maint-<game>-<target>-<run id>-<nonce>`. The harness never pre-creates one; it polls
  until exactly one exists. Before the boot it deletes every gameserver whose identity starts
  with `takaro-maint-<game>-<target>-`, so a crashed earlier run leaves nothing behind — which
  also means two hosted verifications of the same target must not run at the same time.

  The identity has to be new on every run: Takaro answers a registration under an identity
  whose gameserver has been deleted with `409`, so a fixed one would work exactly once. That is
  why the run id and a nonce are part of it.
- `heartbeat`: Takaro's own reachability probe answers `connectable: true`.
- `players`: the new gameserver lists no players.
- `shutdown`: a shutdown asked for through the API stops the container with exit code 0.

The catalogue, console and lifecycle rows are skipped with their reason, so a hosted report
reaches `level: "startup"` by design. Every gameserver the run created is deleted again in a
`finally`, including after a failure.

Nothing identifying survives. The logs are redacted **before** the report is built, so
`logs[].sha256` describes the bytes that were kept, and the report is redacted right after
it is written. Every UUID becomes `<uuid>`, the API and websocket hosts and the account
values are replaced with `<redacted>`, and the gameserver id is stored as `"<redacted>"`
from the start rather than being scrubbed later.

## The report

`<out>/<target>/report.json` validates against
`catalog/schema/v1/verify-report.schema.json`.

| Field | Where it comes from |
|---|---|
| `source.{repo,revision,dirty}` | The checkout the command ran from. |
| `target.{game,id,fingerprint,inputs}` | The catalog record, with each input's resolved URL and hash. |
| `artifacts[]` | The build manifest's rows for this target: role, file, sha256, connector version. |
| `runtime.image.{ref,digest}` | The catalog's pinned container. |
| `runtime.{gameVersion,loader,loaderVersion}` | The platform banner the server actually printed — `Loading Minecraft … with Fabric Loader …`, `This server is running Paper version …`, `NeoForge mod loading, version …, for MC …`. |
| `runtime.java` | The Java release the target pins. |
| `checks[]` | `{id, status, durationMs, detail, log}` per row, in run order. |
| `level`, `outcome` | As above. |
| `logs[]` | Every retained log file with its sha256. |
| `coverage.gameplay` | Always `not covered - recorded client evidence pending (#174)`. |

A failure names its target three times over: the report path is `<out>/<target>/report.json`,
stdout's `reports[].target` carries the id, and the CI leg's name contains it.

## Isolation

- Data directories are `mktemp -d` and are removed in a `finally` (kept only with
  `--keep-on-failure`, and then the path is printed).
- No host port is ever published. The fake Takaro binds the docker bridge gateway, and the
  container reaches it through `host.docker.internal`; `docker.log` records
  `HostConfig.PortBindings` for every boot so this is checkable after the fact.
- The registration token is a throwaway generated per run and is redacted out of
  `docker.log`; the fake logs token payload fields as `<redacted>`.
- Every container carries `tm.run` and `tm.ttl` labels. `--cleanup-orphans` clears a
  previous run with the same id.
- `SIGINT`/`SIGTERM` removes every container and data directory, then exits 130. A removed
  container reports itself dead, so a check that was polling stops immediately rather than
  waiting out its budget.

## In CI

`.github/workflows/connector-release.yml` runs a `verify` matrix job between `build` and
`publish`, one leg per catalog target, `fail-fast: false`. Each leg downloads its own
`dist-<connector>-<target>`, restores the build leg's fingerprint-keyed cache, runs the same
command with `--startup-timeout 600 --cleanup-orphans`, writes a check table into the job
summary, and uploads `verify-<connector>-<target>` (report plus logs) even when it failed.
`publish` needs every leg to succeed.

## What this does not prove

This harness exercises startup and the Generic Connector Protocol. It does not play the
game. Join and leave, chat, inventory, item grants and teleportation need a real client and
recorded evidence, which is tracked separately (#174), and every report says so in
`coverage.gameplay`. No document in this repository should read as if this command had
verified them.
