# Conan Exiles: issue #298 source analysis (source-only)

HEAD 3859fd8. Every finding below comes from reading the source. Nothing was run. Paths are relative to
`games/conan-exiles/` unless they start with `catalog/` or `games/`. `n/` = `games/conan-exiles/native/`.

## 1. Architecture on main

| Item | Linux | Windows | Ref |
|---|---|---|---|
| Placement | In-process native library: `libtakaro-conan-native.so`, loaded through `LD_PRELOAD`. The ELF constructor `Init()` does nothing unless the exe is `ConanSandboxServer-Linux-Shipping` | In-process `winmm.dll` proxy (DLL search-order) that forwards `timeBeginPeriod`/`timeEndPeriod`/`timeGetTime`. `DllMain` starts one thread that runs `StartConnector()` | n/platform/linux/main.cpp:99-102; n/platform/windows/dllmain.cpp:47-80; n/platform/windows/main_win.cpp:165-168 |
| Sidecar / RCON / mod | None. INSTALL tells operators to keep RCON off. `mod/TakaroConanBridge/` holds only a DevKit handoff document set and no code | same | INSTALL.md:3-5, 81-88; mod/TakaroConanBridge/README.md:1-4 |
| Runtime | C++17 `-O2`, exceptions enabled (no `-fno-exceptions`), built static on Debian buster | zig c++ C++17, MinHook | n/platform/linux/build.sh:65; n/platform/windows/build.sh:48 |
| Game-thread entry | ProcessEvent prologue patch plus trampoline. The game thread is taken to be `getpid()` | MinHook detour of ProcessEvent. The game thread is the thread that runs `DllMain` | n/platform/linux/hook.cpp:66-84; n/platform/windows/hook_win.cpp:68-89; dllmain.cpp:76-77 |
| Build gate | Pins check the GNU build-id against `pins.json`. On an unknown build the connector hooks nothing, still connects, and refuses every action | Pins check the PE TimeDateStamp and SizeOfImage | n/platform/linux/main.cpp:141-165; main_win.cpp:212-237; catalog/conan-exiles/game.json (readinessNote) |
| Transport library | libwebsockets 4.5.8 + OpenSSL 3.5.8 (static), ported from VEIN | WinHTTP WebSocket, ported from Enshrouded | n/platform/linux/transport_lws.h:1-3; n/core/takaro/transport.h:1-2 |
| TLS verification | Trust store is `caFile`, else the bundled `ca-certificates.crt` next to the .so, else the first system bundle. A missing bundle is a hard error. The store is test-loaded. `LCCSCF_USE_SSL` relies on lws defaults for SNI and hostname checks. Only `wss://` URLs are accepted | System store by default. With `caFile` it sets `SECURITY_FLAG_IGNORE_UNKNOWN_CA`, then checks the chain itself (`CertGetCertificateChain` + `CertVerifyCertificateChainPolicy(SSL)`) before identify. TLS 1.2/1.3 | main.cpp:74-80,125; transport_lws.cpp:255-275,288,315,366; transport_winhttp.cpp:125-162,447-449,507-530 |
| Wire | Protocol 0/1 negotiated per connection. v1 adds streamId/seq events and eventAck | same | n/core/takaro/bridge.h:11-14 |
| Default catalog target | `linux-25792439` / `windows-25792439`: `default: true`, `support.status: candidate`. The newest **maintained** target is `linux-25738716` / `windows-25738716` (`default: false`) | | catalog/conan-exiles/targets/linux-25792439.json:3,7,9; linux-25738716.json:3,7,9 |
| Connector version | 3.4.0 | | version.txt:1 |

## 2. Drift vs the issue (Conan section, issue lines 227-236)

| Issue hypothesis (quoted) | Status at HEAD | Ref |
|---|---|---|
| CON-1 "planning target was build 25356024; its binary was absent locally, so no ELF/preload probe ran … load an inert process-scoped library" | **Obsolete.** A full native `LD_PRELOAD` connector ships, pinned to builds 25639945, 25738716 and 25792439. Its inert mode is `TAKARO_CONAN_NATIVE_DISABLE=1` | main.cpp:3-15,120-124; INSTALL.md:7-8 |
| CON-2 "one safe player-read hook or supported extension route … Compare against the supported mod/DevKit route" | **Changed.** Reads do not use a hook. They read memory on worker threads (`process_vm_readv` / `ReadProcessMemory`), and the game thread is used only for self-checks and fallbacks. Events come from ProcessEvent subscriptions. The DevKit mod survives only as handoff documents | n/core/conan/reads.h:8-11; n/core/ue/mem.h:1-6; n/platform/windows/selfmem_win32.cpp:1-20 |
| CON-3 "save-database query cost … current optimistic inventory overlay" | **Obsolete.** `saveDb.ts` and the overlay in `bridge/src/conan/adapter.ts` no longer exist (there is no `bridge/` at HEAD). Inventory is read live. giveItem reports the measured count delta (`GetNumberOfItemsByTemplate` before and after). Read-after-grant semantics still need live proof | n/core/conan/actions_game.cpp:96-112; actions.cpp:212-233 |
| CON-4 "RCON queue saturation/karma throttling … chat helpers including … Pippi" | **Obsolete** for RCON and Pippi: RCON is not used, and chat goes through a native ChatRPC ProcessEvent call. **Still true:** "restart identity/presence". On shutdown the connector emits synthetic logouts (Linux and Windows) | chat.cpp:76-106; main.cpp:136; main_win.cpp:205 |
| Starting evidence `bridge/src/conan/adapter.ts` | **Gone.** It existed at the old revision (`git show 147acb21:games/conan-exiles/bridge/src/conan/adapter.ts`). The new authority is `native/` | DEVELOPMENT.md:8,109 |
| Common C7 framing "sidecar removal candidate" | **Done.** The sidecar has already been removed, so C7 becomes "verify the native connector", not "assess feasibility" | DEVELOPMENT.md:8 |

## 3. C1 install (renter)

| Step | Linux | Windows | Ref |
|---|---|---|---|
| Files | Unzip `TakaroConanNative/` anywhere the server user can read (`.so` + `ca-certificates.crt`) | Copy `winmm.dll` into `ConanSandbox\Binaries\Win64\` | INSTALL.md:23-48 |
| Loader / launch | **`LD_PRELOAD=…/libtakaro-conan-native.so`** in the server process environment (Docker: env var + mount) | None: the exe imports WINMM.dll, so the proxy loads on its own | INSTALL.md:28-36; dllmain.cpp:1-3 |
| Writable state | `ConanSandbox/Saved/Config/Takaro/takaro.json` (created on first start), `bans.json`, `ConanSandbox/Saved/Takaro/state/` (outbox, health), `Saved/Logs/TakaroConanNative.log` | same paths | INSTALL.md:52-56,99-100; n/core/takaro/config.cpp:86; main.cpp:105-106 |
| Restart | One restart to load the library. Changes to the token, URL or name hot-reload from takaro.json. Env changes need a restart | same | INSTALL.md:52-56,77-79 |
| Network | Outbound WSS to `wss://connect.takaro.io/` | same | INSTALL.md:60,73 |
| Game build | Must match a pinned build, otherwise every action is refused | same | INSTALL.md:7-8 |
| Classification | **Arbitrary native library injected through `LD_PRELOAD`**: the renter needs control of the server's environment or launch command | **Arbitrary native DLL dropped into the game's binaries directory** (DLL hijack): the renter needs write access to `Binaries\Win64` | |
| Host control needed | Env-var / launch-script control, or host cooperation. FTP alone is not enough | FTP or file-manager write access to the binaries folder. Hosts that verify or reset game files, or that block DLLs, would break it | |

Verdict: "documented but untested" on rental hosts. Both routes are native code injection, not a managed plugin.

## 4. C3 thread trace

| Path | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| **getPlayers** | Action worker thread reads live UObject memory through `SelfMem` (`process_vm_readv` / `ReadProcessMemory`) with torn-read checks (`Snapshot`). The game thread is used only for the one-time `RunPlayerChecks` job and for `SlowPing` when the ping self-check failed | Worker (`PlayerJson`, reads.cpp:628-631) | Bridge thread `Complete`→`SendResponse`, then the transport thread | none | Worker `NativeLog` (queued); the log thread writes the file |
| refs | reads.cpp:520-538, 613-631, 268-301; reads.h:8-11 | | bridge.cpp:124-150, 398-438 | | reads.cpp:585-594; common.cpp:43-80; main.cpp:87-97; main_win.cpp:77-82 |
| **giveItem** | Worker parses arguments and resolves the item code (`rx::ItemNameMap`). `TargetPlayer`→`OnlinePlayers` is one game-thread job. Each `GiveChunk` is one game-thread job (pawn lookup, `GetBackpackInventory`, count, `AddItemTemplate`, count). Up to 200 chunks | Worker (`Ok()` / `Fail`) | Bridge thread | none | Worker actions.cpp:229 |
| refs | actions.cpp:191-233; actions_game.cpp:33-45, 63-120 | | | | |
| **player-connected** | Game thread: `K2_PostLogin` After-hook→`OnPostLogin`→`ReadIdentity`→`QueueConnect`. `QueueConnect` calls `isBanned`, which takes the `BanList` mutex, and pushes a `PendingLogin` under `Sh().mu`. `OnPossessed` sets `emitAt`. Deferred identities are re-read on the game thread through `Run(…,1000)` | Events worker (`Emit`→`Sanitize`/`ConnectedPayload`), then `Adapter::Emit` queue | Bridge thread `PollEvents` (every 100 ms)→`Store::Admit`→`DeliverEvents`→transport | Bridge thread `FlushState`→`SaveOutbox` (tmp, fsync, rename) | Events worker events.cpp:656 |
| refs | events.cpp:418-457, 533-545, 768-774, 802 | events.cpp:641-656; adapter.cpp:233-242 | bridge.cpp:101-105, 470-520 | bridge.cpp:550-558; fileio_posix.cpp:56-64 | |

Workers that hold live game pointers:
- `ReadService` caches `gameStates_` (raw addresses, rescanned at most every 10 s) and `PlayerRecord.ps/.pawn` (reads.cpp:21,137-165). These are read off-thread through the fault-tolerant `Mem`, so a stale pointer yields false, not a crash.
- **Exception:** `SlowLocation(p->pawn)` and `SlowPing(p.ps)` pass a pointer from the worker's snapshot to `ProcessEvent` on the game thread **without an `Alive()` re-check** (reads.cpp:217-227). `RunPlayerChecks` and `RunStatsCheck` do re-check (reads.cpp:276,283,338). These paths are fallbacks that run only after a self-check has failed.
- The events `PendingLogin.pc` and `Deferred.pc` are raw controller addresses. They are re-validated with `IsPlayerController` on the game thread before use (events.cpp:768-772).

**Game thread can wait on disk I/O:** `QueueConnect`→`isBanned`→`BanList::Find` takes `mu_` (bans.cpp:98), which a worker holds across `AtomicWriteFile` with fsync during `Upsert`/`Remove` (bans.cpp:44-60). A ban written while a player logs in blocks the game thread for the length of the fsync.

## 5. C4 queues

| Stage | Capacity / admission | Drain | Timeout / lifetime | Ref |
|---|---|---|---|---|
| Transport → bridge notices | Frames only: 4096 notices or 16 MiB. Over the limit `OnNotice` returns false, and the transport drops the connection | Bridge loop takes up to 512 per wake (wakes at least every 50 ms) | n/a | bridge.h:51-53; bridge.cpp:57-71, 80-96 |
| Bridge → action workers (`jobQueue_`) | Admission is checked on `inflight_.size() >= 128`, which includes timed-out jobs that are still running. Over the limit: an immediate `native action queue overloaded` error | `actionWorkers` threads, default 2 (1-8) | `actionTimeoutMs` default 15000 (1000-600000). Deadline check on the bridge thread: a job still **queued** is set to CAS→Cancelled, answered "did not start", and the worker skips it. A job already **running** is answered "timed out", stays in `inflight_` (still holding an overload slot) and runs to completion; its late result is logged and discarded. The Job is a `shared_ptr` that owns copies of action and args, so the bridge timeout leaves no stack lifetime problem | bridge.h:54,65-76; bridge.cpp:335-396, 416-467, 124-150; config.cpp:169-171 |
| Worker → game thread (`GameThread::Run`) | `std::deque` with **no capacity limit**. Its depth is bounded only by the number of threads blocked in `Run`: action workers, the events worker, HookDispatch, the perf sampler, the chat helper and the catalogue warm-up | `Drain()` runs from the ProcessEvent detour on the game thread (not re-entrant). Per drain: `kMaxJobsPerDrain = 4` jobs or `kBudgetNs = 500 µs`, **checked after each job**, so a single job is never preempted | Per-call `timeoutMs`: reads/chat/hook 2000, mutations/perf 3000, deferred login 1000. **Timed out while queued:** marked Cancelled under the lock, never runs, `Run` returns false (gamethread.cpp:52-55,76). **Timed out while running:** `Run` waits **without a limit** for Done and then returns **true** (gamethread.cpp:56-58). The `Queued→Running` change happens under the same lock (gamethread.cpp:75-84), so no job can start after its caller gave up. `[&]` captures of caller locals are therefore safe: CallOnGame `parms[512]` reads.cpp:176-187, GiveChunk `st` actions_game.cpp:66, hook scan `next/end/out` hook_dispatch.cpp:131-146 | n/core/gamethread.h:16-21; gamethread.cpp:28,36-37,41-94; platform/linux/hook.cpp:48-53; platform/windows/hook_win.cpp:50-53; reads.cpp:20; actions_game.cpp:22; chat.cpp:14; hook_dispatch.cpp:26; perf_sampler.cpp:17; events.cpp:768-774 |
| Game-thread job exception | `job->fn()` is not wrapped in try (gamethread.cpp:85). A C++ throw unwinds through `Drain` into the ProcessEvent detour, which means the game crashes or terminates. The job stays Running, so its caller would wait for ever | | | gamethread.cpp:85 |
| Events capture (game thread → events worker) | `Push`: 4096, drops the newest when full. Pending deaths: 32. Pending logins: no capacity limit (released after possession + 500 ms or 120 s) | Worker tick 50 ms, takes the whole batch | | events.cpp:43-45,160-167,416,723-740 |
| Adapter event queue | 4096. When full it drops the oldest `log` event, otherwise the oldest event | Bridge `PollEvents` every 100 ms takes all | | adapter.h:67; adapter.cpp:233-250 |
| Durable outbox | `kMaxPendingEvents = 5000`, `kMaxPendingBytes = 32 MiB`. Drops the oldest `log` event first and counts `losses` | `DeliverEvents` queues ≤512 per loop after `lastQueuedId_` | Disk rewrite every 100 ms to 5 s, depending on size | outbox.h:44-57; bridge.cpp:494-520, 550-558 |
| Outbound frame queues | Per-kind FIFO, strict priority. Control 64 / 1 MiB; CriticalResponse 256 / 1 MiB; Response 256 / 32 MiB; Event 1024 / 8 MiB. One frame ≤ 8 MiB. `Queue()` never blocks. A full Response queue makes the bridge close the connection | Transport service thread | Frames for a stale epoch are refused | transport.cpp:16-40; transport.h:62-64; bridge.cpp:398-414 |
| Shutdown, Linux | `Fini` (ELF destructor): `GameThread::Stop()` cancels queued jobs and fails future `Run` calls; then events, HookDispatch and perf sampler stop; then `bridge->Stop()` joins the bridge and workers and forces a final `SaveOutbox`. A worker in the middle of a mutation or a `ListResult` wait (≤8 s) or a verify loop (≤6 s) delays exit. A running game-thread job cannot block exit, because `Fini` runs on the exiting thread | | | main.cpp:207-225; bridge.cpp:41-55,117-121; reads.h:44; actions.h:74 |
| Shutdown, Windows | **Nothing is stopped**: no `GameThread::Stop` and no `Bridge::Stop`. The outbox is as fresh as the last `FlushState` (≤100 ms to 5 s old). `EmitShutdownLogouts(2000)` waits 2 s before a Takaro-initiated exit only | | | main_win.cpp:262-264; main.cpp:136 |

Mutation outcome on timeout: the bridge answers "timed out" at 15 s while the worker continues. For example, giveItem can run up to 200 chunks, and each chunk can wait up to 3 s for the game thread before it starts. The item can therefore be delivered after Takaro has recorded a failure, and a Takaro retry would deliver it twice. No idempotency key exists beyond the duplicate-`requestId` check over the last 1024 ids (bridge.cpp:326-333,358-362).

## 6. C5 state inventory

| Category | Authority | Connector store | Persistence | Invalidation |
|---|---|---|---|---|
| Players online | Live game memory (PlayerArray) | None. A snapshot is taken on every request. `gameStates_` index is rescanned ≤ every 10 s | none | Rescan on "no live GameState" (reads.cpp:520-530) |
| Player names (offline fallback) | Last snapshot | `known_` map in memory (reads.cpp:621, 503-507) | none (process lifetime) | never pruned |
| Last location | Live pawn memory (`ComponentToWorld`) or `K2_GetActorLocation` fallback | none | none | n/a |
| Inventory | Live `ItemInventory` memory | none (the old overlay is gone) | none | n/a |
| Item / entity / location catalogue | Game DataTables | `itemsJson_` etc., built once by a warm-up thread | none | process lifetime; not rebuilt on map change (reads.cpp:56,455-500) |
| Identity (server) | `identityToken` in takaro.json | file | yes | operator; hot-reloaded (config_watch.cpp:120-130,172) |
| Identity (player) | `PlayerState.UniqueID` → Steam64 | GT maps `byController` / `bySteam` (memory) | none | logout |
| Bans / timed bans | **Connector-owned** `Config/Takaro/bans.json` | file + memory, atomic fsync writes | yes | Expiry and kicks enforced by a sweep thread (actions.cpp:148-154, 414-460) and a login check (events.cpp:420-424). Not scoped to a world |
| Event cursor | Per-process random `streamId`, seq 1.. | outbox file | yes; replay keeps the original stream/seq | Unparseable file is fenced and never overwritten (outbox.h:10-16; outbox.cpp:25,94-95,133) |
| Pending events / outbox | Connector | `state/event-outbox.json` (5000 / 32 MiB) | yes | Confirmed by v0 pong-watermark or v1 eventAck |
| Health | Connector | `health.json`, `events-health.json` | overwritten | n/a |

## 7. C6 delivery

| Item | Behaviour | Ref |
|---|---|---|
| Outbox bound | 5000 events / 32 MiB on disk; earlier memory stages hold 4096 each | outbox.h:44-45; adapter.h:67; events.cpp:162 |
| Ack, protocol 0 | Transport pong watermark: an event counts as confirmed only after the pong of a ping sent after the event was written (Linux: WS control ping/pong; Windows: app `{"type":"ping"}`). This proves receipt, not processing | transport.h:3-9, 81-125; bridge.cpp:209-213 |
| Ack, protocol 1 | Only `eventAck {streamId, upTo}` removes events. `eventError` is counted and logged; the event stays until it is acked | bridge.cpp:209-213, 293-301, 522-531 |
| Reconnect replay | On Open, `lastQueuedId_ = 0`, so every unconfirmed event is re-sent after identify. Backoff resets only after a successful identify | bridge.cpp:180-186; transport_lws.h:6-11 |
| Lost on connection close | Unanswered requests that had not started are cancelled. Running ones finish and their responses are dropped (stale epoch) | bridge.cpp:214-229, 398-402 |
| Lost on game crash | Events captured on the game thread but not yet admitted (events `queue`, adapter queue: up to 100 ms + 50 ms of events) are lost. Admitted events that have not been flushed are lost too (window 100 ms to 5 s). On Windows nothing is flushed on a clean exit either. No `player-disconnected` is sent for players online at a crash; only a Takaro-initiated shutdown emits them | bridge.cpp:550-558; main_win.cpp:264; main.cpp:136 |
| Mutations | Never replayed | |

## 8. C8 shared / copied code

| Conan file | Sibling | State |
|---|---|---|
| n/core/gamethread.cpp (107 lines) | games/dragonwilds/mod/src/gamethread.cpp (323), games/vein/mod/src/gamethread.cpp (321), games/dune/mod/src/gamethread.cpp | **Separate rewrite**, not a copy: `diff` shows 372-411 differing lines. The header names VEIN's game-thread policy as its source (gamethread.h:4). The comparison is in src/native-runner.md |
| n/core/takaro/bridge.{h,cpp} | games/enshrouded/mod/src/native/bridge.{h,cpp} | Header says "Ported from Enshrouded, behaviour checked against VEIN" (bridge.h:1-3). **Heavily diverged**: 650 vs 1191 lines, 1075 differing diff lines |
| n/core/takaro/transport.{h,cpp} (FrameQueues, Heartbeat) | games/enshrouded/mod/src/native/transport.{h,cpp} | Close: 20-25 differing lines per file |
| n/core/takaro/json_util.{h,cpp} | games/enshrouded/mod/src/native/json_util.{h,cpp} | Near-identical: 4-6 differing lines |
| n/core/takaro/outbox.{h,cpp} | games/enshrouded/mod/src/native/persistence.{h,cpp} | Derived; online, known-player and ban areas removed (outbox.h:1-3). 305 differing lines |
| n/core/takaro/{config,fileio}.* | Enshrouded native config/fileio | Diverged (390 / 315 differing lines) |
| n/platform/windows/transport_winhttp.cpp | games/enshrouded/mod/src/native/transport_winhttp.cpp | Close fork: 792 vs 769 lines, 125 differing lines |
| n/platform/linux/transport_lws.cpp | games/vein/mod/src/native_transport.cpp | "Ported from VEIN" behind ITransport (transport_lws.h:2-3). Restructured |
| n/core/common.cpp JSON | games/vein/mod/src/common.cpp | "copied from" (common.cpp:119) |
| Game-specific | core/conan/* (reads, actions, events, chat, bans, pins), core/ue/*, platform hooks | |

## 9. Existing tests for C4/C6

| Test | Covers |
|---|---|
| n/tests/unit_test.cpp `TestOutbox` (318) | Outbox persistence, fenced corrupt file, 5000 limit drops the `log` event first |
| n/tests/unit_test.cpp `TestQueuesAndHeartbeat` (375) | Per-kind frame limits and priority; pong watermark |
| n/tests/unit_test.cpp `TestProtocolV1` (766) | Stream/seq, `Acknowledge`, replay keeps the original stream after restart |
| n/tests/wire_test.py `basics` / `outage` / `restart` / `protocol_v1` (117, 216, 268, 345) | Real TLS harness against `fake_takaro.py`: confirmation by a later pong, a 20 s outage with in-order replay, SIGKILL replay, cumulative eventAck. Uses `TAKARO_ACTION_TIMEOUT_MS=5000` (46) |
| n/tests/so_test.py | The real .so `LD_PRELOAD`ed into a stand-in exe: unknown-build refusal, outbox replay across an outage, config hot-reload |
| n/tests/abortive_close_test.cpp | Windows dead-link detection |
| n/tests/l2b_test.cpp `TestReflectedGame` (803) | Drives `GameThread::Drain` from a fake game thread (the happy path only) |

**Gaps:**
- No test of `GameThread::Run` timeout: queued-then-cancelled, running-past-timeout, an exception in a job, or behaviour after `Stop`.
- No test of the bridge `CheckDeadlines`: the "did not start" and "timed out" paths, the late-result drop, or the 128-job overload.
- No test of a slow outbound path (response queue `Full` closing the connection) or of notice overflow.
- No sanitizer build.
- No test of the Windows exit path or of the game thread contending with the ban fsync.

## 10. Top risks

| # | Risk | Severity | Follow-up |
|---|---|---|---|
| 1 | A mutation can complete after Takaro was told it timed out. giveItem can run up to 200 × 3 s chunks against a 15 s bridge timeout, and Takaro has no way to tell, so a retry can duplicate the grant (bridge.cpp:440-467; actions.cpp:29,212) | high | Give giveItem a total deadline below `actionTimeoutMs` and report a distinct "outcome unknown" code for a running-job timeout; add a CheckDeadlines unit test |
| 2 | The game-thread runner has no protection against a throwing or hung job. `fn()` is not wrapped in try (gamethread.cpp:85), and the wait after timeout has no limit (gamethread.cpp:57), so one bad job crashes the server or wedges a worker, which holds one of the 2 action slots for ever | med | Catch around `job->fn()` in `Drain` and mark the job Done with a failure; add a deterministic Run/Drain lifetime test under ASan/TSan |
| 3 | Game-thread stalls on connector I/O and unchecked pointers. `isBanned` on the login hook waits on the `BanList` mutex held across fsync (bans.cpp:44-60). `SlowLocation`/`SlowPing` call `ProcessEvent` on a snapshot pointer without `Alive()` (reads.cpp:217-227). Windows never flushes or stops on exit (main_win.cpp:264) | med | Move the ban file write outside `mu_` (copy-on-write) or keep a lock-free snapshot; add `Alive()` to the Slow* fallbacks; force `FlushState` on Windows exit (`atexit`/`DLL_PROCESS_DETACH`) |
