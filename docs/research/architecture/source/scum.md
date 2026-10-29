# SCUM: source-level architecture (issue #298), source-only

All claims are source-only at HEAD 3859fd8. Paths are relative to the repo root. `S/` = `games/scum/`,
`L/` = `games/scum/mod/TakaroSCUM/Scripts/takaro/`, `N/` = `games/scum/native/src/`.

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | In-process: a UE4SS mod inside `SCUMServer.exe`, no sidecar | S/README.md:3; catalog/scum/game.json:37 |
| Split | A Lua mod (protocol, router, events, game adapter) plus a UE4SS **C++ mod** `dlls/main.dll` that only holds the WebSocket and registers 12 `takaro_*` Lua functions. It has no game symbols. | S/DEVELOPMENT.md:6-10; N/mod_main.cpp:1-2,237-257 |
| Runtime | Lua 5.4 in UE4SS (one Lua state); C++ is MSVC-ABI and cross-compiled with clang-cl/lld-link against the MSVC CRT 14.44 and WinSDK 10.0.26100 | S/DEVELOPMENT.md:37-41; native/PROVENANCE.md:13-15 |
| Loader | UE4SS v3.0.1-1181-g83330635 is bundled and pinned by sha256. It loads through the `dwmapi.dll` proxy in `SCUM\Binaries\Win64`. Only the `HookEngineTick` and `HookUObjectProcessEvent` hooks are enabled, because the stock hook set crashes the server on player join. | catalog/scum/targets/windows-25803875.json:124-128; S/README.md:55,71; native/ue4ss/UE4SS-settings.required.txt:1-23; S/DEVELOPMENT.md:45-49 |
| Transport library | WinHTTP WebSocket in async mode, ported from Enshrouded with one supervisor thread, one send thread and one receive thread per epoch | N/transport_winhttp.h:1-10; N/transport_winhttp.cpp:651-766 |
| TLS | Uses the system trust store by default (no `SECURITY_FLAG_IGNORE_*`). If `TAKARO_CA_FILE` is set, only unknown-CA is relaxed and the chain is pinned to that file before identify. A bad CA file fails closed. TLS 1.2 is required, TLS 1.3 is used when available. There is no insecure fallback. Non-`wss://` URLs are refused. | N/transport_winhttp.cpp:498-503,509-526,570; N/transport_winhttp.h:9-10; N/bridge.cpp:43-47 |
| Default target | `windows-25803875` (`default: true`) with **`support.status: "candidate"`**. No SCUM target is `maintained`. | catalog/scum/targets/windows-25803875.json:7-11 |
| Connector version | 0.1.0 | S/version.txt:1; L/app.lua:18 |
| Runtime CI | None. The Windows server runs in no container and "runtime verification is off". CI runs only the Lua tests in Debian. | catalog/scum/targets/windows-25803875.json:149; .github/workflows/scum.yml:59-69 |

## 2. Drift vs the issue

| Hypothesis | Status |
|---|---|
| (none) Issue #298 has no SCUM section. SCUM did not exist at 147acb21 (`git show 147acb21:games/scum` gives "exists on disk, but not in"). | New family. Per P1 it needs an appended section. |
| DRA-2 "shared native runner passes stack references into work that continues after timeout" | **Not applicable as stated.** SCUM has no `gamethread.cpp` (the dragonwilds/vein/dune runner). Game work is Lua that runs synchronously on the game thread (L/game.lua:1-4). The Lua analogue does exist: a deferred mutation (delayed kick or ban) still runs after the request timed out. See §5. |
| VEI-2 "per-tick budget; one slow job exceeding the budget" | **Partly applies.** Only the catalogue build has a time budget: 4000 µs per tick (L/game.lua:33,719-723). Request handling is bounded only by notice count, 500 per tick (L/app.lua:19,165-169). A synchronous `giveItem` of up to 50 spawns runs inside one tick (L/game.lua:37,383-421). |
| VEI-3 "pong confirmation ≠ durable processing" | **Applies to v0 only.** v0 confirms on pong (L/protocol.lua:126-128; N/transport.h:4-9). v1 uses a cumulative `eventAck` per stream/seq (L/protocol.lua:303-318) and is the default (TakaroConfig.txt.example:13). |

## 3. C1 install (ordinary renter)

| Step | Detail | Ref |
|---|---|---|
| Files | Unzip into `SCUM\Binaries\Win64`. This adds `dwmapi.dll`, `ue4ss\` and `ue4ss\Mods\TakaroSCUM\` (Lua scripts and `dlls\main.dll`). | S/README.md:53-55; catalog/scum/targets/windows-25803875.json:131-136 |
| Loader | The bundled UE4SS proxy DLL. An existing UE4SS must be renamed first. | S/README.md:54 |
| Config | Copy `TakaroConfig.txt.example` to `TakaroConfig.txt` and set `REGISTRATION_TOKEN`. | S/README.md:56-67 |
| Launch args/env | None | S/README.md:44-69 |
| Writable state | In the mod folder: `TakaroIdentity.txt`, `known-players.json`, `takaro-bans.json`, `items-cache.json`, `entities-cache.json`, `takaro.log` (8 MiB rotation). Outside it, the connector **writes** `Saved/Config/WindowsServer/BannedUsers.ini` and reads `Saved/SaveFiles/Logs`. | L/app.lua:22-23,79,192; L/game.lua:102-106,686,795-811,854-857; N/log.cpp:13; catalog/scum/targets/windows-25803875.json:142-148 |
| Restart | Stop the server, unzip, start it. Updates replace `Scripts\` and `dlls\main.dll`. | S/README.md:96 |
| Outbound | WSS to `connect.takaro.io:443` | TakaroConfig.txt.example:3 |
| Class | **Arbitrary native libraries.** `dwmapi.dll` is a DLL search-order proxy, and `main.dll` is a native C++ mod. This is not a managed plugin and not a script-only mod. | S/README.md:55 |
| Host control | File upload into the **executable directory** (`Binaries\Win64`) and permission to run unsigned native DLLs. Hosts that allow only `Saved/` or config edits cannot install it ("ask support to place the files", S/README.md:48). | Demonstrated only in a maintainer test environment; renter install is **documented but untested**. |

## 4. C3 thread trace (source-only)

The receive and send threads only move bytes (N/transport_winhttp.cpp:698-716, 651-693). **Every** decode, game access, encode and log line runs on the UE4SS game thread through `LoopInGameThreadWithDelay(100ms)` (S/mod/TakaroSCUM/Scripts/main.lua:38,46-52) and hooks.

| Op | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| getPlayers | Game thread: `FindAllOf(PlayerController)` plus reflection per player (L/game.lua:197-223,251-256) | Game thread: `json.decode` of the frame (L/protocol.lua:108) and `json.encode` of the response (L/protocol.lua:69-75) | Lua calls `takaro_send`, which locks and enqueues (N/bridge.cpp:462-481). The WinHTTP send thread writes it (N/transport_winhttp.cpp:671-676). | none | `log:debug` only (L/actions.lua:135) |
| giveItem | Game thread, synchronous: spawn plus pickup RPC, up to `MAX_GIVE=50` iterations (L/game.lua:37,366-436). It **holds a live UObject `item` in `handQueue`** across ticks. That is game-thread only and re-checked with `valid()` after 1.5 s (L/game.lua:38,154-172,423-425). | game thread | as above | none | Game thread: `log:info` calls `takaro_log`, which does a synchronous `fwrite`+`fflush` under a mutex (L/game.lua:434; N/log.cpp:51-69) |
| player-connected | The login log is tailed every 1 s on the game thread with `io.open`/`seek`/`read` up to 1 MiB per poll (L/game.lua:34,1322-1325,1372-1397; L/scumlog.lua:128-150). Reconcile also happens through getPlayers (L/game.lua:1037-1042). | Game thread: `send_event` encodes (L/protocol.lua:244-256). v1 re-encodes on flush (L/protocol.lua:270-273). | enqueue, then the send thread | `known-players.json` every 30 s if dirty, on the game thread (L/app.lua:189-193) | game thread, synchronous fflush |
| chat-message | The ProcessEvent hook on `ChatRpc`, on the game thread (L/game.lua:1266-1288) | game thread | enqueue | none | game thread |

No worker holds live game objects. The C++ side never sees UObjects (N/mod_main.cpp:1-2).

Other periodic game-thread disk I/O:
- `BannedUsers.ini` is read every 10 s (L/game.lua:35,1355-1363,846-852).
- The catalogue JSON cache is written once (L/game.lua:745-747).

## 5. C4 queues

| Queue | Capacity | Admission | Drain | Timeout / lifetime | Shutdown | Ref |
|---|---|---|---|---|---|---|
| Native inbox (transport to Lua) | 4096 items / 32 MiB | Lifecycle notices are always accepted. When frames overflow, `Push` returns false and the transport drops the link ("bridge overloaded"). | ≤ `MAX_NOTICES_PER_TICK=500` per 100 ms tick, no time budget | n/a | Cleared on `DisconnectAsync` (Lua stop) or a new `Connect` | N/bridge.h:51-52; N/bridge.cpp:377-402,425,452; N/transport_winhttp.cpp:716; L/app.lua:19,165-169 |
| Lua `pending` requests | unbounded table | every request | `Client:tick` once per tick | 8500 ms (testReachability 4000 ms), then a fallback reply. A late `done()` is ignored, but **its side effect still happens.** | in memory, lost | L/protocol.lua:19-20,200-209,349-360 |
| `delayedKicks` (kick/ban) | unbounded | When the player is online, it shows the reason first and kicks 2000 ms later | Every tick, at due time | **Runs after a timeout.** `App:tick` calls `client:tick()` (timeouts) **before** `game.tick` (L/app.lua:170-175). After a game-thread stall longer than 8.5 s, Takaro first gets `success=false`, then the kick executes in the same tick (L/game.lua:1327,314-331). For a ban, `BannedUsers.ini` and `takaro-bans.json` are already written synchronously, before the kick is deferred (L/game.lua:861-878). A timed-out ban is therefore fully in effect. | lost (memory) | L/game.lua:43,304-331 |
| `handQueue`, `kickQueue` | unbounded; `kickQueue` retries 30 times | internal | per tick | not tied to requests | lost | L/game.lua:154-172,1341-1353 |
| Lua event queue + inflight | `EVENT_QUEUE_LIMIT=1000` (queued + inflight). Oldest **queued** event dropped first. | every event | Flushed each tick until the transport refuses | v1: 30 s without an ack, then reconnect and resend | in memory, lost | L/protocol.lua:18,21,258-263,265-288,361-369 |
| Native outbound `FrameQueues` | Control 64/1 MiB, Critical 256/1 MiB, Response 256/32 MiB, Event 1024/8 MiB. Max frame 8 MiB, max inbound 1 MiB. | `Full`/`TooLarge` returned to Lua. A refused response is only logged (L/protocol.lua:233-239), so Takaro waits out its own 10 s. | One send thread, one send outstanding, strict priority, 30 s write-complete wait | Cleared at epoch open and close; events are re-queued from Lua inflight | `~TakaroScumMod` calls `Disconnect()`, which joins the supervisor. Teardown can wait up to 35 s (send) plus 10 s plus 5 s. | N/transport.cpp:17-25,27-40; N/transport.h:57-58; N/transport_winhttp.cpp:564,624,651-693,603-613; N/mod_main.cpp:272-275 |

Started-job lifetime: there are no cross-thread jobs. All mutation runs on the game thread inside the tick, so caller stack data cannot be outlived. The DRA-2 C++ use-after-scope class does not exist here. Only the "mutation after timeout" outcome class above exists.

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation |
|---|---|---|---|---|
| Players online | Game (`FindAllOf` per call) | `online` set used for event dedup | memory | Rebuilt silently at start (L/game.lua:1281-1287) |
| Last location | Game | `lastPos` (live read, login log, 1 Hz death poll) | memory, no eviction | none (L/game.lua:266-278,1046) |
| Inventory | Game (live Children walk) | none | none | n/a (L/game.lua:598) |
| Item/entity catalogue | Game assets | `catalogs` plus `<kind>-cache.json` keyed by engine version, count and schema | file | Key change rebuilds it. Partial lists are served while building (L/game.lua:660-698,752-762). |
| Identities / known players | Steam ID from the game | `known-players.json` (name, IP) | file, 30 s | none (L/app.lua:100-106,189-193) |
| Bans / timed bans | `BannedUsers.ini` (game) plus `takaro-bans.json` (reason, expiry, gameLoaded, pendingUnbans, bootKey) | both files | file | Boot key is the GameMode address. Expiry is enforced by the connector every 10 s. (L/game.lua:813-857,966-977) |
| Event cursor | SCUM log files | Tailer offset | **memory**. The first poll starts at EOF, so lines written while the mod was down are skipped. | Newest file plus truncation reset (L/scumlog.lua:128-147) |
| Pending events / outbox | connector | Lua `events` and `inflight` with a per-process `streamId` | **memory** | A new stream after restart (L/protocol.lua:23-29,48,57-58) |
| Identity token | connector | `TakaroIdentity.txt` | file | (L/app.lua:69-83) |

## 7. C6 delivery

| Item | Value | Ref |
|---|---|---|
| Outbox bound | 1000 Lua events; native Event queue 1024/8 MiB | L/protocol.lua:18; N/transport.cpp:22 |
| Ack | v1 (default): a cumulative `eventAck{streamId,upTo}`, plus `eventError` treated as consumed. v0: the pong of a later ping. | L/protocol.lua:126-128,161-169,303-318; N/transport.h:83-88 |
| Reconnect replay | On close, inflight events are re-queued at the front with the same seq, so delivery is at-least-once. A 30 s ack timeout forces a reconnect. Backoff is 2 s × 2^n, capped at 60 s, and resets on identify. | L/protocol.lua:115-123,338-346,361-369; N/transport_winhttp.h:27; N/transport_winhttp.cpp:412-413 |
| Duplicate risk | There is no abortive close. Bytes left in the kernel send buffer can be delivered after a "dead" link, and are then replayed. Conan added an `SO_LINGER{1,0}` fix for exactly this; the SCUM and Enshrouded copies lack it. v1 seq should dedupe on the Takaro side. | games/conan-exiles/native/platform/windows/abortive_close.h:2-9; N/transport_winhttp.cpp:603-613 |
| Lost on game crash | All queued and inflight events, pending requests, delayed kicks, last positions, online set and the log cursor. On restart the tailer reads the new log file from offset 0, but old-file lines after the last poll are lost. | §6 |

## 8. C8 shared code

| SCUM file | Sibling | Diverged? |
|---|---|---|
| N/transport.h, N/transport.cpp | games/enshrouded/mod/src/native/transport.{h,cpp} | Only the header comment and include path differ (2-line diff) |
| N/transport_winhttp.{h,cpp} | games/enshrouded/mod/src/native/transport_winhttp.{h,cpp} | Includes, user agent, and pong detection (substring `IsTypeFrame` vs full JSON parse). 27 diff lines in total. |
| N/transport_winhttp.cpp | games/conan-exiles/native/platform/windows/transport_winhttp.cpp | **Diverged** (217 diff lines): Conan adds the abortive close (`abortive_close.h`), a `takaro` namespace, `WinHttpConfig`, `connect` on Retarget and retarget stats |
| N/transport.cpp | games/conan-exiles/native/core/takaro/transport.cpp | Same queue limits; 23 diff lines |
| N/bridge.cpp | games/enshrouded/mod/src/native/bridge.cpp | Unrelated. Enshrouded's bridge (1191 lines) holds the protocol in C++; SCUM's is a 166-line Lua shim. |
| no gamethread runner | games/{dragonwilds,vein,dune}/mod/src/gamethread.cpp | Not used. Dragonwilds' transport is libwebsockets/OpenSSL (games/dragonwilds/mod/src/native_transport.cpp:3-4). |
| L/json.lua | rxi/json.lua | Vendored (native/PROVENANCE.md:8) |

There are three hand-maintained copies of the WinHTTP transport (Enshrouded, SCUM, Conan), and fixes already drift between them.

## 9. Existing tests (C4/C6) and gaps

| Test | Covers |
|---|---|
| S/tests/test_wire.lua:313 "a handler that never finishes is answered with its fallback before Takaro's 10 s budget" | Timeout fallback. A late `done()` is not answered twice. A deferred `giveItem` gets `success=false`; the late completion is not checked for side effects. |
| S/tests/test_wire.lua:174, :199 "WIRE-5 (Lua side) …" | No stale response across epochs; events survive a drop and are resent |
| S/tests/test_v1.lua:151, :186 | v1 seq/eventAck resend; 30 s ack timeout reconnect |
| S/tests/test_events.lua:110 "the event queue is bounded while Takaro is unreachable (oldest dropped)" | `EVENT_QUEUE_LIMIT` |
| S/tests/test_game.lua:224, :264, :343, :353, :779, :835 | Delayed kick, ban files, timed-ban expiry, budgeted catalogue, hot-reload dedup, per-tick perf counter |
| S/native/smoke/smoke_main.cpp:1-4 | Manual Windows smoke: identify with bogus tokens, forced close, reconnect. Not in CI. |

Gaps:
- There are no C++ host tests for SCUM's copy of `FrameQueues`, `Heartbeat` or `Bridge` (Enshrouded has games/enshrouded/mod/tests/native/bridge_test.cpp and unit_test.cpp).
- No test covers inbox overflow, a full Response queue (response dropped silently), timeout-then-kick ordering, game-thread stall, log-cursor loss on reload or crash, or long teardown.
- There is no runtime CI.

## 10. Top risks

| # | Severity | Risk | Follow-up |
|---|---|---|---|
| 1 | med | A mutation runs after timeout. Kick/ban defer 2 s, timeouts are checked before `game.tick`, and a ban's file writes happen before its reply. After a stall Takaro sees a failure for a kick or ban that actually happened (L/app.lua:170-175; L/game.lua:861-878,314-331). | Answer deferred mutations as "accepted" immediately, or check deadlines after `game.tick`. Add a stall test. |
| 2 | med | The renter install needs native DLL drop-in rights in `Binaries\Win64` (a dwmapi proxy plus `main.dll`) and pins one UE4SS build with custom hooks. Many hosts will not allow it, and the target is only `candidate`. | Survey host policies for UE4SS on SCUM. Keep `candidate` until a renter install is demonstrated. |
| 3 | med | The WinHTTP transport is copied three times and has drifted. The Conan abortive-close duplicate fix is missing in SCUM and Enshrouded, and SCUM has no native unit tests. | Extract one shared Windows transport (with tests) for Enshrouded, SCUM and Conan. |
| (4) | low | Game-thread I/O has no time budget: synchronous `fflush` logging, the 1 s log tail (up to 1 MiB), the 10 s `BannedUsers.ini` read, and `giveItem` up to 50 spawns in one tick. | Use the existing `perf` counters (L/game.lua:1366-1369) under load and add a request time budget. |
