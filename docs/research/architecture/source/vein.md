# VEIN — issue #298 source-level analysis (source-only)

HEAD 3859fd8. All paths relative to repo root; `V=games/vein`, `S=games/vein/mod/src`. Nothing was run; every claim is from source at HEAD.

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | In-process native library `libtakaro-vein.so` inside `VeinServer-Linux-Test`; no sidecar | `V/INSTALL.md:3`, `V/DEVELOPMENT.md:3` |
| Runtime | C++17, UE vtable detours on `U*Engine::Tick` (game thread pump) and ProcessEvent/PostLogin hooks | `S/gamethread.cpp:116-129`, `V/mod/docs/gamethread-policy.md:12-16` |
| Loader (2 routes) | (a) `LD_PRELOAD` on the game binary only; (b) "panel loader" `libsteam.so` dropped in `Vein/Binaries/Linux`, dlopened by `libsteam_api.so` via DT_RPATH `${ORIGIN}`, which dlopens the connector | `V/INSTALL.md:11-17`, `V/README.md:18-37`, `V/mod/shim/panel_loader.c:1-6`, `V/mod/build.sh:83-88`, `V/scripts/build-release.sh:22` |
| Target guard | Constructor attaches only if exe path contains `VeinServer`; process lock against double load (LD_PRELOAD + panel loader) | `S/main.cpp:150-171` |
| Transport | libwebsockets 4.5.8 + OpenSSL 3.5.8 on own service thread; nlohmann json 3.12.0 | `S/native_transport.cpp:330-354,485`, `catalog/vein/targets/linux-25708939.json` (`build.deps`) |
| TLS verification | `wss://` mandatory; CA bundle from system candidates or `TAKARO_CA_FILE`, fails closed if none loads; SNI + hostname verification left to LWS defaults (`LCCSCF_USE_SSL`, no `ALLOW_INSECURE`/self-signed flags) | `S/native_transport.cpp:331-352,363-369,427`, `S/ca_bundle.cpp:9-13` |
| Default catalog target | `linux-25708939` (`default: true`, `support.status: maintained`, since 2026-10-07) — only target | `catalog/vein/targets/linux-25708939.json` |
| Connector version | `0.7.0` | `V/version.txt` |
| Former sidecar | `games/vein/sidecar/` still in tree but **legacy**: "not built, packaged or run by the native release"; build script deletes any stale sidecar archive; kept as behaviour/migration reference (drain helpers `scripts/drain-legacy.py`) | `V/DEVELOPMENT.md:3,52`, `V/scripts/build-release.sh:39-40`, `V/INSTALL.md:22-53` |

## 2. Drift vs the issue (issue text: scratchpad/issue298.md:216-225, written at 147acb2, VEIN 0.2.3)

| Issue hypothesis (quoted) | Status at HEAD | Ref |
|---|---|---|
| VEI-1 "Verify preload and target guards" | Still applicable; guards now exist in code (exe-name check + instance lock) and a second, non-preload loader was added | `S/main.cpp:150-171`, `V/mod/shim/panel_loader.c` |
| VEI-2 "lazy snapshot's freshness and the per-tick budget, including one slow job" | Still true: lazy snapshot TTL 500 ms and 500 µs tick budget unchanged; new finding: a timed-out refresh silently serves the older cache | `S/actions.cpp:522-531,559-577`, `S/gamethread.cpp:42-52,74-76` |
| VEI-2 "Reuse the shared native runner lifetime probe" | Changed: the stack-reference defect existed at 147acb2 (`RunJson` and 8 callers used `[&]`) and is fixed at HEAD; all current `GameThread::Run` call sites capture by value / `shared_ptr`; a deterministic ASAN test exists | old `git show 147acb2:games/vein/mod/src/gamethread.cpp:281-292`; HEAD `S/gamethread.cpp:287-300`; `V/mod/tests/game_thread_timeout_test.cpp` |
| VEI-3 "heartbeat-based transport confirmation … do not equate pong confirmation with durable processing" | Partly obsolete: pong-after-write confirmation still used for protocol v0; protocol v1 uses Takaro `eventAck upTo` with a 30 s ack timeout that forces reconnect + resend | `S/native_bridge.cpp:1010-1019,493-500,1201-1205` |
| VEI-3 "game/sidecar restart", "online/known identities and timed-ban expiry retry" | Sidecar gone; same state now lives in in-process durable store (`event-outbox.json` incl. derived online/known, ban-intent journal) | `S/native_persistence.h:17-37,71-76`, `V/INSTALL.md:57-59` |
| VEI-4 "watchdogs and built-in HTTP/log fallbacks remain necessary after a direct connection" | Still open: built-in VEIN HTTP `:8080` player fallback and log-tail join/chat fallbacks remain in the native build | `S/native_behavior.cpp:379-385`, `S/native_transport.h:75-79`, `S/events.cpp:1843,1874,1928` |
| VEI-4 "Proceed to a minimal native transport/action/event proof only after loader/install requirements are explicit" | Obsolete: full native connector shipped (#299, vein 0.4.0+) | `git log d6141aa`, `catalog/vein/targets/linux-25708939.json` evidence |
| Starting evidence `mod/docs/gamethread-policy.md` | Still present and updated with worker-ownership rule | `V/mod/docs/gamethread-policy.md:35-46` |

## 3. C1 install (ordinary renter)

| Step | Panel route (default in README) | Admin route |
|---|---|---|
| Files | Upload `libtakaro-vein.so` + `libsteam.so` into `Vein/Binaries/Linux` (`V/README.md:18-21`) | `libtakaro-vein.so` anywhere readable (`V/INSTALL.md:9`) |
| Loader | Game's `libsteam_api.so` dlopens optional `libsteam.so` (`V/README.md:37`, `V/mod/shim/panel_loader.c:2-5`) | `LD_PRELOAD` on game binary only, never SteamCMD (`V/INSTALL.md:11-17`) |
| Config / writable state | `takaro.cfg` auto-created next to the .so; paste registration token, live reload, no restart (`V/README.md:22-23`, `V/INSTALL.md:10`); state + `plugin.log` under `Vein/Binaries/Linux/takaro` (`S/native_persistence.cpp:200`, `V/README.md:43`) | env vars or `TAKARO_STATE_DIR` / `TAKARO_PLUGIN_DATA_DIR` (`V/INSTALL.md:10`) |
| Restart | One stop/start for install; token change needs none | same |
| Runtime deps | System CA bundle (`ca-certificates`) or `TAKARO_CA_FILE` (`V/README.md:43`, `S/ca_bundle.cpp:9-13`); OpenSSL/LWS statically bundled | same |
| Network | Outbound `wss://` only (`S/native_transport.cpp:363-369`); optional loopback `:8080` game API read | same |
| Update / uninstall | Overwrite two files; delete `libsteam.so` to remove (`V/README.md:37,47`) | remove `LD_PRELOAD` |

Classification: **arbitrary native library** (not a managed plugin DLL). Host control needed: panel route needs only SFTP/file-manager write to the game binary directory (no launch-line/env control) — but it depends on an undocumented game/Steam probe of `libsteam.so` (`V/README.md:112`); LD_PRELOAD route needs launch-command control. Doc drift: `V/README.md:3` and `V/connector.json` asset hint still say LD_PRELOAD is required, contradicting `V/README.md:18-37`.

## 4. C3 thread trace

Threads: game thread (Tick detour), LWS service thread (`S/native_transport.cpp:485`), bridge worker and single action worker (`S/native_bridge.cpp:1502-1503`), init/housekeeping thread every 2 s (`S/main.cpp:130-139`).

| Op | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| getPlayers | Game thread only on cache miss (>500 ms): `BuildPlayerSnapshotOnGameThread` copies plain rows (`S/actions.cpp:537-555,571`); fallback HTTP read of VEIN `:8080` performed by LWS thread for action worker (`S/native_behavior.cpp:383`, `S/native_transport.h:75-79`) | Action worker (`S/actions.cpp:2026-2033`, `S/native_behavior.cpp:379-388`) | Completion → bridge → `NativeTransport::Queue` → LWS thread (`S/native_bridge.cpp:1250-1330`, `S/native_transport.cpp:530-565`) | None | `PluginLog` is enqueue-only, bounded 1024 lines/1 MiB; file write by `FlushPluginLogs` on housekeeping/init thread (`S/common.cpp:67-84,123-139`) |
| giveItem | Three sequential game-thread jobs: count-before + up to 2 attempts (`S/actions.cpp:2708,2738-2745`), each via `OnGameThread` (5 s) (`S/actions.cpp:282-302`) | Action worker | as above | None for give; ban/unban flush `bans.json` off the game thread after the job (`S/actions.cpp:2998`, `S/state.cpp:259-271`) | as above |
| player-connected | Detected on game thread (housekeeping job `ResolvePendingJoins`, `S/events.cpp:787` inside `GameThread::Run` at `S/events.cpp:2672`) | **Deferred**: closure stored in ring; serialized on bridge thread in `EventsJson` (`S/events.cpp:469`, `S/state.cpp:51-57,78`) | Bridge `FlushEvents` → LWS (`S/native_bridge.cpp:676-690`) | Bridge worker fsyncs `event-outbox.json` before send (`S/native_persistence.cpp:39-62,220`) | as above; `ConsoleLine` does a synchronous `write(STDOUT)` from the caller's thread (`S/common.cpp:88-103`) |
| chat-message | Captured inside ProcessEvent detour on game thread (`S/events.cpp:1074`) | Deferred closure, bridge thread (`S/events.cpp:949-958`) | same | same | same |

Live-object retention: deferred event closures capture copied `Ident`/strings, not UObject pointers (`S/events.cpp:469,481,949`). `KillAllowed` keys a map on a raw actor pointer (`S/events.cpp:915-923`) — identity only, not dereferenced. No worker holds game pointers for later dereference found in the traced paths.

## 5. C4 queues

| Queue | Capacity | Admission | Drain | Timeout / lifetime | Shutdown |
|---|---|---|---|---|---|
| Game-thread jobs `g_jobs` | 256 (`S/gamethread.cpp:264`, `>=` since #299; was `>` = 257 at 147acb2) | Reject (return false, counted nowhere) when full or pump not installed (`S/gamethread.cpp:258,264`) | Per tick: ≤ `kJobsPerTick`=16 (`S/gamethread.h:14`), budget `TAKARO_TICK_BUDGET_US` default 500 µs clamp 50–33000 (`S/gamethread.cpp:42-52`); first job always runs, budget checked only between jobs (`S/gamethread.cpp:75-76`) — a single slow job is unbounded | Caller waits on cv until deadline (`S/gamethread.cpp:267-283`). On timeout: not-started → `cancelled` and skipped (`:272,83`); **started → keeps running after caller returned false** (`:272-274`, header `S/gamethread.h:19-20`). Caller-stack safety: `RunJson` owns fn+result via `shared_ptr` (`:287-300`); every call site captures by value/`shared_ptr` (`S/actions.cpp:285-298,571,1567,1701,1997`; `S/admin.cpp:415,453`; `S/http.cpp:255,281,304`; `S/events.cpp:2484,2504,2672`; `S/main.cpp:106`). No `[&]` capture into `Run` at HEAD. Policy codifies it (`V/mod/docs/gamethread-policy.md:43-46`) | No drain; pending jobs are owned and destroyed with the deque; `NativeBridge::Stop` joins the action worker, which can be blocked in `Run` for up to its timeout (5/8/20 s) if the tick has stopped (`S/main.cpp:34-40`, `S/native_bridge.cpp:1519-1520`) |
| Takaro action requests `actions` | 128 entries (pending + running + reserved), 128 pending ids, 4 MiB (`S/native_bridge.cpp:544-547`) | Over → immediate `"native action queue overloaded"` error (`:547`) | Single action worker, strictly serial (`actionApplied` gate) (`S/native_bridge.cpp:1256-1263`) | Game-thread timeout → handler returns 503; next action may start while the timed-out game job is still running/queued | `stopping` → loop break (`:1258-1259`) |
| Notices (transport→bridge) | 512 / 8 MiB normal, 128 critical (`S/native_bridge.cpp:1492-1495`) | Full → overload counter / socket close (`:421`) | Batch 512 (`:55`), 100 ms wait (`:938`) | — | — |
| Transport outbound | Control 128/1 MiB, Critical 128/1 MiB, Response 128/16 MiB (shared 16 MiB with critical, single response ≤8 MiB), Event 5000/32 MiB (`S/native_transport.cpp:544-559`) | `Full`/`TooLarge`/`StaleEpoch` returned; caller keeps message (`S/native_transport.h:67-70`) | LWS writeable callback | Epoch check stops late results crossing a reconnect (`S/native_transport.cpp:535`) | — |
| Event ring (game→bridge) | `kMaxEvents`=5000, drop-oldest (`S/state.h:36`, `S/state.cpp:56`) | Always admits | Bridge polls 256 per pass every 1 s (`S/native_bridge.cpp:563,1166`) | Truncation recorded as durable loss (`:582-594`) | in memory only |
| Durable outbox | 5000 events / 32 MiB incl. raw staging (`S/native_persistence.cpp:186,272-297`, `S/native_bridge.cpp:1415`) | Evicts oldest pending as recorded loss (`S/native_persistence.cpp:282,297`) | `FlushEvents` until transport `Full` (`S/native_bridge.cpp:676-690`) | — | persisted |
| Log queue | 1024 lines / 1 MiB drop-oldest (`S/common.cpp:77-81`) | — | 2 s housekeeping | — | `FlushPluginLogs` in `ShutdownPlugin` |

Key C4 answer: **a timed-out job can still run** (if already started), and **started jobs do outlive the caller** — but at HEAD they carry only owned data, so the remaining hazard is semantic (a mutation such as give/teleport/kick reported 503 to Takaro while the effect still lands; `S/actions.cpp:297-300`), not memory safety.

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation |
|---|---|---|---|---|
| Players online | Game (`ReadPlayers` on game thread) | `g_snapRows` memory cache TTL 500 ms (`S/actions.cpp:522-577`); derived online list in outbox | `derivedOnlineJson` in `event-outbox.json` (`S/native_persistence.h:34`) | TTL; on game-thread timeout older cache served (`S/actions.cpp:573-576`) |
| Last location | Game (snapshot row / teleport lookup) | Snapshot only | none | TTL |
| Inventory | Game (game-thread job per request) | none | none | per request (`S/actions.cpp:2058`) |
| Item catalog | Game UClass reflection | built once per boot (`S/actions.cpp:1990-2002`, policy table `:58`) | none | boot |
| Entity catalog | Game | cached 5 min (`V/mod/docs/gamethread-policy.md:59`) | none | 5 min |
| Identities (known players) | Connector | `SaveKnownPlayers`, derived known JSON (`S/native_persistence.h:34,70`) | file + legacy mirror | none on world change |
| Bans / timed bans | Split: game `AVeinGameStateBase::BanID` + plugin `bans.json` + timed metadata, `ban-intent.json` journal (`S/actions.cpp:1308-1330`, `S/state.cpp:136-140,259`, `S/native_persistence.h:71-76`) | file | fsync+rename (`S/native_persistence.cpp:39-62`) | expiry via bridge (`expiryInFlight`, `S/native_bridge.cpp:93`) |
| Event cursor | Connector (`scan`, `confirmed` SourceCursor with bootId) | outbox file | yes | `SwitchBoot` on new bootId (`S/native_bridge.cpp:568-576`) |
| Pending events / outbox | Connector | `event-outbox.json` (`S/native_persistence.h:28-37`) | fsync + dir fsync; delivery fenced while dir fsync pending (`S/native_persistence.h:12-15`) | bounded eviction = recorded loss |

## 7. C6 delivery

| Item | Behaviour | Ref |
|---|---|---|
| Outbox bound | 5000 events / 32 MiB, drop-oldest with durable loss counter | `S/native_persistence.cpp:282-297` |
| Ack v0 | Ping sent after event write; matching pong ⇒ `ConfirmThrough` (transport-level only, not Takaro processing) | `S/native_transport.cpp:185-197`, `S/native_bridge.cpp:1015-1019,953-965` |
| Ack v1 | `eventAck{streamId, upTo}` clamped to highest sent; no ack for 30 s ⇒ close 1013 and resend | `S/native_bridge.cpp:242,493-500,1201-1205` |
| Heartbeat | Ping every 5 s; >=2 outstanding or 20 s idle ⇒ heartbeat timeout close | `S/native_transport.cpp:214-217` |
| Reconnect | Backoff 2 s → 60 s (`TAKARO_RECONNECT_MAX_MS`); all unconfirmed pending re-sent per new epoch (at-least-once) | `S/native_transport.h:43-44`, `S/native_bridge.cpp:1477,679-681` |
| Game crash loss | In-memory ring events not yet admitted (≤1 s poll window, up to 5000) are lost; durable outbox survives and replays; deferred event closures are in-memory only; in-flight game-thread mutation outcome unknown (ban path has intent journal, others none) | `S/state.cpp:51-57`, `S/native_bridge.cpp:1166`, `V/INSTALL.md:59` |

## 8. C8 shared/copied code

| File | Sibling | Divergence (changed lines, `diff`) |
|---|---|---|
| `S/gamethread.cpp` | `games/dragonwilds/mod/src/gamethread.cpp` | 26: only `Resolve::`↔`Sym::` and Tick symbol names/comments; logic identical incl. `RunJson` fix and 256 cap |
| `S/gamethread.cpp` | `games/dune/mod/src/gamethread.cpp` | 77 (ProcessEvent-driven pump; see parent's `src/native-runner.md`) |
| `S/gamethread.h` | dragonwilds / dune | 2 / 22 |
| `V/mod/tests/game_thread_timeout_test.cpp` | `games/dragonwilds/mod/tests/game_thread_timeout_test.cpp` | differs (diff -q) |
| `S/native_persistence.{h,cpp}` | dragonwilds | 0 / 4 (default state dir only, `:200`) |
| `S/state.cpp` | dragonwilds | 4 (log strings) |
| `S/perf.cpp` | dragonwilds, dune | 0 / 0 |
| `S/native_transport.cpp` | dragonwilds | 183 (VEIN adds game-HTTP fetch, CA bundle) |
| `S/native_bridge.cpp` | dragonwilds | 540 |
| `S/native_behavior.cpp` | dragonwilds | 1932 (game-specific mapping) |
| `S/common.cpp`, `S/hooks.cpp`, `S/http.cpp`, `S/reflect.cpp`, `S/config_file.cpp`, `S/native_log.cpp` | dragonwilds | 141 / 17 / 80 / 172 / 256 / 417 |
| `S/ca_bundle.cpp`, `S/instance_guard.cpp`, `V/mod/shim/panel_loader.c` | — | VEIN-only |

No shared library; every file is a per-game copy. Runner, persistence, state ring and perf are effectively identical with dragonwilds and good extraction candidates.

## 9. Existing tests for C4/C6

| Test | Covers |
|---|---|
| `V/mod/tests/game_thread_timeout_test.cpp:28-79` (ASAN, `detect_stack_use_after_return`, `V/mod/tests/run-timeout.sh:9-13`) | not-started cancel; started job outlives caller with owned payload; `RunJson` with destroyed caller output; 256 boundary |
| `V/mod/tests/native_bridge_adversarial.py` `overload` (:495), `stale_epoch` (:535), `completion_limit` (:555) | action queue overload, late result across reconnect, completion bounds |
| `V/mod/tests/native_full_bridge.py` `replay` (:164), `backlog_request_latency` (:188), `directory_sync` (:214), `crash_write` (:264), `stale_effect` (:289), `protocol_v1` (:80), `engine_crash_setup` (:312), ban-crash cases (:355-553), `shutdown` (:518) | outbox replay, fsync fencing, crash mid-write, v1 acks, ban journal recovery |
| `V/mod/tests/native_transport_tls.py` | TLS verification |

Gaps: no test of per-tick budget overrun / single slow job; no test of mutation reported 503 while effect lands (late effect); no test that stale snapshot is served after a game-thread timeout; no shutdown-with-stalled-tick join test; no ring (5000) overflow-before-poll test; panel-loader test exists (`run-panel-loader.sh`) but not on a real host.

## 10. Top risks

| # | Risk | Sev | Follow-up |
|---|---|---|---|
| 1 | Timed-out but started mutation (give/teleport/kick, `S/actions.cpp:282-302`) still lands after a 503 to Takaro; a retry duplicates the effect; give runs up to 3 × 5 s serially on the single action worker | high | Return a distinct "outcome unknown" (504) and add per-request idempotency / late-result log, with a deterministic slow-job test |
| 2 | `PlayerRows` silently returns an arbitrarily old snapshot when the refresh times out (`S/actions.cpp:571-576`); `ageMs` is not surfaced to Takaro | med | Cap stale age (e.g. 2×TTL) or mark rows stale; test with a stalled pump |
| 3 | Panel install depends on the game probing an optional `libsteam.so` (`V/README.md:112`) and README/connector.json still contradict each other on LD_PRELOAD being required | med | Add a target-level check that the probe still happens per new build; align `V/README.md:3` and `V/connector.json` hint |
