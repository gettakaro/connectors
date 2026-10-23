# Dune: Awakening — source-level architecture (issue #298), source-only

HEAD 3859fd8. All paths are relative to the repo root unless they start with `mod/` or `sidecar/`, which mean
`games/dune/mod/` and `games/dune/sidecar/`. Nothing here was run against a game or Takaro.

## 1. Architecture on main

| Item | Value | Evidence |
|---|---|---|
| Placement | **Sidecar (required) + optional LD_PRELOAD plugin in the map process only** | games/dune/DEVELOPMENT.md:9-18, :66-86; catalog/dune/game.json:25-33 |
| Sidecar runtime | Node 22 + TypeScript; deps `ws`, `pg`, `amqplib` | games/dune/DEVELOPMENT.md:9; sidecar/package.json:15-19 |
| Plugin runtime | C++17 `libtakaro-dune.so`, built on bookworm/glibc 2.36, static libstdc++ | games/dune/DEVELOPMENT.md:18, :99-102 |
| Loader | `LD_PRELOAD` on `DuneSandboxServer-Linux-Shipping` only ("Never apply LD_PRELOAD to the complete battlegroup") | games/dune/README.md:103-117 |
| Game access | Sidecar: Postgres read-only + game RabbitMQ (chat, GM command bus = only write path). Plugin: hooks `ProcessEvent` slot 86 + PostLogin/OnNetCleanup, HTTP API on 127.0.0.1:18890 with Bearer token | games/dune/DEVELOPMENT.md:68-85 |
| Takaro transport | `ws` WebSocket, outbound, default `wss://connect.takaro.io/` | sidecar/src/takaro/client.ts:116; sidecar/src/takaro/settings.ts:32, :189 |
| TLS verification | Takaro WSS: `new WebSocket(url)` with no options → Node default verification ON (client.ts:116). Game RabbitMQ: **`rejectUnauthorized:false` by default** (`DUNE_RMQ_TLS_INSECURE` defaults true) | sidecar/src/dune/config.ts:248; sidecar/src/dune/rmq.ts:267-271; README.md:67 |
| Protocol | v0 + v1 (`protocolVersions:[0,1]`, eventAck) | sidecar/src/takaro/client.ts:125, :446-454 |
| Default catalog target | `linux-26051014` (`default:true`, status **candidate**) | catalog/dune/targets/linux-26051014.json:3,7,9 |
| Newest `maintained` target | `linux-25689360` | catalog/dune/targets/linux-25689360.json:3,9 |
| Connector version | 0.4.0 | games/dune/version.txt:1 |
| Runtime verification | `verification.required: contract`; runtime proof only in a maintainer test environment (needs Funcom token, 15 GB images) | games/dune/DEVELOPMENT.md:127-131; catalog/dune/targets/linux-25689360.json:21 |

**Why the sidecar is still justified (the extra-focus question):**

| Reason | Evidence |
|---|---|
| A server is a *battlegroup*: igw-postgres, game RMQ, admin RMQ, text-router, bg-director, gateway, **one UE5 map server per map** — no single process holds all truth | games/dune/DEVELOPMENT.md:61-64 |
| No RCON, no kill feed, no documented log grammar | games/dune/DEVELOPMENT.md:63-64 |
| Roster/identity/inventory/life-state are Postgres rows (`player_state ⋈ accounts ⋈ encrypted_accounts ⋈ actors`); FLS id lives only in the `accounts` view | games/dune/DEVELOPMENT.md:68-69; sidecar/src/dune/pg.ts:210-245 |
| Map process has **no FLS id** UPROPERTY; plugin can only report accountId/actor ids and the sidecar joins them to Postgres | games/dune/DEVELOPMENT.md:88-90; sidecar/src/dune/pluginJoin.ts:7-25; mod/src/query.cpp:75-77 |
| Chat in/out only via game RabbitMQ exchanges (`chat.intercept`, `chat.whispers`, `chat.map`) | games/dune/DEVELOPMENT.md:70-71; sidecar/src/dune/config.ts:249-254 |
| All mutations go over the RMQ GM command bus (amqp / docker-exec / kubectl-exec publisher), with Postgres read-back | games/dune/DEVELOPMENT.md:72-75; sidecar/src/dune/gm.ts:268, :394; sidecar/src/dune/adapter.ts:339-372 |
| Plugin is deliberately read-only ("mutations: none by design") | mod/docs/gamethread-policy.md:14-18; mod/src/query.cpp:152 |
| Map transfers between map processes need a battlegroup-level presence differ with grace (`reconnect_grace_period_end`) | sidecar/src/dune/presence.ts:55-80, :145-170, :241-247 |
| Bans do not exist in the game; connector owns them across all maps | sidecar/src/dune/bans.ts:6-12 |
| Database name changes with the server code branch (`dune_sb_1_5_15_0`) — sidecar detects frozen DB | games/dune/DEVELOPMENT.md:149; sidecar/src/dune/pg.ts:161-170 |

Conclusion: placing the connector inside one map process would lose the battlegroup-wide roster, identity, chat
and GM write path; the issue itself exempts Dune from sidecar removal and source agrees.

## 2. Drift vs the issue (DUN-1..DUN-4, issue298.md:238-247)

| Hypothesis (quoted) | Status at HEAD | Evidence |
|---|---|---|
| DUN-1 "Record the actual battlegroup topology, optional map-plugin deployment, Postgres/RabbitMQ/GM dependencies, and identity joins" | **Still true**, documented in source; plugin optional, single `DUNE_PLUGIN_URL` | games/dune/DEVELOPMENT.md:61-90; sidecar/src/dune/pluginClient.ts:40, :89 |
| DUN-1 "map transfers, grace periods, and reconnects do not create false player disconnects or mismatched identities" | **Still true** as a design; grace = server's `reconnect_grace_period_end` else 45 s; name joins refuse ambiguity | sidecar/src/dune/presence.ts:145-170, :247; sidecar/src/dune/config.ts:296; sidecar/src/dune/pluginJoin.ts:35-38 |
| DUN-2 "ProcessEvent-driven pump … do not assume a regular Tick cadence" | **Still true**; pump runs every 64th ProcessEvent call, stall watchdog (18 s gap measured) | mod/src/livehooks.cpp:51-57; mod/src/gamethread.cpp:37-47; mod/src/gamethread.h:18-28 |
| (old) gamethread-policy.md says jobs are "drained from the Tick hook" | **Doc drift** (unchanged since 147acb2): code no longer reaches Tick; policy doc still says Tick | mod/docs/gamethread-policy.md:31-32, :50 vs mod/src/gamethread.cpp:140-148 |
| DUN-2 "do not … move unsafe live reads onto a worker" | **Partly violated**: housekeeping thread reads live UObjects (lifeState re-read, presence sweep over GUObjectArray) | mod/src/events.cpp:891-895; mod/src/presence.cpp:51-85 |
| DUN-3 "GM mutation read-back, connector-owned bans/expiry, stale-chat policy, queued events, … sidecar recovery" | **All present**: read-back polls, bans.json + expiry sweep, chat max age 600 s, 5000-event queue, self-exit watchdog | sidecar/src/dune/adapter.ts:743-746; sidecar/src/dune/bans.ts:102-107; sidecar/src/bridge.ts:24, :32; sidecar/src/__tests__/outage-resilience.test.ts:139 |
| DUN-3 "Keep database access read-only" | **Still true** | games/dune/DEVELOPMENT.md:68; mod/docs/gamethread-policy.md:14 |
| DUN-4 "persistence/indexing … without duplicating battlegroup truth in each map process" | **Still true**: persistent state only in sidecar `/data` (cursor, online set, bans, known players, identity) | sidecar/src/dune/config.ts:179, :307-310 |
| Changed since 147acb2 | Protocol v1 + eventAck added; DB-branch detection; death coalescer; `.env` hot reload; version 0.2.0 → 0.4.0 | `git log 147acb2..HEAD -- games/dune` (#481, #563, #565, #438); sidecar/src/dune/deathCoalescer.ts:59 |

## 3. C1 install (ordinary renter)

| Step | Evidence |
|---|---|
| Self-hosted battlegroup + Funcom self-hosting token (no anonymous boot) | README.md:10; catalog/dune/targets/linux-25689360.json:21 |
| Add 2 launch args to every map server (`NotificationSystem.Enabled`, `ServerCommandsAuthToken`) | README.md:28-33 |
| Edit `UserGame.ini` GM allow-list (KickPlayer, ServiceBroadcast); restart map | README.md:35-42 |
| Unpack sidecar, `.env` with Takaro token, `DUNE_PG_URL` (build-specific DB name), `DUNE_RMQ_URL` (AMQPS), GM token, publisher | README.md:46-69 |
| GM publisher needs either RMQ `internal` auth backend + `fls` user, **Docker socket**, or **kubectl** into k3s | README.md:71-75 |
| `docker compose up -d --build` (or Node 22 `npm ci && npm run catalogue`); writable `/data` volume | README.md:79-97, :151 |
| Outbound WSS to Takaro | sidecar/src/takaro/settings.ts:32 |
| Optional: `LD_PRELOAD` + `TAKARO_PLUGIN_TOKEN` on the map process; sidecar must share its network namespace | README.md:103-117 |

Classification: **extra process (sidecar)** + optional **arbitrary native library via LD_PRELOAD**. Host control
required: network reach to Postgres and the game broker, ability to change map launch args/env and config files,
and for two of three publishers Docker-socket or kubectl access. Not achievable on a typical rented panel; this is
an operator-of-the-battlegroup install by design.

## 4. C3 thread trace (source only)

| Path | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| **getPlayers** (sidecar) | Postgres `onlinePlayers()` query on Node event loop (async pg pool, max 4) — **no game thread** | Node event loop | `ws` on event loop | none | event loop | 
| ↳ evidence | sidecar/src/dune/adapter.ts:186-189; sidecar/src/dune/pg.ts:326; sidecar/src/dune/config.ts:244 | | sidecar/src/bridge.ts:319-347 | | |
| getPlayerLocation (plugin path) | Plugin HTTP thread → `Players::EnsureFresh` queues `Refresh()` onto game thread (TTL 500 ms, 5 s wait); `Query::Players` JSON on HTTP thread | HTTP conn thread (one detached pthread per connection) | plugin HTTP thread → sidecar `fetch` (10 s timeout) | none | PluginLog |
| ↳ evidence | mod/src/players.cpp:528-540; mod/src/query.cpp:69-140 | mod/src/query.cpp:72-95 | mod/src/http.cpp:557-569; sidecar/src/dune/pluginClient.ts:91; sidecar/src/dune/config.ts:289 | | |
| **giveItem** | Sidecar publishes GM `addItemToInventory` on RMQ (or spawns docker/kubectl exec, 20 s timeout); the **map server's own game thread** consumes it (outside our code); sidecar polls Postgres for read-back (window 8 s, step 500 ms) | event loop | amqplib channel / child process | none | event loop |
| ↳ evidence | sidecar/src/dune/adapter.ts:339-372, :743-746; sidecar/src/dune/gm.ts:268-275, :346-354, :394-419; sidecar/src/dune/config.ts:285, :297-299 | | | | |
| **chat-message** | Game publishes to `chat.intercept`; sidecar consumer (prefetch 16) handles synchronously then `ack` | event loop | `bridge.emit` → `ws` | none (outbox in memory) | event loop |
| ↳ evidence | sidecar/src/dune/rmq.ts:276, :293-303; sidecar/src/bridge.ts:205-233 | | | | |
| **player-connected** | Primary: Postgres presence poll every 5 s (event loop). Plugin hint: PostLogin detour / presence sweep → `GameThread::Run(NoteLogin)` → `EmitEvent` into plugin ring; sidecar polls `/events` every 1 s | plugin: housekeeping thread; sidecar: event loop | sidecar `ws` | `online-players.json` after every presence tick (sync fs) | both |
| ↳ evidence | sidecar/src/dune/presence.ts:91, :116-170; mod/src/events.cpp:1684-1700, :1562; sidecar/src/dune/eventPoller.ts:73; sidecar/src/__tests__/presence.test.ts:155 | | | sidecar/src/dune/config.ts:308 | |
| player-death (plugin) | ProcessEvent detour copies primitives + raw pointers into 512-slot SPSC hit ring; housekeeping thread (every 2 s) decodes, dedupes, builds JSON, `EmitEvent` | housekeeping thread | sidecar | `event-cursor.json` (sync, tmp+rename) | housekeeping |
| ↳ evidence | mod/src/events.cpp:343-399, :1485-1491, :865-1001, :815; mod/src/main.cpp:137-147 | | | sidecar/src/dune/cursorStore.ts:24-29 | |

Workers holding live game pointers:
- Player registry entries store raw `controller`/`playerState`/`pawn` pointers, read under `g_lock` by HTTP threads (copied in `Find`) — mod/src/players.h:56-58; mod/src/players.cpp:493-516.
- Hit ring carries `self/victim/killer/causer/weapon/victimPlayerState` pointers to the housekeeping thread, which **dereferences** `victimPlayerState` off-thread (guarded by `MemReadable` + `LooksLikeUObject`, still racy vs GC) — mod/src/events.cpp:345-350, :394, :891-895.
- Presence reconciler walks GUObjectArray and reads properties on the housekeeping thread every 2 s ("pure guarded reads") — mod/src/presence.cpp:51-85, :33-48.
- `KillNearest` (debug) captures an off-thread-chosen `actor` and re-validates on the game thread — mod/src/events.cpp:2017-2023.

## 5. C4 queues

| Queue | Capacity / admission | Drain | Timeout behaviour | Shutdown | Evidence |
|---|---|---|---|---|---|
| Plugin game-thread job deque | reject when `size() > 256` (i.e. 257 admitted; Dragonwilds/VEIN use `>= 256`) | from ProcessEvent every 64th call; ≤16 jobs, budget `TAKARO_TICK_BUDGET_US`=500 µs (clamped 50..33000), ≥1 job always | caller waits ≤ timeoutMs (5 s default, 20 s boot validate); if not started → cancelled; **if started, job keeps running after caller returned** | none (detached; library never unloads) | mod/src/gamethread.cpp:51-63, :99-127, :289-317; mod/src/gamethread.h:13, :30-32; mod/src/livehooks.cpp:51-57 |
| ↳ lifetime hazard | `RunJson` lambda captures `result` **by reference** (`[&]`) on the caller's stack; `http.cpp:260` (`ranOn`), `http.cpp:305/326`, `events.cpp:2021` (`err`, `called`, `deathFlagOff`) likewise. A started job that outlives a timed-out caller writes into a dead stack frame. VEIN/Dragonwilds use `*result` (heap pointer form) | | | | mod/src/gamethread.cpp:319-329; mod/src/http.cpp:260, :305, :326; mod/src/events.cpp:2021; vein/mod/src/gamethread.cpp:293 |
| Hit ring (game thread → housekeeping) | 512 SPSC; full → `g_ringDropped++` (event lost) | housekeeping every 2 s, unbounded per pass | n/a | n/a | mod/src/events.cpp:400-404, :1485-1491, :865-867; mod/src/main.cpp:137 |
| Plugin event ring (`/events`) | deque `kMaxEvents=5000`, oldest evicted; `truncated` flag | sidecar polls every 1 s from cursor | n/a | lost on map restart (new bootId) | mod/src/state.h:31; mod/src/state.cpp:46-73; sidecar/src/dune/eventPoller.ts:73, :87-98 |
| Plugin HTTP | `listen(s, 32)`; **one detached pthread per accepted connection, no cap** | — | — | — | mod/src/http.cpp:550, :557-569 |
| Sidecar outbox `pendingEvents` | `MAX_PENDING_EVENTS=5000`; overflow drops oldest `log` first, else oldest event; counted | drained on next emit / identify / 1 s retry timer | chat older than 600 s dropped at reconnect | in-memory only; lost on sidecar exit | sidecar/src/bridge.ts:24, :32, :389-412, :437-470 |
| Sidecar `unconfirmedEvents` | unbounded array of written-but-unacked; re-queued at front on disconnect then trimmed to 5000 | released on pong/eventAck | — | lost on exit | sidecar/src/bridge.ts:113-120, :353-385 |
| WS send | refuses frames when `bufferedAmount > 1 MiB`; ping 5 s, 2 missed pongs or 20 s idle → terminate; backoff 2 s..60 s | | | | sidecar/src/takaro/client.ts:84-89, :223-231, :244-262, :195-196 |
| Takaro request handling | **no concurrency limit**; each request `await`s adapter; GM exec publisher spawns a process per call (20 s timeout) | | Takaro-side request timeout; teleport read-back capped to it | | sidecar/src/bridge.ts:319-347; sidecar/src/dune/gm.ts:346-354; sidecar/src/dune/adapter.ts:455-461 |
| RMQ chat consumer | durable named queue, prefetch 16, ack after handling (before Takaro delivery) | | | | sidecar/src/dune/rmq.ts:276, :286-303 |
| Postgres | pool max 4, statement timeout 5 s | | | | sidecar/src/dune/config.ts:243-244 |

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation on restart/world change | Evidence |
|---|---|---|---|---|---|
| Players online | Postgres `player_state` (three-state presence) | sidecar presence differ + `online-players.json` | file, after every tick | restart reconciles vs persisted set | sidecar/src/dune/presence.ts:116-170; sidecar/src/dune/config.ts:308; sidecar/src/__tests__/presence.test.ts:96 |
| Plugin-observed players | plugin PostLogin registry (hint only) | memory, max 128, linger 60 s | none | map restart clears | mod/src/players.cpp:17-20; mod/src/query.cpp:75-77 |
| Last location | plugin live pawn (snapshot TTL 500 ms) → chat `m_OriginLocation` hint → Postgres last-saved transform | plugin memory; sidecar memory (`lastPositions`) | none | — | sidecar/src/dune/adapter.ts:209-276; mod/src/query.cpp:28-37 |
| Inventory | Postgres `inventories ⋈ items` (game persists periodically) | none (read per request) | — | — | sidecar/src/dune/adapter.ts:286-297; sidecar/src/dune/pg.ts:352 |
| Item catalog | community wiki generated at build/entrypoint (`items.json`) | file | file | rebuilt per image | games/dune/DEVELOPMENT.md:133-142; sidecar/src/dune/config.ts:311 |
| Identities | Postgres `accounts` view (FLS id); plugin accountId joined | `known-players.json`; `takaro-identity.json` (server identity) | file | known store survives | sidecar/src/dune/pg.ts:210-245; sidecar/src/dune/config.ts:310; games/dune/DEVELOPMENT.md:49-52 |
| Bans / timed bans | **connector** (game has none) | `bans.json`, kick-on-sight sweep ≥5 s, connector expiry | file, reloaded when edited | survives restart | sidecar/src/dune/bans.ts:6-12, :102-107; sidecar/src/index.ts:147, :323 |
| Event cursor | plugin `seq`+`bootId` | `event-cursor.json` (tmp+rename) | file | reset to 0 on new bootId or seq going back | sidecar/src/dune/cursorStore.ts:10-29; sidecar/src/dune/eventPoller.ts:87-98 |
| Pending events / outbox | sidecar | memory (5000) | **none** | lost on sidecar exit (plugin events replay via cursor; chat/presence-derived do not) | sidecar/src/bridge.ts:111-120 |
| DB branch | image's code branch | detected at start | — | warns on stale DB name | sidecar/src/dune/pg.ts:161-170; games/dune/DEVELOPMENT.md:149 |

## 7. C6 delivery

| Aspect | Behaviour | Evidence |
|---|---|---|
| Outbox bound | 5000, log lines evicted first, drops counted | sidecar/src/bridge.ts:24, :396-412 |
| Ack semantics | v0: pong for a ping written after the event confirms up to that sendId (FIFO fallback if payload not echoed); v1: `eventAck {streamId, upTo}` (pongs ignored for confirmation) | sidecar/src/takaro/client.ts:36-42, :168-184, :446-454 |
| Reconnect replay | unconfirmed events re-queued at front, re-sent after identify, in order; may duplicate within one heartbeat | sidecar/src/bridge.ts:375-385; sidecar/src/__tests__/delivery-confirmation.test.ts:51 |
| Stream identity | `streamId = randomUUID()` per process, so a sidecar restart starts a new v1 stream | sidecar/src/takaro/client.ts:70-74 |
| Plugin event replay | persisted cursor never passes a queued hole; restart replays from it | sidecar/src/dune/eventPoller.ts:106-133; sidecar/src/__tests__/plugin-integration.test.ts:331 |
| Outage presence | flaps collapse into one reconciliation | sidecar/src/__tests__/outage-resilience.test.ts:342 |
| Sidecar crash | in-memory outbox lost; RMQ chat already acked → **chat lost**; presence/death re-derived from Postgres edge (a death that happened while down is not re-reported: "corpse on first poll") | sidecar/src/dune/rmq.ts:296-303; sidecar/src/__tests__/presence.test.ts:148 |
| Map crash | plugin rings (hit ring, 5000-event ring, registry) lost; new bootId resets cursor; Postgres stays authoritative | mod/src/state.h:31-38; sidecar/src/dune/eventPoller.ts:87-98 |
| Plugin ring overflow | `truncated:true` is returned but the sidecar poller **does not read it** (silent gap) | mod/src/state.cpp:71-73; sidecar/src/dune/eventPoller.ts:83-133 (no `truncated`) |
| Shutdown | SIGTERM: stops sources, flushes death coalescer, closes ws/rmq/pg, exits after 100 ms; pending outbox not persisted | sidecar/src/index.ts:412-428 |

## 8. C8 shared vs game-specific

| Dune file | Sibling(s) | State |
|---|---|---|
| mod/src/perf.cpp | vein/mod/src/perf.cpp, dragonwilds/mod/src/perf.cpp | **identical** (diff -q) |
| mod/src/hooks.cpp | vein (21 changed lines), dragonwilds (38) | near copy, diverged slightly |
| mod/src/gamethread.cpp | vein (77), dragonwilds (101), conan-exiles/native/core (411) | copied, **diverged**: Dune uses `> 256` and by-ref `RunJson`; VEIN/DW use `>= 256` and `*result` | 
| mod/src/state.cpp, common.cpp/h, http.cpp, reflect.cpp, main.cpp, events*.cpp, resolve.cpp | vein, dragonwilds, enshrouded, conan-exiles/native | same lineage, heavily diverged (170–4600 diff lines) |
| sidecar/src/takaro/client.ts | vein/sidecar (230), enshrouded/sidecar (392), terraria/bridge (597) | same lineage, diverged |
| sidecar/src/takaro/protocol.ts | vein (51), enshrouded (58), terraria/bridge (174) | diverged |
| sidecar/src/logger.ts, healthServer.ts | vein, enshrouded (whole-file diffs) | diverged |
| sidecar/src/takaro/settings.ts, eventWhitelist.ts | no sibling with that path | Dune-only |

Game-specific (must stay): Postgres roster SQL/DB-branch detection, RMQ chat/GM envelope, presence grace, connector-owned
bans, plugin↔Postgres identity join, RTTI-anchored resolver for the stripped ELF (games/dune/DEVELOPMENT.md:79-84).
Candidates to share: `perf.cpp` (already identical), the native `gamethread` job queue (fix once, three callers), the
sidecar outbox/confirm logic (`bridge.ts` + `client.ts` confirmation) once VEIN/Enshrouded leftovers are retired.

## 9. Existing tests (C4/C6) and gaps

| Test | Covers |
|---|---|
| sidecar/src/__tests__/delivery-confirmation.test.ts:51, :76, :89, :104, :113, :169, :193 | resend unconfirmed once, no resend after confirm, outage queue order, bound + counted drop, partial flush, pong semantics, no send before identify |
| sidecar/src/__tests__/congestion.test.ts:42, :54, :63, :75, :86 | congested-socket drain order, idle retry, log-first eviction, cursor not past queued hole |
| sidecar/src/__tests__/outage-resilience.test.ts:55, :86, :117, :139, :182, :230, :300, :342, :400, :411 | sources run during outage, plugin poller keeps draining, no replay after delivery, watchdog only on game-side loss, ban enforcement + expiry offline, stale-chat drop, presence collapse, bounded queue |
| sidecar/src/__tests__/presence.test.ts:60, :80, :96, :148, :167 | map-transfer grace, restart reconciliation, roster failure |
| sidecar/src/__tests__/plugin-integration.test.ts:331 | cursor across sidecar restart and plugin bootId change |
| sidecar/src/__tests__/protocol-v1.test.ts | v1 eventAck path |
| sidecar/src/__tests__/chat-wire.test.ts:112 | broker refusal retry |
| mod/tests/unit_test.cpp:190-221 | plugin event ring overflow sets `truncated` |

Gaps: no test of `GameThread::Run` timeout/started-job lifetime, queue-full rejection or pump stall (mod/tests has
no gamethread case); no hit-ring overflow test; no test that the sidecar reacts to `truncated`; no test of chat
loss on sidecar crash after ack; no bound/test on concurrent Takaro requests or exec-publisher fan-out; no thread
assertion that housekeeping reads of live UObjects are safe; runtime levels never run in CI (`verification.required: contract`).

## 10. Top risks

| # | Severity | Risk | Follow-up |
|---|---|---|---|
| 1 | **high** | Plugin `GameThread::RunJson`/`Run([&]…)` jobs capture caller stack by reference; a job that started before the 5 s timeout keeps running on the game thread and writes into a returned stack frame (mod/src/gamethread.cpp:304, :319-329; mod/src/http.cpp:260, :305, :326; mod/src/events.cpp:2021). Probability rises with ProcessEvent pump stalls (18 s measured, gamethread.cpp:37-40). | Port VEIN/Dragonwilds' heap-owned `*result` form and require by-value captures; add a unit test with a fake pump that starts a job then times out. |
| 2 | med | Off-game-thread reads of live UObjects (presence sweep over GUObjectArray every 2 s, lifeState re-read in drain) rely on `LooksLikeUObject` heuristics and can race GC (mod/src/presence.cpp:51-85; mod/src/events.cpp:891-895). | Move both into a budgeted game-thread job or capture the value on the hook path; measure with `/debug/perf`. |
| 3 | med | Silent loss paths: RMQ chat acked before Takaro delivery and outbox only in memory (sidecar crash loses chat); plugin `/events` `truncated` ignored; hit-ring overflow only counted (sidecar/src/dune/rmq.ts:296-303; sidecar/src/bridge.ts:111; sidecar/src/dune/eventPoller.ts:83-133; mod/src/events.cpp:1486-1487). Also only one `DUNE_PLUGIN_URL`, so multi-map battlegroups get plugin data from one map only (sidecar/src/dune/pluginClient.ts:40). | Surface `truncated`/ringDropped in sidecar health; decide (and document) whether chat needs ack-after-confirm or a small durable outbox; document/support one plugin URL per map. |

Low-severity notes: gamethread-policy.md still describes a Tick-driven pump (mod/docs/gamethread-policy.md:31-32, :50);
RMQ TLS verification is off by default (sidecar/src/dune/config.ts:248); uncapped per-connection pthreads in the
loopback plugin HTTP server (mod/src/http.cpp:557-569).
