# Enshrouded: source-level architecture analysis (issue #298)

Everything here is **source-only** at HEAD 3859fd8: no game, Wine or test was run. Paths are relative to the repo root.
`E` = `games/enshrouded/mod/src`.

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | **In-process.** One `dbghelp.dll` proxy inside `enshrouded_server.exe`, holding the Takaro WSS connection itself. **No sidecar process** since 0.6.0 | games/enshrouded/DEVELOPMENT.md:7-23; INSTALL.md:3-5 |
| Loader | The exe imports only `MiniDumpWriteDump` from dbghelp. The proxy forwards it to `%SystemDirectory%\dbghelp.dll` and starts `InitThread` from `DllMain` (no work under the loader lock) | E/dllmain.cpp:1-2, 15-29, 46-54; DEVELOPMENT.md:72-75 |
| Wine/Proton load path | `WINEDLLOVERRIDES=dbghelp=n,b` is required, because Wine's builtin dbghelp wins otherwise. The compose file bind-mounts `takaro/plugin/dbghelp.dll` to `/opt/enshrouded/server/dbghelp.dll` | catalog/enshrouded/targets/proton-1267041.json:61; games/enshrouded/docker-compose.example.yml:45,68-69; dev-servers/compose/enshrouded.yml:50,79-80; INSTALL.md:46 |
| Runtime / toolchain | C++17 cross-compiled with zig 0.13 to x86_64-windows-gnu. MinHook is vendored. Links `ws2_32 winhttp crypt32 bcrypt` | E/../build.sh:2,19; catalog/.../proton-1267041.json:78-83 |
| Transport | **WinHTTP, async mode** (`WINHTTP_FLAG_ASYNC`): `WinHttpOpen` → `WinHttpConnect` → `WinHttpOpenRequest(..., WINHTTP_FLAG_SECURE)` → `WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET` → `WinHttpSendRequest`/`ReceiveResponse` → status must be 101 → `WinHttpWebSocketCompleteUpgrade(e->req, (DWORD_PTR)e)`. Send and receive use `WinHttpWebSocketSend`/`Receive`, and close uses `WinHttpWebSocketShutdown(1000)` | E/native/transport_winhttp.cpp:454-455, 501-504, 512-513, 534-542, 669, 728, 606 |
| TLS verification | WinHTTP default validation (system trust) with TLS 1.2 plus 1.3 (it falls back to 1.2 only if the 1.3 flag is refused). With `caFile`, **only** `SECURITY_FLAG_IGNORE_UNKNOWN_CA` is set; the chain is then rebuilt and must end in a root from the CA file, and `CertVerifyCertificateChainPolicy(SSL)` checks the host name before identify is sent. An unreadable CA file fails closed. Only `wss://` URLs are accepted. **No proxy support** (`WINHTTP_ACCESS_TYPE_NO_PROXY`) | transport_winhttp.cpp:58, 121-168, 496-499, 506-511, 516-523, 454; DEVELOPMENT.md:37-38 |
| Default catalog target | **`proton-1267041`**: `default: true`, but `support.status: "candidate"` with `evidence: []`. The only other target, `proton-1024233`, is `retired`, so **no target has status `maintained`**. Runtime image `mornedhels/enshrouded-server:dev-proton@sha256:d122801b…` (GE-Proton11-9) | catalog/enshrouded/targets/proton-1267041.json:3-13,57-65; proton-1024233.json:7-9 |
| Connector version | `0.8.1` | games/enshrouded/version.txt:1; E/common.h:14 |
| Catalog platforms | `["proton"]` only. README claims a native Windows test (README.md:7-9,20), but no Windows target exists | catalog/enshrouded/game.json:6 |
| Doc drift | DEVELOPMENT.md still describes `proton-1024233` as "the catalog target" and says "pinned to build 1024233" | DEVELOPMENT.md:79, 92, 386 |

## 2. Drift vs the issue (ENS-1..4, written against 147acb2)

| Issue hypothesis | Status at HEAD |
|---|---|
| ENS-1 "planning target used GE-Proton10-30 and a dbghelp.dll proxy" | **Changed.** GE-Proton10-30 now belongs to the retired `proton-1024233` target (proton-1024233.json:43,50). The default target uses GE-Proton11-9, and 10-30 cannot start 1.0 (proton-1267041.json:118). The proxy is still used. |
| ENS-1 "neither runtime nor probe was available… compile a WinHTTP WSS probe" | **Obsolete as a question.** The production WinHTTP WSS transport exists (transport_winhttp.cpp) and grew out of a "G0" probe (transport_winhttp.h:1; mod/tests/fake_takaro_ws.py:4). It runs in CI under the pinned Proton image against a fake TLS Takaro (`.github/workflows/enshrouded.yml:88-119`). The probe source itself (`mod/probe/`) is no longer in the tree. |
| ENS-2 DLL placement, overrides, config discovery, updates | **Still true.** Renters still need `WINEDLLOVERRIDES` under Proton (INSTALL.md:46) and must disable game auto-update (INSTALL.md:48-49). Config is now one file, `takaro/plugin.json`, re-read live (DEVELOPMENT.md:340-361). |
| ENS-2 "Native Windows is a separate target and needs separate evidence" | **Still true.** README claims a Windows test (README.md:20), but the catalog has `platforms: ["proton"]` and no Windows target (game.json:6). |
| ENS-3 Tick, moderation and player-update queues | **Still true.** All 3 queues exist (§5) and are still unbounded with a count-only drain. A bridge job queue (128) and 4 action workers now sit in front of them. |
| ENS-3 "delayed/time-out mutation execution" | **Partly mitigated.** A queued task is cancelled on timeout. A started task still runs to completion, and multi-step `giveItem` keeps granting after the 30 s Takaro error (§5). |
| ENS-4 event loss, log fallback, saved identities, reconciliation, crash | **Changed.** The sidecar's poller, cursor and reconciliation were ported in-process (bridge.cpp:782-960). The durable outbox, `known-players.json`, timed bans and the ban-intent journal are new (DEVELOPMENT.md:55-64). |
| ENS-4 "minimal direct-connection proof, preserving the current split as baseline" | **Obsolete.** The split is gone. `sidecar/` is kept only for parity fixtures and rollback, and is not packaged (DEVELOPMENT.md:16-19,68-69). |

## 3. C1 install (renter)

**Class:** an **arbitrary native library** (a Windows PE DLL that hooks game code via MinHook), not a managed or script plugin. It is a single file plus a config file. No extra process is needed.

| Step | Detail | Ref |
|---|---|---|
| Files | Drop `dbghelp.dll` next to `enshrouded_server.exe`. (`takaro-maint deploy` instead places it at `takaro/plugin/dbghelp.dll`, and compose mounts it over the exe directory.) | INSTALL.md:18; DEVELOPMENT.md:225-228; proton-1267041.json:86-91 |
| Config / state (writable) | `takaro/plugin.json` is auto-created with a UUID identity and holds secrets. Writable `takaro/connector-state/`, `takaro/plugin.log`, `takaro/accounts.json` and `takaro/enshrouded-gamedata.json` are also needed | INSTALL.md:19-45,73-76; DEVELOPMENT.md:55-60,74-75,138-140 |
| Env / launch | **Proton/Wine: `WINEDLLOVERRIDES=dbghelp=n,b` is mandatory.** On Windows no environment is needed (README.md:152). Token env vars are optional | INSTALL.md:41-46 |
| Restart | Stop the server to replace the DLL, because a running server holds it open. Token, URL, name and identity changes apply live; `caFile`, `token` and `mapDir` need a restart | DEVELOPMENT.md:228-231,307-308,353-361 |
| Update pinning | The game build must not auto-update: hooks are signature-pinned, and a new build degrades capabilities | INSTALL.md:47-49; DEVELOPMENT.md:116-123 |
| Network | Outbound WSS only, to `wss://connect.takaro.io/`. No proxy. Diagnostics listen on 127.0.0.1:18890 only | DEVELOPMENT.md:15,362; transport_winhttp.cpp:454 |

**Host control needed:**
- **Windows host:** file upload into the executable directory. This is a native DLL, so the provider must permit arbitrary native code.
- **Proton/Linux host:** additionally the ability to set a Wine DLL override in the game process environment, which **requires host cooperation**.
- **Status:** documented but untested on a rental. The provider policy is unknown.

## 4. C3 thread trace (source only)

| Path | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| **getPlayers** (adapter.cpp:78,93 → `GET /players` → state.cpp:310-322) | **None.** It reads the `PluginState` snapshot under `lock_`. That snapshot is fed by the game's log-sink hook (hooks.cpp:46-66 → state.cpp:156) | Action worker, under `lock_`, which is shared with the log-sink hook (state.cpp:311) | Bridge thread → `FrameQueues` → per-epoch send thread (bridge.cpp:685; transport_winhttp.cpp:648-693) | none | worker/bridge `PluginLog` |
| **teleportPlayer** (adapter.cpp:194-198 → http.cpp:394 → world.cpp:1067-1115) | **Game main thread**, in the WorldTask drained in `UpdDetour` after `Server::updatePlayers` (world.cpp:150-175). It calls `g_addComp` on live ECS memory | Tiny result built on the game thread; response JSON built on worker/bridge | send thread | none | **`PluginLog` from the game thread** (world.cpp:1102) |
| **giveItem** (adapter.cpp:140-156 → http.cpp:405 → world.cpp:1179-1251) | Entity lookup runs on the main-thread WorldTask (world.cpp:1187-1193). Creation is a **global single-slot injection** (`g_give`) consumed in `InvJobDetour` on whatever thread runs the inventory ECS job (world.cpp:341-347,1218-1226). The worker **polls `Sleep(10)` for up to 4 s per create action, at most 64 actions** | worker | send thread | none | worker `PluginLog` |
| **chat-message event** (world.cpp:359-381) | The `AddMsgDetour` hook thread (the chat system). It reads server slots through a raw `g_server` pointer (world.cpp:364-370) | **Built on the hook thread** (JSON string, world.cpp:372-375). `PlayerJsonByName` takes `PluginState::lock_` | Bridge polls `/events` every 250 ms (bridge.cpp:235-238,782-843) → durable outbox → send thread | outbox file is written on the bridge thread (bridge.cpp:1062-1069) | **`PluginLog` (fopen/fprintf/fclose) on the hook thread** (world.cpp:377) |
| **player-connected event** (hooks.cpp:46-66 → state.cpp:156 → state.cpp:129-154) | The thread calling `keen::log` (log sink). The first login **reads `enshrouded_server.json` from disk under `lock_`** (state.cpp:100-108,137) | Built on the log-sink thread under `lock_` (state.cpp:152) | as above | as above | `PluginLog` on the log-sink thread (state.cpp:153) |
| kick/ban (hooks.cpp:407-488) | Moderation-system detour thread. It temporarily flips the slot permission byte (hooks.cpp:429-443,461-466) | worker | send thread | Ban-intent journal is written **before** the action, on the bridge thread (bridge.cpp:485-512) | `PluginLog` on the moderation thread (hooks.cpp:439,459) |

`PluginLog` opens, appends to and closes `plugin.log` on every call, under a global `g_logLock` (E/common.cpp:24-39). The bridge thread logs every admitted non-`log` event (bridge.cpp:835), so a game-thread hook can wait on that lock behind bridge or worker file I/O.

No worker holds live game objects. Workers pass only names and ids into the game-thread lambdas, and lambdas are captured by value into a `shared_ptr` task (world.cpp:1079,186-187; hooks.cpp:673-674). `g_server` is a raw pointer that hook threads cache (world.cpp:154; hooks.cpp:261), but it is dereferenced only inside hooks and game-thread tasks. One exception is `IsAdminDetour`/`GiveTryInject`, which run on the ECS job thread (thread identity not proven in source).

## 5. C4 queues

| Queue | Capacity / admission | Drain | Timeout behaviour | Ref |
|---|---|---|---|---|
| Inbound notices (transport → bridge) | 4096 frames / 16 MiB (`Frame` only). When full, the transport **drops the connection** | Bridge takes 512 per loop, waiting 50 ms | n/a | bridge.h:59-61; bridge.cpp:152-166,213-222; transport_winhttp.cpp:719 |
| Bridge job queue → 4 action workers | `kMaxPendingActions = 128` user jobs in flight (the 129th is answered `native action queue overloaded`). Request ids are deduped (1024 recent); requestId ≤ 128 B; nesting ≤ 64 | FIFO, 4 workers (`TAKARO_ACTION_WORKERS`) | Deadline is 30 s (`actionTimeoutMs`). A job that never started is CAS-cancelled and never runs. A **running job keeps running**: it is answered `game_error` at 30 s, its late result is dropped (`lateResults++`), but its state changes (bans) are recorded. On disconnect, unstarted jobs of that epoch are cancelled | bridge.h:62-63; bridge.cpp:444-469,476-520,253-277,689-727,321-339; config.h:32-34 |
| Game "Tick" queue `g_queue` (session update) | **Unbounded `std::deque`**, with no size check on push | **16 tasks per tick, count only, no time budget** | 5 s default. A queued task is CAS-cancelled; a started one finishes and its result is dropped | hooks.cpp:74-128,671-683 |
| Moderation queue `g_modQueue` | **Unbounded** | **8 per moderation-system call**, count only | 3 s / 5 s; same cancel semantics | hooks.cpp:253-276,391-403,472-480 |
| World/player-update queue `g_wq` (after `updatePlayers`) | **Unbounded** | **16 per call**, count only | 3 s; same CAS cancel (`"did not finish"` when it was already running) | world.cpp:133-202 |
| giveItem injection slot `g_give` | 1 slot, serialized by `g_giveSerial` | consumed by `InvJobDetour` | **Not cancellable.** The worker loop continues after the 30 s bridge deadline, so with 64 actions × 4 s, **items can still be granted up to ~256 s after Takaro got an error**. Not idempotent if Takaro retries | world.cpp:1184,1209-1249 |
| Outbound `FrameQueues` (per kind, strict priority) | Control 64 / 1 MiB; CriticalResponse 256 / 1 MiB; Response 256 / 32 MiB; Event 1024 / 8 MiB. One frame ≤ 8 MiB, one inbound message ≤ 1 MiB. A full Response queue → **close connection** | One send outstanding per epoch, waiting up to 30 s for WRITE_COMPLETE before the link is marked dead | Frames for an older epoch are refused (`StaleEpoch`) | transport.cpp:16-37; transport.h:56-57; bridge.cpp:544-561; transport_winhttp.cpp:648-693 |
| Plugin event ring | 5000 events in memory. The oldest are dropped, and the bridge counts `ringGaps` | Bridge reads 512 × ≤ 8 rounds every 250 ms | n/a | state.cpp:7,44-50; bridge.cpp:785-843 |

**Lifetime:** game tasks own their callable and arguments (`shared_ptr<GameTask/WorldTask>`), so **no started job can outlive caller stack data**. This differs from the shared native runner risk in DRA-2 (hooks.cpp:74-100; world.cpp:186-187).

**Unbounded game queues in practice:** live entries are effectively limited by the number of blocked callers (4 workers plus the bridge thread). Cancelled entries stay in the deque until popped.

**Shutdown:**
- `Bridge::Stop` joins the workers (bridge.cpp:136-150). A worker inside `giveItem` blocks it for up to ~256 s.
- **Nothing calls `Stop` at process exit.** There is no `DLL_PROCESS_DETACH` handling (dllmain.cpp:46-55).
- A forced `FlushState` happens only on a Takaro `shutdown` action (bridge.cpp:683) or on bridge stop (bridge.cpp:249).

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation |
|---|---|---|---|---|
| Players online | Game log lines, correlated in `PluginState` (state.cpp:129-154) | memory `players_` (state.h:77); bridge `online-players.json` | file (bridge.cpp:1070) | New `bootId` → reconcile. Periodic reconcile every 30 s needs 2 misses (bridge.cpp:793-798,921-960) |
| Last location | Game (live ECS read on main thread) | bridge `lastPositions_` (memory) | none | 60 s connect/disconnect fallback window (bridge.h:64; bridge.cpp:661-673) |
| Inventory | Game (live read, world.cpp:1117-1177) | none | none | n/a |
| Item catalog | Compiled 1.0 tables (`gamedata.cpp`, `itemlabels.cpp`) | static | in DLL | Refused on builds without a gamedata section (proton-1024233.json:14) |
| Identities | SteamID64 from the log. Account hash from the game session table | `accounts.json` (steamId → hash), `known-players.json` (≤ 5000) | file (http.cpp:229-260; persistence.h:65) | never expired |
| Bans | **Game** (no reason, no expiry) | `timed-bans.json`, `ban-intent.json` journal | file, tmp + FlushFileBuffers + MoveFileEx | Bridge lifts expired bans every 5 s and recovers intents at start (bridge.cpp:119; DEVELOPMENT.md:62-64) |
| Event cursor | Plugin ring `seq` per `bootId` | `event-outbox.json` (scan and confirmed cursors) | file | A new `bootId` resets the scan to 0 (bridge.cpp:793-801) |
| Pending events / outbox | connector | `event-outbox.json`, ≤ 5000 events / 32 MiB. When full, the oldest `log` event is evicted, else the oldest event (`losses++`) | **Whole file rewritten** every 250 ms–5 s on the bridge thread | Corrupt file → fenced plus `connectorState` degraded (DEVELOPMENT.md:58-59) |

## 7. C6 delivery

- **Outbox bound:** 5000 events / 32 MiB, with `log` evicted first (persistence.h:63-64; persistence.cpp:254-266). On the transport, the Event queue holds 1024 frames / 8 MiB. When it is full, delivery pauses (bridge.cpp:897-906).
- **Ack semantics:**
  - Protocol 1: `eventAck {streamId, upTo}` → `ConfirmThrough` (bridge.cpp:534-542).
  - Protocol 0: the transport's ping/pong watermark. An event is confirmed only by the pong of a **later** ping, and only counts when `protocol_ == 0` (transport.h:3-8,82-115; bridge.cpp:305-311).
  - On 0 this is **transport receipt, not durable processing**.
- **Reconnect replay:** on `Open`, `lastQueuedId_ = 0`, so all unconfirmed events are resent (at least once). Protocol 1 adds `(streamId, seq)` for dedup (bridge.cpp:289; bridge.cpp:890-891).
- **Lost on game crash:**
  - (a) plugin-ring events not yet polled (≤ 250 ms window, memory only; state.cpp:7);
  - (b) outbox admissions since the last `SaveOutbox` (250 ms–5 s window; bridge.cpp:1065);
  - (c) in-flight requests, whose results Takaro sees as a timeout;
  - (d) a `giveItem` partially granted.
- **Recovered after a crash:** ban intents (journal) and timed bans. Online players are reconciled on the new `bootId`.
- **Clean shutdown:** a server stop not initiated through Takaro gets no forced flush (§5).

## 8. C8 shared vs game-specific

| Enshrouded file | Sibling | Relation |
|---|---|---|
| E/native/transport_winhttp.cpp (769 lines) | games/conan-exiles/native/platform/windows/transport_winhttp.cpp (792) | **Fork, diverged.** 125 diff lines. Conan adds `abortive_close` (socket lookup), a `generation` counter instead of `retargeted_`, and `NativeLog`. Logic is otherwise the same. |
| E/native/transport.{h,cpp} (FrameQueues, Heartbeat) | conan-exiles/native/core/takaro/transport.{h,cpp} | Fork. Conan adds `Heartbeat::OnPongId` (protocol-1 ping ids). About 45 diff lines. |
| E/native/json_util.cpp | conan-exiles/native/core/takaro/json_util.cpp | **Identical** except the namespace and include (6 lines). |
| E/native/fileio.cpp | conan-exiles/native/platform/windows/fileio_win32.cpp | "ported unchanged from the Enshrouded native connector" (fileio_win32.cpp:1-2). Conan split it into a core plus platform file. |
| E/native/bridge.cpp (1191) | conan-exiles/native/core/takaro/bridge.cpp (650) | Different implementations (1075 diff lines). Same concepts: outbox, epoch, workers. |
| hooks.cpp `GameTask` / world.cpp `WorldTask` | dragonwilds/vein/dune `mod/src/gamethread.cpp`, conan `native/core/gamethread.cpp` | **Not shared.** Enshrouded has its own 3 ad-hoc queues. Siblings differ from dragonwilds: vein 26, dune 101, conan 374 diff lines. |
| WinHTTP vs siblings' transport | dragonwilds/vein `native_transport.cpp` use libwebsockets plus OpenSSL (dragonwilds native_transport.cpp:3-4) | Different stack (Linux). Dragonwilds and VEIN copies differ (`diff -q`). |

Game-specific: hooks, world, state (log correlator), moderation, gamedata/labels, and the http.cpp router.

Reuse candidates:
- `src/native/*` is explicitly game-independent (DEVELOPMENT.md:27-29; game_api.h:1-6), so it could become the shared Windows core that Conan already forked.
- The `sidecar/` TS code survives only as the parity fixture source, and CI still runs its tests (enshrouded.yml:65-75).

## 9. Existing tests for C4/C6, and gaps

| Test | Covers | Ref |
|---|---|---|
| `bridge-overload-timeouts` | 130 requests → exactly 2 `overloaded`; a running job times out at 300 ms with its late result dropped; an unstarted job is cancelled and **never runs** (`CallCount == 1`) | mod/tests/native/bridge_test.cpp:415-466 |
| `bridge-outbox-confirm`, `bridge-reconnect`, `bridge-backlog-latency`, `bridge-adversarial`, `bridge-outbox-restart`, `bridge-corrupt-outbox`, `bridge-ban-intent-recovery`, `bridge-timed-bans`, `bridge-new-boot`, `bridge-periodic-reconcile`, `bridge-v1-events` | confirm on later pong, resend on reconnect, outbox durability and corruption fencing, crash-mid-ban recovery, v1 acks | bridge_test.cpp:236,274,319,358,673,754,535,467,695,713,885 |
| `heartbeat`, `frame-queues`, `persistence` unit groups | ping FIFO watermark, per-kind bounds, store eviction | mod/tests/native/unit_test.cpp:68,110,164 |
| **CI `native-wine`** (hosted runner) | The real DLL under the **pinned GE-Proton11-9 image** (same digest as the default target) via a `host.exe`, against a Python TLS fake Takaro. It covers identify, ping/pong both ways, requests, confirm-on-later-pong, TCP abort plus resend, clean 1012 close plus reconnect, the 20 s dead-link check, wrong CA / missing CA / wrong host rejection, live `plugin.json` reconfigure, restart with the same identity, and no token leak. **No game runs** (game capabilities degrade) | .github/workflows/enshrouded.yml:85-126; mod/tests/wine/run.sh:1-5,74-76,118-148,150-230; mod/tests/fake_takaro_ws.py |
| Local `takaro-maint verify` (not CI) | Boots the real server plus DLL plus fake Takaro: `native-identify`, `reconnect`, `event`, `stop`, `negative-degraded-hooks`. The doc reports a pass on 2026-09-29 for **build 1024233 only** | DEVELOPMENT.md:233-281 |
| CI runtime with a real game | **None.** `runtime: false` (enshrouded.yml:163-168; DEVELOPMENT.md:416-419) | |

**Proton evidence summary:**
- WinHTTP WSS under Proton has **real CI execution**, but against a fake Takaro and with no game.
- The full-server Proton proof is documented, done locally, and only for the retired 1024233/GE-Proton10-30 target.
- The default 1267041 target has `evidence: []` (proton-1267041.json:11).
- The README's 1.0 live proof is on **Windows** (README.md:19-21).

**Gaps:**
- (a) `hooks.cpp`/`world.cpp` queues and their cancel semantics are not compiled by host tests (run.sh:17-19 omit them).
- (b) No test covers a timed-out multi-action `giveItem`, or a game-thread drain under load or time budget.
- (c) No test for crash loss windows (ring, outbox flush).
- (d) No test for a clean server stop flushing the outbox.
- (e) Proxy-required egress and real-CA system trust are untested under Wine (tests always use `TAKARO_CA_FILE`, wine/run.sh:107).
- (f) Wine's undrained PING payload causes 12152 (transport_winhttp.cpp:736-743), which is only logged.

## 10. Top risks

| # | Risk | Sev | Proposed follow-up |
|---|---|---|---|
| 1 | `giveItem` keeps granting items after Takaro already received a timeout error (up to 64 × 4 s), is not cancellable and not idempotent. Duplicate grants are possible on a Takaro or module retry | **high** | Add a cancel flag checked between create actions (world.cpp:1215), plus a host test with a gated injection. |
| 2 | Proton install needs `WINEDLLOVERRIDES` (host cooperation) and loads an arbitrary native DLL. The default target is only `candidate`, with no Proton runtime proof for 1.0 and no Windows catalog target | **high** | Record a renter-control matrix (Windows file drop vs Proton env), add a Windows target or drop the README claim, and run `verify` on `proton-1267041`. |
| 3 | Game and hook threads do synchronous disk I/O (`PluginLog` fopen/fclose per line under a global lock shared with the bridge; `enshrouded_server.json` read under `PluginState::lock_`). The 3 game queues are unbounded with count-only drains | med | Move `PluginLog` to an async bounded ring writer, and add a per-tick time budget and a queue cap with diagnostics. |

Lower: no outbox flush on a non-Takaro shutdown (low); `WINHTTP_ACCESS_TYPE_NO_PROXY` blocks proxied hosts (low).
