# Dragonwilds: issue #298 analysis (source-only, HEAD 3859fd8)

All paths are relative to the repo root; `src/` means `games/dragonwilds/mod/src/`. I read the source only: nothing was built, run or tested live.

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | In-process native `.so` loaded with `LD_PRELOAD` into `RSDragonwildsServer-Linux-Shipping`, with no sidecar | `games/dragonwilds/README.md:3-5`, `games/dragonwilds/INSTALL.md:3-4` |
| Loader | ELF `__attribute__((constructor))`. It attaches only if the exe name contains `RSDragonwildsServer`, then starts one init thread | `src/main.cpp:143-151` |
| Runtime | C++17. Engine addresses come from the depot's `.sym` at load time; offsets come from UE reflection | `games/dragonwilds/DEVELOPMENT.md:9,25-30,109-121` |
| Game-thread pump | Vtable-slot detour on `UJgxGameEngine/UGameEngine/UDomGameEngine::Tick` | `src/gamethread.cpp:117-130,232-244` |
| Transport | libwebsockets + OpenSSL, owned by one LWS service thread | `src/native_transport.cpp:214-299`, `games/dragonwilds/DEVELOPMENT.md:74-78` |
| TLS verification | On. The CA file defaults to `/etc/ssl/certs/ca-certificates.crt` and is pre-checked with `X509_STORE_load_file`. `LCCSCF_USE_SSL` is set without the ALLOW_SELFSIGNED or SKIP_HOSTNAME flags (grep finds none) | `src/native_transport.cpp:220-233,287`; README `TAKARO_CA_FILE` "certificate checks always stay on" `README.md:81` |
| Default catalog target | `linux-25991736`, with `default: true` but `support.status: candidate` | `catalog/dragonwilds/targets/linux-25991736.json:3,7,9` |
| Newest `maintained` target | `linux-25630937` (`default: false`) | `catalog/dragonwilds/targets/linux-25630937.json:3,7,9` |
| Connector version | `0.5.0` | `games/dragonwilds/version.txt:1` |
| Doc drift | README says it was "tested live against 25808123", but the default is 25991736 | `README.md:16-17` |

## 2. Drift vs the issue

The issue #298 text is not available locally (I checked the repo and the scratchpad, with no network). I took the old hypotheses from revision `147acb21`.

| Old state @147acb21 | Now | Verdict |
|---|---|---|
| Plugin + Node sidecar (v0.2.0, "Install both") `git show 147acb21:games/dragonwilds/README.md:3-9`, `sidecar/` tree | The native `.so` holds the Takaro WSS itself (`src/native_bridge.cpp:1614-1615`) and the sidecar is gone | **Obsolete** |
| "The native runner passes stack references into work that can continue after timeout" | At 147acb21 this was **true**: `RunJson` captured `[&]` with a stack `std::string result` (old `gamethread.cpp:201-211`), and `actions.cpp:1021` had `Run([&]{ built = BuildItems(); })`. At HEAD, `RunJson` owns `fn` and the result through `shared_ptr` (`src/gamethread.cpp:289-302`). Every `GameThread::Run` call site captures by value or by `shared_ptr` (table in section 5) | **Obsolete for Dragonwilds** (fixed in #352, commit 6682328) |
| A timed-out started job still runs | Unchanged: `src/gamethread.cpp:273-276` cancels only jobs that have not started | **Still true** (now memory-safe, but the side effect can still apply after the caller sees a failure) |
| Queue cap of 256 | Still 256. The check was `>` and is now `>=` (`src/gamethread.cpp:266`) | Still true |

## 3. C1 install (ordinary renter)

| Step | Ref |
|---|---|
| Linux dedicated server (app 4019830) with `RSDragonwildsServer-Linux-Shipping.sym` next to the binary | `README.md:11-14` |
| Copy `libtakaro-dragonwilds.so` **outside** the Steam tree into a writable dir. `takaro.cfg` is created next to it | `README.md:27-32,38-45`, `INSTALL.md:12-14` |
| Paste the registration token into `takaro.cfg`. Hot reload covers URL, token, identity and name | `README.md:40-45`, `DEVELOPMENT.md:56-60` |
| A writable, persistent `TAKARO_STATE_DIR` (needed with Docker) | `README.md:77`, `INSTALL.md:15-16` |
| `LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log`, on the game binary only, because SteamCMD is 32-bit | `README.md:89-94`, `INSTALL.md:21-27` |
| Restart the server | `INSTALL.md:28` |
| Outbound WSS to `wss://connect.takaro.io/` | `README.md:62,74` |

- **Classification:** an arbitrary native library injected by `LD_PRELOAD`. It is not a managed or script plugin, and there is no mod loader.
- **Host control needed:**
  - edit the game's launch command or entrypoint;
  - writable bind mounts for the plugin dir and the state dir;
  - a glibc that matches Bookworm (`DEVELOPMENT.md:80-82`).
- `games/dragonwilds/connector.json` lists "Control of the Dragonwilds Linux dedicated server startup command" (`games/dragonwilds/connector.json:6-8`), so ordinary game-server renters without a startup-command override cannot install it.
- Contradiction: `DEVELOPMENT.md:195` says the plugin dir is "mounted read-only", while `README.md:96` and `INSTALL.md:12` require it writable for `takaro.cfg`.

## 4. C3 thread trace (source only)

Threads involved:
- **GT**: the engine game thread inside the Tick detour.
- **AW**: the bridge action worker, `ActionLoop`.
- **BW**: the bridge worker, `BridgeLoop`.
- **LWS**: the transport service thread.
- **IT**: the init/housekeeping thread, `main.cpp:129-137`.

| Step | getPlayers (`Actions::Players`, `src/actions.cpp:1151-1165`) | giveItem (`Actions::Give`, `src/actions.cpp:1600-1640`) | player-connected (`EmitJoin`, `src/events.cpp:362-367`) |
|---|---|---|---|
| Game access | GT: `ReadPlayers()` inside `OnGameThread` (`actions.cpp:1152-1154`). It nulls `playerState/controller/pawn` before leaving GT (`:1154`) | GT: `FindPlayerById` and `TryGiveItemToPlayer(controller, asset, amount)` (`actions.cpp:1626-1637`). Catalogue lookup and `Housekeep()` run on AW first (`:1613-1624`) | GT: the PostLogin hook, then `ResolvePendingJoins` in the GT housekeeping job (`events.cpp:1847-1860`, `:712`) |
| JSON build | AW: the `render` lambda runs after the job (`actions.cpp:1155-1163`, called at `:213`) | GT builds the small result string (`actions.cpp:1635-1636`); AW re-serialises it in `NativeBehavior` (`native_behavior.cpp:748`) | BW: `EmitEventDeferred` stores a lambda over `Ident` strings, with no pointers (`events.cpp:55-64,364`). It is serialised in `EventsJson` on BW (`native_bridge.cpp:626`, `state.h:28-30`) |
| Network send | `NativeTransport::Queue` from BW; the write happens on LWS (`native_transport.cpp:361-394`) | same | same, through the durable outbox in `FlushEvents` (`native_bridge.cpp:762-775`) |
| Disk write | none (the state mirror goes through BW `Commit`) | none for give. Ban/unban: `state::FlushBans` on IT or AW (`actions.cpp:1131`) | BW: `durable->AdmitMany` (fsync file and dir, `native_persistence.cpp:36,56`; `native_bridge.cpp:666`) |
| Logging | `PluginLog` only enqueues (`common.cpp:115-132`); `FlushPluginLogs` writes from IT or BW (`main.cpp:134`, `common.cpp:136-160`) | same | same (`events.cpp:366`) |

Workers holding live game pointers:
- `ItemDef::asset` (a `void*` UObject) lives in `g_items` (`actions.cpp:395-401`). AW reads it under `g_itemLock` and passes it by value into the GT job (`actions.cpp:1621,1626`). It is never revalidated: it assumes the catalogue "never changes afterwards" (`actions.cpp:1133`).
- `DebugObject` passes a user-supplied `ptr` (debug only, `http.cpp:274-283`).
- `g_conns[].controller` lives under `g_connLock`, but it is touched by GT hooks and GT jobs only (`events.cpp:351-358`).

## 5. C4 queues

| Queue | Capacity / admission | Drain | Timeout / lifetime | Ref |
|---|---|---|---|---|
| **GameThread `g_jobs`** (any thread to GT) | 256. `Run` returns false without admitting (`gamethread.cpp:266`). Cancelled jobs **stay in the deque** and count toward 256 until popped (`:83`; test `game_thread_timeout_test.cpp:74-79`) | Per tick: at most `kJobsPerTick=16` (`gamethread.h:14`; loop `gamethread.cpp:75`). Time budget `TAKARO_TICK_BUDGET_US`, default 500 µs, clamped 50–33000 (`gamethread.cpp:45-49`), checked only **between** jobs. The first job always runs (`:76`), so a single long job is unbounded. Popped cancelled jobs use up one of the 16 slots (`:83`) | Default 5000 ms (`gamethread.h:21`). On timeout: if the job has not started, `cancelled=true` and the job never runs. If it **has started, it keeps running**, and `Run` returns false (`gamethread.cpp:273-276`). `started` and `cancelled` are flipped under `g_q` (`:79-85`, `:270-277`), so there is no race. **The started job cannot outlive caller stack data**: `RunJson` puts `fn` and the result in the job's `shared_ptr` (`:289-302`), and every call site captures by value or `shared_ptr` (`actions.cpp:195-198,1045,1140`, `http.cpp:255,281,304,315`, `mapsurvey.cpp:98-101,128-135,235-238`, `events.cpp:1847-1848`, `main.cpp:106`). The only `[&]` lambdas in `src/` are local helpers, never passed to `Run` (grep). Residual: a timed-out **mutation** (give, teleport, ban) may still apply after AW reports `503 "game thread unavailable"` (`actions.cpp:209-211`). The job's captures are destroyed on GT | `src/gamethread.cpp`, `src/gamethread.h` |
| Bridge notices (LWS to BW) | Frames: 512 / 8 MiB. Critical (non-frame): 128. Full means the sink returns false and the socket is closed | Each pass drains everything (`native_bridge.cpp:1064-1075`) | n/a | `native_bridge.cpp:1602-1609` |
| Action queue (BW to AW) | 128 pending IDs, 128 queued+running+reserved, 4 MiB; over that, `"native action queue overloaded"` | **One action at a time**: `actionApplied` gates the next pop until BW has applied the completion (`native_bridge.cpp:1390-1394`) | Answer deadline 8500 ms (`TAKARO_REQUEST_DEADLINE_MS` 1000–9500; `:104,1526`). If the action is still queued, it is dropped unexecuted. If it is running, it **finishes, its effects apply, and its late answer is suppressed** (`:1015-1056`) | `native_bridge.cpp:606-609` |
| Completions (AW to BW) | 128 / 16 MiB. AW blocks on `completionCv` | n/a | n/a | `native_bridge.cpp:1449-1470` |
| Transport outbound (BW to LWS) | Control 128/1 MiB, CriticalResponse 128/1 MiB, Response 128/16 MiB (8 MiB max frame), Event 5000/32 MiB; responses and critical responses share 16 MiB. Full means `QueueStatus::Full`, and an overload `SendError` triggers a 1013 close | written on the LWS thread | Stale epoch is rejected | `native_transport.cpp:361-394`, `native_bridge.cpp:307-317` |
| Inbound frame | 1 MiB | | | `native_transport.cpp:104,215` |
| Event ring (GT to BW) | `kMaxEvents=5000`, drop-oldest (`state.cpp:56`). Truncation is recorded as a loss (`native_bridge.cpp:642-656`) | BW reads 256 per pass (`:626`) | n/a | `state.h:36` |
| Plugin log queue | 1024 lines / 1 MiB, drop-oldest | IT and BW flush | | `common.cpp:124-131` |

Shutdown with pending work:
- `ShutdownPlugin` joins IT, then `NativeBridge::Stop()`:
  - it joins BW and AW, and AW can sit up to 5 s in `GameThread::Run`;
  - it clears the action, notice and completion queues **without answering them** (`native_bridge.cpp:1623-1641`).
- After that comes `Hooks::RestoreAll()` (`main.cpp:30-37`). Queued GT jobs then never run; they own their data, so nothing dangles.
- `mapsurvey` runs on its own detached thread with a 30 s job timeout (`mapsurvey.cpp:134-154`), and shutdown does not join it.
- Housekeeping ignores `Run`'s return value (`events.cpp:1847`).

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation |
|---|---|---|---|---|
| Players online | Game (read on GT, `actions.cpp:1151`) | `derivedOnline` in the outbox snapshot, ≤500 (`native_persistence.cpp:171`); legacy `online-players.json` (`:206`) | file (fsync) | Reconciled every 30 s against the live list (`native_bridge.cpp:1330-1331`). Synthetic `player-disconnected` for ghosts (`native_behavior.cpp:1310-1329`) |
| Last location | Game, on demand | `eventLocationWindows` (memory, ≤500, 60 s) | none | time window (`native_bridge.cpp:119-122,243`) |
| Inventory | Game, on demand (GT) | none | none | n/a |
| Item catalogue | Game assets | `g_items` (memory, `void* asset`) | none | built once, **never invalidated** in-process (`actions.cpp:1133-1145`); rebuilt on restart |
| Identities | Takaro identity: `takaro.cfg` (`DEVELOPMENT.md:52-55`). Player identities: `known-players.json` ≤500 (`native_persistence.cpp:126-138,208`) | file | file | none |
| Bans | Game `DedicatedServer.ini` via `SetBannedUsers` (`DEVELOPMENT.md:196-198`) + plugin `bans.json` enforced at PreLogin/PostLogin (`state.cpp:167`, `state.h:54-59`) | `bans.json` (plugin data dir) | file | revision counter (`state.h:77,82`) |
| Timed bans | Connector (the bridge lifts them on expiry) | `timed-bans.json` + `ban-intent.json` (two-file intent) | file (fsync) | `native_persistence.cpp:207,210`; `native_persistence.h:71-75` |
| Event cursor | Connector | `event-cursor.json` {bootId, seq} | file | `SwitchBoot` on a new bootId (`native_bridge.cpp:629-637`) |
| Pending events / outbox | Connector | `event-outbox.json`, ≤5000 / 32 MiB, drop-oldest with `deliveryLosses++` | file, fsync + dir fsync (`native_persistence.cpp:36,56,284-288`) | ConfirmThrough on ack |
| Symbol cache | `.sym` | `symcache.json` keyed by build-id | file | changes when the build-id changes (`DEVELOPMENT.md:114-116`) |

## 7. C6 delivery

| Aspect | Behaviour | Ref |
|---|---|---|
| Outbox bound | 5000 events / 32 MiB, drop-oldest, counted in `deliveryLosses` | `native_persistence.cpp:284-288` |
| Ack v0 | No acks. The **pong after the write** confirms (`pingAfterWrite`, then a `Confirmed` notice) | `native_bridge.cpp:1146-1150`, `native_transport.cpp:129,183-184` |
| Ack v1 | `eventAck {streamId, upTo}`, capped at `highestSentSeq`. 30 s without an ack forces a 1013 reconnect and resend | `native_bridge.cpp:555-561,136,1333-1338` |
| Seq stability | v1 `seq` = durable outbox id, so a resend keeps the same seq | `native_bridge.cpp:297-305` |
| Reconnect replay | All `pending` entries not yet sent in this epoch are resent after identify; nothing goes out before identifyResponse | `native_bridge.cpp:762-775`; test `native_full_bridge.py:581 outage` |
| Lost on game crash | (a) Events still in the in-memory ring (≤5000) not yet admitted by BW (BW polls about every 100 ms, `native_bridge.cpp:1071`). (b) A running GT mutation whose result was never answered. (c) The in-memory catalogue and location windows. Online players are fixed up by reconciliation after restart (`native_behavior.cpp:1311-1312`). The admitted outbox survives (fsync) | |
| Log events | Rate-limited, 40 per 30 s by default. Over-limit entries are marked sent and counted as `rawLogLosses` | `native_bridge.cpp:746-757,768-772` |

## 8. C8 shared code (`diff` line counts vs siblings)

| File | vs `games/vein/mod/src/` | vs `games/dune/mod/src/` | Notes |
|---|---|---|---|
| `gamethread.cpp` | 26 lines: names only (`Sym::`→`Resolve::`; Tick symbols `UVeinGameEngine/UEngine`). **Queue, timeout and RunJson logic identical** | 101 lines | Conan `games/conan-exiles/native/core/gamethread.cpp`: 374 lines differ (separate design) |
| `gamethread.h` | 2 (comment) | 22 | |
| `perf.cpp` | 0 | 0 | exact copy |
| `native_persistence.h` / `.cpp` | 0 / 4 | – | near copy |
| `state.cpp` | 4 | 238 | |
| `hooks.cpp` | 17 | 38 | |
| `native_transport.cpp` | 183 | – | diverged |
| `native_bridge.cpp` | 540 | – | diverged |
| `native_behavior.cpp` | 1932 | – | game-specific |
| `http.cpp` / `common.cpp` / `reflect.cpp` / `config_file.cpp` / `native_log.cpp` | 80 / 141 / 172 / 256 / 417 | 314 / 223 / 464 / – / – | diverged forks |

The code is copy-and-diverge with no shared library. A fix to the runner or transport must be ported by hand to each copy.

## 9. Existing tests for C4/C6, and gaps

| Test | Covers |
|---|---|
| `mod/tests/game_thread_timeout_test.cpp:28-31` | A queued job past its deadline is cancelled and never runs |
| `mod/tests/game_thread_timeout_test.cpp:33-51` | A started job outlives the caller; its owned payload stays valid |
| `mod/tests/game_thread_timeout_test.cpp:53-71` | `RunJson`: the caller's `output` is untouched after a timeout; the job finishes safely |
| `mod/tests/game_thread_timeout_test.cpp:73-79` | The 256-job cap boundary; drain of 16 per pump |
| `mod/tests/native_full_bridge.py:751 answer_deadline` | Every request is answered before the deadline while GT is stuck |
| `native_full_bridge.py:581 outage`, `:188 replay`, `:212 backlog_request_latency`, `:288 crash_write`, `:238 directory_sync`, `:621 reconcile_after_crash` | Outbox durability, replay and reconcile |
| `mod/tests/native_bridge_adversarial.py:511 overload`, `:551 stale_epoch`, `:571 completion_limit` | Bridge admission bounds |
| `mod/tests/native_transport_tls.py:145-165` | Untrusted chain, hostname mismatch and an expired certificate are all rejected |

Gaps:
- No test where a timed-out but **started mutation** (give or teleport) applies its effect while Takaro got an error. The bridge tests use a fake handler, not the real GT and `Actions::*`.
- No test for the per-tick time budget with one long job, or for cancelled jobs filling the 256 cap.
- No test for shutdown while a GT job is in flight.
- No test for catalogue `asset` pointer staleness.

## 10. Top 3 risks

| # | Severity | Risk | Follow-up |
|---|---|---|---|
| 1 | **med** | A started GT mutation outlives a 5 s `Run` timeout or the 8.5 s bridge deadline. The effect applies but Takaro sees an error, so a retry can give an item twice or ban twice (`gamethread.cpp:273-276`, `actions.cpp:209-211`, `native_bridge.cpp:1043-1044`) | Return a distinct "outcome unknown, may have applied" error for started-and-timed-out mutations, and add an idempotency key per requestId |
| 2 | **med** | The per-tick budget only applies between jobs. One long job (the housekeeping sweep with `GetObjectsOfClass`, or a map-survey chunk of 2048 traces) is unbounded on the game thread, and the first job always runs (`gamethread.cpp:76`, `events.cpp:1847-1862`, `mapsurvey.cpp:134-154`) | Record and alarm on `maxJobMs` (already in `StatsJson`). Split the sweep into bounded slices |
| 3 | **low** | Cancelled jobs keep their queue slots until a tick pops them, so a stalled pump plus retries hits the 256 cap. The cached catalogue `UObject*` is never revalidated (`gamethread.cpp:83,266`, `actions.cpp:395-401,1133`) | Erase cancelled jobs at timeout. Revalidate `asset` with `IsValidLowLevel` or a name lookup on GT before `TryGiveItemToPlayer` |
