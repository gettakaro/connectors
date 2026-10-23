# ARK: Survival Evolved — issue #298 source analysis (source-only)

HEAD 3859fd8. All paths relative to repo root; `src/` = `games/ark/mod/src/`. Nothing here was run against a game,
Takaro or the gamer PC.

## 1. Architecture on main

| Item | Value | Ref |
| --- | --- | --- |
| Placement | In-process native `.so`, loaded with `LD_PRELOAD` into `ShooterGameServer`; no sidecar, no listening socket | `games/ark/scripts/launch-exact.sh.in:16`, `src/native.cpp:1-3`, `games/ark/README.md:3-4` |
| Runtime / loader | ELF constructor `ark_native_init` (hash gate, then hooks, then bridge); destructor `ark_native_stop` restores prologues | `src/native.cpp:1397-1429`, `src/native.cpp:1431` |
| Hooks | inline detours on fixed addresses: main-loop tick `0x81fce0`, PostLogin, Logout, OnDied, chat via exec thunk + vtable slot; one exported symbol interposes `SteamGameServer_RunCallbacks` | `src/native.cpp:49-63`, `src/native.cpp:1179-1208`, `src/native.cpp:1389-1395` |
| Exact-build gate | SHA-256 of `/proc/self/exe` must equal `7e7ded49…c520` (both launcher and constructor) | `games/ark/scripts/launch-exact.sh.in:12-14`, `src/native.cpp:48`, `src/native.cpp:1076-1100` |
| Transport | own RFC 6455 client over nonblocking socket + statically linked OpenSSL 3.0 | `src/ws_client.hpp:1-4`, `games/ark/scripts/build-release.sh:20-23` |
| TLS verification | `SSL_VERIFY_PEER`, TLS ≥1.2, hostname via `SSL_set1_host` (IP via `X509_VERIFY_PARAM_set1_ip_asc`), CA = `TAKARO_CA_FILE` or first of 4 system bundles | `src/ws_client.hpp:91-96`, `src/ws_client.hpp:410-435` |
| Protocol | Generic, offers versions `[0,1]` + capability manifest | `src/takaro_bridge.hpp:56`, `src/takaro_bridge.hpp:306-327` |
| Default catalog target | `linux-21241282`, `default: true`, `support.status: "candidate"` (no `maintained` target exists) | `catalog/ark/targets/linux-21241282.json:7-9` |
| Connector version | `0.2.0` | `games/ark/version.txt:1` |

## 2. Drift vs the issue

ARK is **new since 147acb21** (`git cat-file -e 147acb21…:games/ark` → path absent). The issue has no ARK section;
it only mentions "ARK candidate #289" in P1 (issue line 22). Cross-game hypotheses that apply:

| Issue hypothesis | Status for ARK | Ref |
| --- | --- | --- |
| P1: ARK is a candidate, classify separately | Still true: target is `candidate`, but it is now native-only 0.2.0, not a sidecar | `catalog/ark/targets/linux-21241282.json:9`, `games/ark/connector.json:3` |
| C4: delayed requests can "execute after a reported timeout" | **True** for started actions: caller returns `timeout` while the game thread keeps running the action | `src/gate_state.hpp:644-648`, `src/gate_state.hpp:673-676` |
| C4: jobs can "outlive their captured data" | **Not true**: actions are `std::shared_ptr<gate::Action>`, no caller-stack references cross threads | `src/gate_state.hpp:649`, `src/native.cpp:146-167` |
| DRA-2/VEI-2/DUN-2 "shared native runner" lifetime risk | Not applicable: ARK does not use `gamethread.cpp`; it has its own gate | `src/gate_state.hpp:26-29` |
| C6: "pong is not processing" | Still true on protocol v0 (cursor advances on pong); v1 uses `eventAck` | `src/takaro_bridge.hpp:471-480`, `src/takaro_bridge.hpp:495-501` |
| C6: "sidecar owns recovery that disappears in-process" | True: event ring is memory-only, cursor is boot-scoped, game crash loses unsent events | `src/gate_state.hpp:704`, `src/takaro_bridge.hpp:186-187` |
| C3: workers must not retain live game objects | Holds: bridge reads only copies; live pointers are game-thread only | `src/takaro_bridge.hpp:9`, `src/native.cpp:144-167` |

## 3. C1 install

| Step | Detail | Ref |
| --- | --- | --- |
| Files | unzip release into `<ark>/TakaroArk/` → `TakaroArk/TakaroArkNative/libtakaro-ark-native.so`, `launch.sh` | `games/ark/README.md:37-44`, `games/ark/scripts/launch-exact.sh.in:7-8` |
| Config | copy `takaro.cfg.example` → `takaro.cfg`, set `TAKARO_REGISTRATION_TOKEN` (env vars override) | `games/ark/README.md:45-46`, `games/ark/mod/takaro.cfg.example:1-5`, `src/native.cpp:1339-1386` |
| Launch | start through `launch.sh <root> <args>`; it checks hash, `export LD_PRELOAD`, `exec` server with `-FORCELOGFLUSH`; Docker needs `--init` | `games/ark/README.md:47-54`, `games/ark/scripts/launch-exact.sh.in:12-19` |
| Writable state | `<ark>/.takaro/ark-native/` (identity-token, ban-metadata.json, known-players.json, event-cursor.json, status.json); missing/unwritable → bridge disabled | `src/native.cpp:1353-1371`, `src/takaro_bridge.hpp:174-179`, `src/takaro_bridge.hpp:679` |
| Restart | required (preload at process start) | `games/ark/README.md:58-62` |
| Network | outbound WSS to `wss://connect.takaro.io/` default; needs system CA bundle or `TAKARO_CA_FILE` | `src/takaro_bridge.hpp:38`, `src/ws_client.hpp:91-96` |
| Uninstall | delete `TakaroArk/`, start server directly | `games/ark/README.md:68` |

Classification: **arbitrary native library via `LD_PRELOAD`** (not a managed plugin DLL, no mod loader). Host control
needed: ability to replace the server start command / set environment for the server process, plus exact game build
(any update breaks it until a new target ships). FTP-only renter panels are insufficient. Renter install:
**requires host cooperation** (documented, untested on a rental).

## 4. C3 thread trace

Threads: game thread (tick/PostLogin/Logout/chat/death hooks), `drain` log thread, `takaro-io`, 4 request workers,
`takaro-maint` (`src/takaro_bridge.hpp:1-9`, `src/takaro_bridge.hpp:196-198`, `src/native.cpp:1404`).

| Phase | getPlayers (read) | giveItem (mutation) | player-connected (event) |
| --- | --- | --- | --- |
| Game access | none at request time; snapshot built on game thread: roster at PostLogin, ip/ping every 2 s, pos 250 ms (`src/native.cpp:401-420`, `src/native.cpp:642-654`) | game thread in `tick_hook`: before/after inventory snapshot + native grant + read-back (`src/native.cpp:1022-1050`); remainder 1 grant/tick (`src/native.cpp:837-867`) | `postlogin_hook` on verified game thread, bans checked from snapshot (`src/native.cpp:401-420`) |
| Snapshot copy | worker copies `players_` under gate mutex (`src/gate_state.hpp:525-531`) | inventory copied into gate (`src/native.cpp:566-573`) | `record_login` builds `Event` strings + `utc_now()` under gate mutex on game thread (`src/gate_state.hpp:299-319`) |
| JSON build | worker (`src/takaro_adapter.hpp:359-365`) | worker (`src/takaro_adapter.hpp:441-487`) | `takaro-io` in `pump()` (`src/takaro_bridge.hpp:504-533`) |
| Network send | worker `post()` → outbound deque → `takaro-io` writes (`src/takaro_bridge.hpp:295-304`, `src/takaro_bridge.hpp:420-429`) | same | `takaro-io` (`src/takaro_bridge.hpp:528`) |
| Disk write | none | none | cursor file fsync on `takaro-io` after ack (`src/takaro_bridge.hpp:490`, `src/takaro_bridge.hpp:499`); known-players on maint (`src/takaro_bridge.hpp:690`) |
| Logging | worker → `enqueue` | game thread `enqueue(label)` (string build + mutex push, cap 8192) → `drain` thread writes stderr every 100 ms (`src/native.cpp:172-201`, `src/native.cpp:203-233`) | same |

Live pointers: controllers/pawns/world are resolved per tick through weak-object validation and used only on the game
thread (`src/native.cpp:297-306`, `src/native.cpp:651`); globals `g_latest_controller`/`g_world` are atomics touched only
by hooks (`src/native.cpp:137-139`, `src/native.cpp:441-442`). `g_pending_*` hold `shared_ptr<Action>` (data only),
game-thread-only (`src/native.cpp:146-167`). No worker holds a live game object.

Game-thread work per tick beyond actions: world bootstrap probe ≤1/s, positions all players every 250 ms, ban-list
snapshot every 2 s, inventory snapshot all players every 1 s, catalog/entity paging 128 per tick until complete
(`src/native.cpp:603-614`, `src/native.cpp:642`, `src/native.cpp:704-723`, `src/native.cpp:794-825`).

## 5. C4 queues

| Queue | Capacity / admission | Drain | Timeout / lifetime | Ref |
| --- | --- | --- | --- | --- |
| Request queue (io → workers) | 64; full → request silently withheld (Takaro times out) | 4 workers | none | `src/takaro_bridge.hpp:599-605`, `src/takaro_bridge.hpp:53` |
| Gate action queue (workers → game thread) | 128; also rejects needs-online actions for offline player; `not_ready` before first tick | **1 new action per tick** (`take_action` once) | `start_wait` 3000 ms then `finish_wait` 4500 ms (adapter) / 6000 default | `src/gate_state.hpp:649-682`, `src/native.cpp:903`, `src/takaro_adapter.hpp:436-438` |
| Pending continuations (game thread) | gives ≤64 (each ≤64 rounds, ≤3000 ms), bans ≤128 (≤2500 ms, rescan every 100 ms each), kicks unbounded (≤2500 ms) | all per tick | own deadlines, then `complete` | `src/native.cpp:828-901`, `src/native.cpp:1008`, `src/native.cpp:1040`, `games/ark/mod/src/moderation_bindings.hpp:38-43` |
| Outbound frames | 1024 per connection epoch; stale epoch dropped | io thread each loop | dropped on disconnect | `src/takaro_bridge.hpp:295-304`, `src/takaro_bridge.hpp:342-345` |
| WS send buffer | 2 MiB; response frame over the bound → dropped + log; event frame → retried next pump | io flush | — | `src/ws_client.hpp:209`, `src/ws_client.hpp:339-343`, `src/takaro_bridge.hpp:427`, `src/takaro_bridge.hpp:528` |
| Event ring | 4096 events, oldest evicted; pump reads 64 per 250 ms | io thread | truncation logged once | `src/gate_state.hpp:214`, `src/takaro_bridge.hpp:505-509`, `src/takaro_bridge.hpp:51` |
| Log queue | 8192 lines, excess dropped | 100 ms | — | `src/native.cpp:200`, `src/native.cpp:232` |

**Timeout semantics (the key question).**
- Not started by `start_wait`: caller sets `cancelled=true` under `action->mutex`; the game thread checks `cancelled`
  under the same mutex before setting `executing` → never runs (`src/gate_state.hpp:667-671`, `src/native.cpp:904-907`).
  Race-free because the predicate check and the claim share `action->mutex`.
- Started: caller waits `finish_wait`, then returns `Submit::timeout`; **the action keeps running** (pending give rounds,
  ban confirmation) and later `complete()` sets `done` with nobody waiting (`src/gate_state.hpp:673-676`,
  `src/gate_state.hpp:497-503`). Adapter withholds the response (`src/takaro_adapter.hpp:485`), Takaro reports a
  timeout after its 10 s (`games/ark/generic-failure-contract.md:14-19`) → **outcome unknown; a Takaro retry can
  duplicate a grant**. Ban path keeps a durable `pending` intent for this case (`src/takaro_adapter.hpp:199-210`,
  `src/takaro_adapter.hpp:311-316`).
- **Can a started job outlive caller-stack data? No.** The job is a heap `shared_ptr<Action>` co-owned by the gate
  deque, `g_pending_*` and the caller (`src/gate_state.hpp:641`, `src/gate_state.hpp:705`, `src/native.cpp:146-167`);
  the only by-reference lambda (`[&]` predicate) runs inside `submit` itself (`src/gate_state.hpp:667`, `:673`);
  `after_write` captures the `shared_ptr` by value (`src/takaro_adapter.hpp:678`). Output is written by the game
  thread into the shared Action (`src/native.cpp:968`) and read by the caller only after `done` under the lock.
  Unit test asserts the late-completion case with ASan/UBSan (`games/ark/mod/tests/gate_state_test.cpp:184-197`,
  `games/ark/scripts/build-release.sh:36`).
- Shutdown pending: tick returns early, so queued actions are not claimed and time out at `start_wait`
  (`src/native.cpp:617-638`).

Shutdown/unload with pending work: destructor clears `g_hooks_ready` first (so `tick_hook` stops claiming actions,
`src/native.cpp:639`), then `Bridge::stop()` joins io, workers (a worker blocked in `submit` waits up to 3 s, or 7.5 s
if already executing), maint (`src/native.cpp:1432-1434`, `src/takaro_bridge.hpp:201-214`). Unclaimed actions leak in
the never-freed gate (`src/native.cpp:130`, `src/native.cpp:1401`). Pending `g_pending_*` work is abandoned.

Lock coupling: `BanManager::ban/unban/reconcile` hold `BanManager::mutex_` across `gate_.submit` (up to 7.5 s), so the
maint thread's timed-ban expiry and `listBans` serialize behind a slow ban (`src/takaro_adapter.hpp:191-220`,
`src/takaro_adapter.hpp:237-245`). The game thread shares `gate::Server::mutex_` with workers that copy the catalog
(≤65536 rows) under it (`src/gate_state.hpp:607-611`, `src/takaro_adapter.hpp:510`).

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation |
| --- | --- | --- | --- | --- |
| Players online | game (PostLogin/Logout hooks) | `gate players_` map | memory | Logout erases; shutdown synthesizes logouts (`src/gate_state.hpp:321-360`, `src/takaro_adapter.hpp:672-674`); process exit clears |
| Last location | game, sampled 250 ms | `positions_`, freshness <2 s; departure/death fallback | memory | stale >2 s → unavailable (`src/gate_state.hpp:557-596`) |
| Inventory | game, sampled 1 s + after grant | `inventories_`, freshness <3 s | memory | cleared on failed read / logout (`src/gate_state.hpp:598-605`) |
| Item / entity catalog | game MasterItemList / DinoEntries | `catalog_items_`, `entity_items_` | memory, built once per boot | cleared and retried 10 s on page error (`src/native.cpp:724-730`) |
| Locations | game PlayerStarts | `locations_` | memory | once per boot (`src/native.cpp:773-792`) |
| Identities | Steam64 from game; names | `known-players.json` (≤100000) | file, atomic+fsync | flushed by maint each 1 s (`src/takaro_store.hpp:230-280`, `src/takaro_bridge.hpp:689-690`) |
| Server identity | connector | `identity-token` | file, generated once | never (`src/native.cpp:1359-1371`) |
| Bans | game native ban list (sampled 2 s, fresh ≤3 s) | — | game | `src/native.cpp:794-803`, `src/gate_state.hpp:540-545` |
| Ban reason/expiry, pending intent | connector | `ban-metadata.json` (≤4096) | file, atomic+fsync; corrupt → bridge refuses start | reconcile each 1 s lifts expired bans (`src/takaro_adapter.hpp:306-342`, `src/takaro_store.hpp:162-222`, `src/native.cpp:1418-1423`) |
| Event cursor | connector | `event-cursor.json` {bootId, seq} | file, but only reused when bootId matches; bootId is fresh per process | effectively reset every restart (`src/native.cpp:1134-1144`, `src/takaro_bridge.hpp:186-187`) |
| Pending events / outbox | connector | `events_` ring 4096 | **memory only** | lost on process exit |

## 7. C6 delivery

| Aspect | Behaviour | Ref |
| --- | --- | --- |
| Outbox bound | 4096-event ring; eviction reported via `truncated` and one log line, delivery continues | `src/gate_state.hpp:623-634`, `src/takaro_bridge.hpp:506-509` |
| Ack v0 | ping carries highest send id; matching pong confirms all earlier frames (transport receipt, not processing) | `src/takaro_bridge.hpp:465-480`, `src/takaro_bridge.hpp:537-540` |
| Ack v1 | `eventAck{streamId=bootId, upTo}`; pongs confirm nothing; `eventError` only logged | `src/takaro_bridge.hpp:477`, `src/takaro_bridge.hpp:575-587` |
| Reconnect replay | on disconnect/connect `scan_ = persisted_` → at-least-once replay of unconfirmed events; v1 deduped by (streamId, seq) | `src/takaro_bridge.hpp:341`, `src/takaro_bridge.hpp:383`, `src/takaro_bridge.hpp:520-524` |
| Request results | responses bound to connection epoch; late result after reconnect dropped | `src/takaro_bridge.hpp:293-297` |
| Heartbeat | ping 5 s, 2 missed pongs or 20 s idle → reconnect, backoff 2 s·2^n ≤60 s | `src/takaro_bridge.hpp:46-50`, `src/takaro_bridge.hpp:447-459`, `src/takaro_bridge.hpp:349-352` |
| Game crash | loses ring, unsent events, in-flight actions (outcome unknown), pending gives/bans; on restart new bootId → new stream, no replay of old boot; online roster rebuilt from PostLogin only | `src/native.cpp:1397-1403`, `src/takaro_bridge.hpp:186-187` |
| Clean shutdown | world save before ACK; departures recorded and given ≤3 s to deliver; exit requested ≥200 ms after response flushed, abandoned after 5 s | `src/takaro_adapter.hpp:664-679`, `src/native.cpp:617-634` |

## 8. C8 shared vs game-specific

| Code | Sibling | Status |
| --- | --- | --- |
| `gate_state.hpp` (game-thread gate) vs shared runner `gamethread.cpp` | `games/dragonwilds/mod/src/gamethread.cpp`, `games/vein/mod/src/gamethread.cpp`, `games/conan-exiles/native/core/gamethread.cpp` | independent design (shared_ptr action + cancel flag), not a copy |
| `takaro_bridge.hpp`, `ws_client.hpp`, `takaro_store.hpp`, `json.hpp` | `games/conan-exiles/native/core/takaro/bridge.h` (ported from Enshrouded), `games/enshrouded/mod/src/native/bridge.h`, `games/scum/native/src/bridge.h` | no file of the same name outside `games/ark`; different namespaces/APIs and threading (`find games -name ws_client.hpp` etc. → none) → fully diverged/independent |
| `takaro_adapter.hpp` | ARK 0.1 sidecar adapter (removed) | port; header says so (`src/takaro_adapter.hpp:2`) |
| Game-specific | all `*_bindings.hpp`, fixed addresses in `native.cpp:48-85` | exact build only |

ARK is a fourth independent in-process Generic client (after Conan/Enshrouded lineage, VEIN/Dragonwilds `native_bridge.cpp`,
SCUM) — a candidate caller for any shared C++ protocol core, but nothing is shared today.

## 9. Existing tests for C4/C6 (all built with `-fsanitize=address,undefined` before packaging, `games/ark/scripts/build-release.sh:34-38`)

| Test | Covers | Ref |
| --- | --- | --- |
| `gate_state_test` "unclaimed action times out / cancelled before dispatch / cannot be acknowledged later" | queued cancellation | `games/ark/mod/tests/gate_state_test.cpp:175-182` |
| `gate_state_test` "stalled executing action never reports success" + late `complete` | started-job timeout, lifetime | `games/ark/mod/tests/gate_state_test.cpp:184-197` |
| `gate_state_test` ring paging/truncation | outbox bound | `games/ark/mod/tests/gate_state_test.cpp:89-99` |
| `takaro_bridge_test::test_events_cursor_and_outage_replay` | reconnect replay, cursor | `games/ark/mod/tests/takaro_bridge_test.cpp:408` |
| `takaro_bridge_test::test_game_thread_stall_withholds` | stalled game thread, reads stay served | `games/ark/mod/tests/takaro_bridge_test.cpp:593-614` |
| `takaro_bridge_test::test_shutdown_response_then_exit_flag` | departures before ACK, exit after write | `games/ark/mod/tests/takaro_bridge_test.cpp:562` |
| `takaro_bridge_test::test_protocol_v1` | eventAck-only confirmation | `games/ark/mod/tests/takaro_bridge_test.cpp:675-690` |
| `takaro_bridge_test::test_bans_and_expiry` | timed-ban expiry | `games/ark/mod/tests/takaro_bridge_test.cpp:505` |

Gaps: no overload test for the 128 action queue / 64 request queue / 1024 outbound / 2 MiB send buffer; no test of
response drop on full send buffer; no test that a timed-out started give is not duplicated by retry; no duplicate-grant
count; no test of destructor with pending actions; no tick-cost measurement (catalog paging, 128 pending-ban rescans);
no game-crash/boot-change replay test (cursor across bootId); `eventError` handling untested.

## 10. Top risks

| # | Risk | Sev | Follow-up |
| --- | --- | --- | --- |
| 1 | Started mutation times out (4.5 s) while game thread completes it; response withheld, Takaro retry can duplicate a give | high | Add idempotency key (requestId) per action so a retried request returns the recorded outcome instead of re-granting. |
| 2 | All undelivered events are memory-only and boot-scoped; any crash/restart loses them and the persisted cursor never helps across boots | med | Decide (protocol-level) whether a durable outbox keyed by stable stream id is required; otherwise drop the cursor file and document loss. |
| 3 | Game-thread budget unbounded by time: 1 action/tick but all pending gives (≤64) and pending bans (≤128, each a full ban-list snapshot every 100 ms) plus catalog pages run per tick; ban mutex held across 7.5 s submits | med | Add a per-tick time budget/counter for pending continuations and a harness measuring tick cost under a 128-ban/64-give burst. |
