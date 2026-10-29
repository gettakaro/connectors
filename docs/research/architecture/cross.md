# Connector architecture research: native feasibility, threading, state and recovery (all games)

Refs #298 (parent epic #297). Research only: no production code, target or release metadata
changes. Probe code lives under [`probes/`](probes/) and is not referenced by any connector
build, CI workflow or release.

- **Source revision:** `3859fd8aaabdd0c391153671f8bb2f0c0e10d1ba` (main, 2026-10-23). The issue's hypotheses were
  written against `147acb21`; 173 commits and 995 changed files under `games/` separate the two.
- **Statuses** follow the issue: `passed`, `failed`, `inconclusive`, `blocked`, `not applicable`.
  "Source-only" marks a finding read from code, not observed at runtime.
- **Per-game source reports:** [`source/`](source/), one file per game with `path:line`
  references at the source revision above, plus [`source/native-runner.md`](source/native-runner.md)
  comparing the four native game-thread runners.

## Rescope against current main

The issue's premise has moved. What changed, and how this report handles it:

| Issue assumption (at `147acb21`) | Current main | Effect on this research |
| --- | --- | --- |
| Several sidecar connectors are candidates for going native (Conan, Dragonwilds, Enshrouded, VEIN, Terraria) | Conan (3.4.0), Dragonwilds (0.5.0), Enshrouded (0.8.1, sidecar retired in 0.6.0) and VEIN (0.7.0) already ship in-process native connectors. Their `sidecar/` folders are legacy, and none is built or shipped. | Q1 now asks this only of **Terraria** (TShock plugin + Node bridge) and **Dune** (battlegroup sidecar, exempt). For the already-native four, the question becomes whether renters can install them (C1). |
| Eleven baseline families | Also maintained in the catalog: **SCUM** (UE4SS, Windows), **TF2** (SourceMod) and **ARK** (`LD_PRELOAD`, candidate). There is no Palworld connector in this repo. | SCUM, TF2 and ARK are appended as new families, with the same source-level coverage. |
| DRA-2: the shared native runner passes stack references into work that outlives a timeout | Fixed for Dragonwilds and VEIN (owned captures, ASan test). **Still true for Dune.** Conan uses a different design. | Re-run as experiment X1 on all four copies. |
| VAL-1/VAL-4 companion inventory and position | Companion position reporting removed (#353). | VAL-4 is reduced to the server-only path. |

## Environment

| Item | Value |
| --- | --- |
| Host | Linux x86_64 build host, 8 cores. Every probe ran in a container. No game server and no game client ran. |
| Native probes | `games/dune/mod/Dockerfile.build` image (debian:bookworm, g++ 12) with `-fsanitize=address` |
| Dragonwilds/VEIN runner tests | Each connector's own `tests/run-timeout.sh` (its build image, ASan) |
| Zomboid probe | `eclipse-temurin:25-jdk` compiling the production `MainThreadQueue.java` |
| Terraria probe | Built with `mcr.microsoft.com/dotnet/sdk:9.0`, run on `ghcr.io/pryaxis/tshock@sha256:70e59a8e…f7e7`, the exact runtime image pinned by catalog target `tshock-v6.2.1` (.NET 9.0.20, Debian 12) |
| Takaro | Staging connector endpoint, used only for an identify that carries a deliberately invalid registration token. Takaro answered `invalid_args`; no game server was created and no token was read or regenerated. |

Connector versions and default catalog targets at the source revision:

| Game | Version | Default target (status) | Placement on main |
| --- | --- | --- | --- |
| 7 Days to Die | 0.5.0 | `linux-3.3.0.b18` (maintained) | Managed mod (Mono), built-in `Mods/` loader |
| Rust | 0.3.0 | `carbon-25902425` (candidate) | Carbon script plugin. Oxide is not in the catalog. |
| Minecraft | 0.5.0 | `paper-1.21.11`, `fabric-26.2`, `neoforge-1.21.11` (maintained) | Plugin/mod jar per loader, shared core client |
| Valheim | 4.2.0 | `linux-1.0.20` (candidate) | BepInEx managed plugin |
| Terraria | 0.6.0 | `tshock-v6.2.1` (maintained) | TShock plugin + **Node sidecar** (`bridge/`) |
| Project Zomboid | 1.4.0 | `linux-42.21.0` (candidate) | JVM agent (`-javaagent`) |
| Enshrouded | 0.8.1 | `proton-1267041` (candidate) | Native `dbghelp.dll` proxy under Proton |
| RuneScape: Dragonwilds | 0.5.0 | `linux-25991736` (candidate) | Native `.so`, `LD_PRELOAD` |
| VEIN | 0.7.0 | `linux-25708939` (maintained) | Native `.so`, `libsteam.so` drop-in or `LD_PRELOAD` |
| Conan Exiles | 3.4.0 | `linux-25792439`, `windows-25792439` (candidate) | Native `.so` (`LD_PRELOAD`) / `winmm.dll` proxy |
| Dune: Awakening | 0.4.0 | `linux-26051014` (candidate) | **Sidecar** (Node) + optional read-only native map plugin |
| SCUM (new) | 0.1.0 | `windows-25803875` (candidate) | UE4SS `dwmapi.dll` proxy + native `main.dll` + Lua |
| TF2 (new) | 0.1.0 | `linux-25995664`, `windows-25995664` (candidate) | SourceMod plugin + two native SM extensions |
| ARK (new) | 0.2.0 | `linux-21241282` (candidate) | Native `.so`, `LD_PRELOAD` |

## Go/no-go per question

| # | Question (issue #298) | Answer | Verdict | Evidence |
| --- | --- | --- | --- | --- |
| Q1 | Which sidecar connectors can become practical in-process installs? | Only **Terraria** remains a candidate. Its transport gate passed on the exact TShock runtime (X3). Loader and installability are already met, because the plugin DLL is the standard TShock route. Safe game access is not met: no game-thread scheduler exists today, and the REST-invoked give/teleport probably runs off the game thread (source-only). **Dune** keeps its sidecar: battlegroup topology, Postgres/RabbitMQ, and no game-owned bans. | Terraria: **conditional go** (gates 3 and 4 open). Dune: **retain sidecar**. | X3, `source/terraria.md`, `source/dune.md` |
| Q2 | Where is game-thread time spent; what can move to snapshots/workers? | No connector bounds a single game-thread job. Zomboid and Minecraft do not bound a drain at all, 7D2D (32) and Valheim (64) bound it by job count only, and the native C++ runners add a 500 µs budget that is checked only *between* jobs. One slow job (catalog scan, give loop, map survey) is therefore unbounded everywhere. Per-line synchronous log file I/O runs on game or hook threads in Zomboid, Enshrouded and SCUM. Catalog builds and JSON serialization run on the main thread in Rust and Valheim. | **Partial.** Source map done. No tick measurement under load (blocked: no live run this round). | X2 (Zomboid drain cost), `source/*.md` §4 |
| Q3 | Which data needs a mirror, which needs persistence, which stays game-owned? | Bans stay game-owned wherever the game has them. Connector-owned timed-ban metadata is the one category that needs durable storage, and several stores are fragile (Zomboid rewrites in place, deletes a row even when the unban failed). Event outboxes are memory-only in most connectors, and durable only in VEIN, Dragonwilds and Conan. The 7D2D in-memory LiteDB mirror is the only database. No evidence favours adding one elsewhere. | **Go for "no new database".** Durable outbox: decision needed (protocol). | `source/*.md` §6–7 |
| Q4 | What fails under load, delay, disconnect, restart, world change? | **Every connector can apply a mutation after Takaro was told it failed** (X1, X2, source for the rest). A retry can then double-apply a give or ban. **Dune can corrupt game-thread memory** when a started job outlives its caller (X1). Rust and Minecraft (default config) drop events while disconnected. The Valheim receive loop ignores message completion (source-only). | **Failed** (defects found; see follow-ups). | X1, X2, `source/*.md` §5, §7 |
| Q5 | What should be shared; what stays game-specific? | Real duplicates with drift: the WinHTTP transport (Enshrouded, SCUM, Conan Windows), the Linux native runner (Dragonwilds, VEIN, Dune, and Conan as a redesign), and the Java core client (Minecraft → Zomboid). Share **acceptance fixtures first** (late-mutation, fragmented-frame, stalled-pump, blocked-send). Extract code only where two callers exist and the fixtures pass. | **Go** for shared fixtures; **conditional** for shared code. | `source/native-runner.md`, `source/*.md` §8 |
| Q6 | What guidance, diagnostics and gates enforce this? | Mechanically checkable: owned captures in native runners (ASan timeout test as in Dragonwilds/VEIN), bounded queues with an explicit cap, a reject-expired-at-dequeue test, `EndOfMessage` assembly tests. Needs runtime evidence: tick budgets, outbox durability. Docs that are stale: Zomboid README version and premain note, Enshrouded DEVELOPMENT target, Dune gamethread-policy (still describes a Tick pump), Rust `connector.json` env hint. | **Go** (proposals below; maintained docs not edited). | `source/*.md` §9 |

## Experiments run

| ID | Issue item | How it was run | Result | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| X1a | C4 / DRA-2, DUN-2 | [`probes/native-runner/run.sh dune`](probes/native-runner/run.sh): production `games/dune/mod/src/gamethread.cpp` under ASan; the game thread starts a `RunJson` job, the caller times out (200 ms) and returns, then the job finishes | ASan `SEGV` in `RunJson`'s lambda (`gamethread.cpp:323`): the job assigns into the returned caller's `result`. Production callers also capture by reference at `http.cpp:260` and `events.cpp:2021`. | **failed** (defect) | `native-runner-asan.log` |
| X1b | C4 | `probes/native-runner/run.sh conan`: production Conan runner, slow job (1.5 s) against a 200 ms timeout; expired queued job; 600 submitters while the pump is stalled | `Run` returned `true` after **1521 ms**: a started job is always waited for, with no upper bound. The expired queued job was cancelled with no effect. **All 600** queued jobs were admitted (no cap; Dune and Dragonwilds cap at 256). | **passed** for lifetime; **failed** for boundedness | `native-runner-asan.log` |
| X1c | C4 / DRA-2, VEI-2 | Each connector's own `tests/run-timeout.sh` (ASan, `detect_stack_use_after_return`) | `game-thread timeout ownership: passed` for Dragonwilds and VEIN: the issue's claim is fixed there | **passed** | `native-runner-owned-tests.log` |
| X2 | C4 / ZOM-2 | [`probes/zomboid-timeout/run.sh`](probes/zomboid-timeout/run.sh): production `MainThreadQueue` + the `handleOnMainThread` timeout pattern (`TakaroWebSocketClient.java:669-690`), timeout scaled from 10 s to 200 ms; 50 give-like requests while the tick is stalled, then one drain | 50/50 reported failed to Takaro, then **50/50 still applied** on the next tick. 200,000 tasks were admitted and drained in **one tick (85–92 ms)**. | **failed** (defect) | `zomboid-late-mutation.log` |
| X3 | C7 / TER-2 transport gate | [`probes/terraria-wss/run.sh`](probes/terraria-wss/run.sh): `ClientWebSocket` on the TShock target's own .NET 9.0.20 runtime, against the Takaro staging connector endpoint | WSS upgrade (639 ms cold, 135 ms warm), identify round trip (`identifyResponse` `invalid_args`, as intended), clean close, and reconnect on a fresh socket all worked. DNS failure, self-signed, wrong-host and expired certificates were all **refused** with default validation. | **passed** | `terraria-native-transport.log` |
| X4 | P1–P3, C1, C3, C5, C6, C8 | Per-game source reading at the revision above (four parallel reviewers, the main risks re-read by hand) | 15 reports in [`source/`](source/) | source-only | `source/*.md` |

### Experiments not run this round

| Issue items | Status | Why | Next bounded experiment |
| --- | --- | --- | --- |
| C2 (real-client baseline) for all games | **blocked** | No game client or server ran in this offline round. Recent per-target live proofs exist from release work, but were not re-run against this revision. | Per game, the existing `takaro-maint verify` plus a one-client smoke on its default target. Two-player recipient and identity checks are **not testable with one client**. |
| C3 tick/frame p50–p99 with no connector, idle and loaded | **blocked** | Needs a live server, a client and a load generator per game | Start with Zomboid (X2 shows the drain cost) and Valheim. Run 10 min windows ×2, 1 client, a fixed request rate. |
| C6 crash/restart with numbered events | **blocked** | Needs live servers | VEIN and Dragonwilds have durable outboxes. Compare them against Rust or Minecraft with v0 (no outbox), each with 100 numbered chat events and a socket cut. |
| C1 renter installation on a real rental | **blocked** | No rental account is available (the issue forbids buying one) | Documented classification below. |
| C5 LiteDB assessment (DTD-4) | **inconclusive** | Source-only: the mirror is in-memory and the evidence shows no need for persistence | None unless a persistence need is shown |
| Terraria gates 3 and 4 (game access, responsibilities) | **inconclusive** | Which thread runs TShock REST commands needs a decompile or a live trace | Live trace: log `Thread.CurrentThread.ManagedThreadId` in `/takarogive` against the GameUpdate hook thread |

## Renter installation (C1, documented, untested on a rental)

| Class | Games | Host control needed | Verdict |
| --- | --- | --- | --- |
| Managed plugin or mod in the game's own loader | 7D2D (`Mods/`), Rust (Carbon/Oxide `plugins/`), Minecraft (Paper/Fabric/NeoForge jar), Valheim (BepInEx), Terraria plugin half (TShock `ServerPlugins/`) | File upload + restart. The loader must be offered by the host. | documented but untested |
| Script plugin + native extensions in the game's extension loader | TF2 (SourceMod `.smx` + two extensions) | Upload into `tf/addons/`, including native binaries | documented but untested |
| Proxy DLL dropped next to the executable | Conan Windows (`winmm.dll`), SCUM (`dwmapi.dll` + `main.dll`), Enshrouded (`dbghelp.dll`, **plus** `WINEDLLOVERRIDES` under Proton), VEIN panel route (`libsteam.so`) | Write access to the binaries directory. Hosts that validate or reset game files will remove it. | requires host cooperation (Proton: env control) |
| `LD_PRELOAD` native library | Dragonwilds, Conan Linux, ARK, VEIN (alternative route), Dune map plugin (optional) | Launch-environment control | requires host cooperation |
| JVM agent | Zomboid (`JAVA_TOOL_OPTIONS` or `vmArgs`; SteamCMD validate reverts `vmArgs`) | Env var or JVM-args control | requires host cooperation |
| Extra process | Terraria bridge (Node), Dune sidecar | A second supervised process | requires host cooperation |

## Per-game recommendation (C9)

Effort: S is under 1 week, M is 1–3 weeks, L is more than 3 weeks, each for one engineer familiar with the connector.
Confidence is high where a probe ran, and medium where the finding is source-only.

| Game | Placement | Must fix first | Effort | Confidence |
| --- | --- | --- | --- | --- |
| Terraria | **Migrate after gates**: plugin-only connector with a GameUpdate scheduler, in-process outbox and hook-fed events. Keep the bridge until the live parity run passes. | Move REST-invoked mutations onto the game thread. Persist the log cursor or replace it with hooks. | M | transport: high; game access: medium |
| Dune | **Retain sidecar** | Owned captures in `GameThread::Run`/`RunJson` (X1a). Off-thread UObject reads in the presence sweep. | S | high |
| Zomboid | Keep (JVM agent) | Expire-at-dequeue + bounded queue (X2); async logging; atomic ban-store writes | S–M | high |
| Conan | Keep (native) | Catch exceptions around jobs; bound the queue and the post-timeout wait (X1b); total deadline for give | S | high |
| Dragonwilds, VEIN | Keep (native) | "Outcome unknown" reporting for started mutations; per-job time accounting | S | high (runner), medium (rest) |
| Enshrouded, SCUM | Keep (native) | Cancellable give; port Conan's abortive-close dedupe fix; one shared WinHTTP transport | M | medium |
| Rust | Keep (Carbon plugin) | Bounded single-writer sender instead of `Task.Run` + lock; outbox while disconnected | M | medium |
| Minecraft | Keep (per-loader jars) | Request expiry + bounded scheduler hop; outbox independent of `map=true` | M | medium |
| Valheim | Keep (BepInEx) | Assemble frames until `EndOfMessage`; per-request deadline | S | medium |
| 7D2D | Keep (Mods); in-memory LiteDB mirror stays ephemeral | Caps on the three ingress queues; separate log events from player events in the outbox; one transport item raised with the maintainers directly | M | medium |
| TF2 | Keep (SourceMod) | Exercise v1 replay and acks in the harness; certificate-refusal probe for the WS extension | S | medium |
| ARK | Keep (`LD_PRELOAD`, candidate) | Idempotent give; per-tick time budget | M | medium |

## Proposed follow-up issues (not filed)

1. **Protocol: report "outcome unknown" for mutations that started before a timeout**
   Every connector can apply a give, teleport or ban after Takaro received a failure (X1, X2).
   Decide a protocol-level error code or an idempotency key (`requestId`) with Takaro.
   Add one shared acceptance fixture: stall the game thread, time out, resume, count effects.
2. **Dune: owned captures in the native game-thread runner**
   `RunJson` and two `Run([&]…)` callers let a started job write into a returned stack frame (X1a).
   Port Dragonwilds' owned-result `RunJson` and the `game_thread_timeout_test.cpp` ASan test.
   Fix the 256/257 queue-cap off-by-one while there.
3. **Zomboid: expire queued main-thread tasks and bound the queue**
   Timed-out tasks still run, the queue is unbounded, and a tick drains everything (X2: 200k in 85–92 ms).
   Check the deadline at dequeue, cap admission, and add a per-tick time budget.
   Make `BanStore` writes atomic and remove the expiry row only after a successful unban.
4. **Conan: bound the game-thread runner**
   No queue cap (600/600 admitted while stalled), an unbounded wait after a timeout, and an uncaught job exception (X1b).
   Cap the queue, catch around `job->fn()`, and alarm when the post-timeout wait exceeds N × the timeout.
   Add a direct unit test for `GameThread::Run`; none exists today.
5. **Terraria: plugin-only connector proof (sidecar removal)**
   Transport gate passed on the exact runtime (X3). Prove game-thread access and in-plugin events next.
   Bounded proof: identify + getPlayers + chat event from the plugin, against the bridge as baseline.
   Measure what a native connector loses on game crash compared with the bridge's in-memory cursor.
6. **Valheim: assemble WebSocket messages until `EndOfMessage`**
   The receive loop parses each 32 KiB `ReceiveAsync` chunk as a whole message (source-only).
   Accumulate up to a byte cap, reject oversize with a close, and add a fragmented-frame test.
   Add a per-request deadline so a stalled main thread doesn't queue mutations invisibly.
7. **Shared WinHTTP transport for Enshrouded, SCUM and Conan Windows**
   Three drifted copies. Conan's abortive-close duplicate-delivery fix is missing in two of them.
   Extract one tested transport, starting from Enshrouded's `native-wine` CI job as the gate.
   Keep game adapters separate.
8. **Event delivery while disconnected for Rust and Minecraft (default config)**
   Both drop events silently whenever the socket is down. Minecraft's outbox exists only with `map=true`.
   Add a bounded in-memory outbox with v1 acks, as Conan, VEIN and Dragonwilds have.
   Count drops in the health output.
9. **Engineer guidance: game-thread and queue rules with executable checks**
   Rules: owned captures, bounded queues, expire-at-dequeue, time budget per job, no disk I/O on game threads.
   Map each rule to a test seam (ASan timeout test, stalled-pump fixture, fragmented-frame fixture).
   Fix stale docs: Zomboid README/DEVELOPMENT, Enshrouded target, Dune gamethread-policy, Rust `connector.json`.
10. **Live measurement campaign (C2/C3/C6) per default target**
    One-client tick profiles (none / idle / loaded) and numbered-event outage runs, game by game.
    Record a run budget per experiment as the issue requires. Two-client checks stay out of scope.
    Start with Zomboid, Valheim and Terraria, where the decisions above depend on it.

## Limitations

- No game server or game client ran. Every gameplay, tick and crash statement is source-only or
  comes from an isolated harness that runs production queue code.
- Probe timeouts are scaled down (10 s → 200 ms) and every probe runs with one synthetic caller set.
  This is not busy-server evidence.
- The issue's reporting steps (a progress index, a report comment per game and a synthesis comment
  on the issue and the epic) were not posted. This document and its `source/` appendices hold that
  content for review.
