# Rust (Facepunch) — issue #298 architecture analysis

**source-only.** Nothing was built, run, or tested live. Every reference is `path:line` at HEAD `3859fd8`.
`CS` = `games/rust/mod/TakaroConnector.cs` (the whole connector is this one 2202-line file, DEVELOPMENT.md:17).
`OLD` = the same file at `147acb21b924d807f029dbc38a7e7ebd3b08a2f0`.
Anything the Carbon/Oxide framework does (NextTick drain, hook thread, timer teardown) is not in this repo and is marked *framework-owned, unverified*.

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | In-process plugin. A single `.cs` file that the framework compiles at load. No sidecar and no native code | CS:15-19, README.md:46, DEVELOPMENT.md:12-17 |
| Runtime | RustDedicated Unity/Mono. The compile check targets `netstandard2.1` | games/rust/compile/TakaroConnector.Compile.csproj:10 |
| Loader | Written against the Oxide API (`Oxide.Plugins`, `RustPlugin`, `Interface.Oxide.*`). Only Carbon is pinned and booted | CS:12,15,19; catalog/rust/game.json:6; DEVELOPMENT.md:296 |
| Carbon boot | Doorstop `LD_PRELOAD` via `carbon/tools/environment.sh` | games/rust/start.sh:23-30; DEVELOPMENT.md:113,149 |
| Transport | `System.Net.WebSockets.ClientWebSocket` | CS:6,450,761 |
| Protocol | v0. Identify sends only `identityToken`/`registrationToken`, with no `protocolVersions`. There are no event ids and no acks | CS:1010-1018, CS:1070-1078, CS:206 ("protocol v0") |
| TLS verification | Platform default. No `ServerCertificateValidationCallback` or `ServicePointManager` override exists (grep finds none). How Mono validates the certificate store at runtime is unverified | CS:761-768 |
| Keepalive / heartbeat | None configured: no `KeepAliveInterval`, no app ping/pong, no receive deadline. A `ping` frame falls into `default:` and logs "Unknown message type" | CS:761, CS:913-941 |
| Default catalog target | `carbon-25902425`, `default:true`, **support.status `candidate`**. No target is `maintained`: the other 7 are `retired` | catalog/rust/targets/carbon-25902425.json:3,7,9 |
| Connector version | `0.3.0` | games/rust/version.txt:1, CS:17 |

## 2. Drift against the issue's hypotheses

| Id | Issue text (quoted) | Status | Evidence |
|---|---|---|---|
| RUS-1 | "Record maintained Carbon/Oxide support separately … Do not infer one loader's proof covers the other." | **Still true, and now sharper.** The catalog knows only `carbon` (game.json:6). All 8 targets are `carbon-*`, none is maintained, and the default is `candidate`. Oxide is ⚠️ and never booted. The source has no loader-specific branches (only `Interface.Oxide.*`, CS:503,1585,1957). The compile relies on members that **Carbon publicizes at load** (csproj:21); whether Oxide exposes the same members is unverified | game.json:6; targets/*.json; README.md:179; DEVELOPMENT.md:296 |
| RUS-2 | "Profile frequent player/inventory reads, static catalog traversal, hook JSON construction, and main-thread response creation …" | **Still true, and the main-thread surface has grown.** `listItems` and `listEntities` re-traverse on every call with no cache (CS:1312-1353). Response JSON is serialized on the main thread (CS:1063, 982). New: map-tile render + PNG encode + base64 run on the main thread (CS:1672-1679), and the first map render can pause the server 3.6-12 s (README.md:185). Workers hold no live game objects (see §4) | CS:1161-1180 |
| RUS-3 | "Block outbound progress and generate a controlled event burst. Measure tasks waiting behind the socket lock …" | **Still true, and the code is unchanged.** The send path at HEAD is identical to OLD: `Task.Run` per message, then `lock(_sendLock)`, then a sync-over-async `SendAsync().GetResult()` | CS:975-1002 vs OLD:240-265 |
| RUS-4 | "Verify last-location cache freshness/eviction … Confirm game-owned ban persistence …" | **Changed.** The last-location cache is unchanged (CS:467 vs OLD:39). Bans are no longer purely game-owned. In OLD every ban went to `ServerUsers` (OLD:938-957). At HEAD, **timed bans live in a connector-owned data file** and are enforced by a `CanUserLogin` hook. Only permanent bans still use `ServerUsers` | CS:471-478, 1832-1854, 1952-1989, 2023-2035 |

## 3. C1 install (renter)

| Step | Detail | Ref |
|---|---|---|
| Prerequisite | Carbon or Oxide must already be installed. Carbon on Linux needs Doorstop `LD_PRELOAD` (a host-level launch environment change). Hosts that offer Carbon usually provide this, but that is unverified | README.md:19-22; start.sh:23-26 |
| File | Copy `TakaroConnector.cs` to `carbon/plugins/` or `oxide/plugins/`. The name matters | README.md:50-59 |
| Restart | None. The framework hot-compiles the file | README.md:66, 132-133 |
| Config | `carbon/configs/TakaroConnector.json` or `oxide/config/…` is auto-created. Paste the token; the file is re-read every 5 s | README.md:66-81; CS:503,557-606,641-689 |
| Env (optional) | `TAKARO_*` env vars override the config file but are not required. **`connector.json:6` still lists "Ability to set environment variables" as a requirement, which is stale** | README.md:99-101; games/rust/connector.json:6 |
| Writable state | Config JSON; `<data>/TakaroConnector_bans.json`; `<data>/TakaroConnector_map_*.png` | CS:475,1554,1585-1609 |
| Network | Outbound WSS to `wss://connect.takaro.io/` only. No inbound port | CS:27; DEVELOPMENT.md:10 |
| Update / uninstall | Replace the `.cs` file and the framework reloads it, keeping the config. Uninstall = delete the file. Timed bans then stop being enforced (see §6) | README.md:130-135 |
| **Classification** | **Script plugin (C# source compiled by the managed framework)**. Not an arbitrary native library, JVM agent or extra process. The only native piece is Carbon's own Doorstop, which belongs to the loader, not the connector | — |
| Host control needed | FTP or file-manager access to the plugins and configs directories, plus a host that offers Carbon or Oxide. Verdict: **documented but untested** for real rentals (source-only) | — |

## 4. C3 thread trace (source-only)

The main thread means framework hooks, `timer` callbacks and `NextTick` (*framework-owned, unverified*). The socket thread means the thread-pool continuation of `Task.Run`/`ReceiveAsync` (CS:764, 814).

| Stage | getPlayers (read) | giveItem (mutation) | player-connected (event) |
|---|---|---|---|
| Frame receive + `JObject.Parse` + dispatch | socket thread (CS:814-840, 910, 1088-1150) | socket thread (same) | n/a |
| Hand-off | `NextTick`: generation check, then execute (CS:1159-1169) | same | hook on the main thread (CS:2046) |
| Game access | main: `BasePlayer.activePlayerList`, `net.connection`, `GetAveragePing` (CS:1244-1250, 1229-1235) | main: `FindByID`/`FindSleeping`, `ItemManager.Create`, `GiveItem`/`Drop` (CS:1776-1793) | main: `transform.position`, `PlayerToJson` (CS:2050-2054) |
| JSON build | main (CS:1246-1248) | main (`new JObject()` CS:1196) | main (CS:2052-2055, 1070-1078) |
| Serialize + UTF-8 bytes | main: `msg.ToString` CS:1063, `GetBytes` CS:982 | main (same) | main: `ToString` CS:1083, `GetBytes` CS:982 |
| Network send | thread-pool task under `lock(_sendLock)` (CS:983-1001) | same | same |
| Disk write | none | none | none (bans write on main thread: CS:1836,1845,1853,1868,1983. Config re-read every 5 s on main: CS:643-645,665) |
| Logging | main for the Debug line (CS:980,1061). The send-failure warning logs from the thread pool (CS:998) | same | same |
| Live objects on workers? | **No.** The send task captures only `byte[]` plus `ws` (CS:982-994). The socket task captures `settings` and `ws` (CS:756-776) | No | No |

Other main-thread costs: `listEntities` runs `FindPrefab` + 2×`GetComponent` per manifest entity on every call (CS:1332-1339). `listItems` walks the full `ItemManager.itemList` (CS:1315). `getMapTile` renders, encodes PNG and base64 on a cache miss (CS:1672-1679). The first `EnsureMapRaster` call does a full `MapImageRenderer.Render` plus `GetPixels32` (CS:1597, 1626). `testReachability` is answered directly on the socket thread (CS:1128-1130).

## 5. C4 queues

| Stage | Capacity | Admission | Drain / budget | Timeout | Shutdown / unload | Ref |
|---|---|---|---|---|---|---|
| Inbound receive buffer | 8192-byte chunks appended to a fresh `StringBuilder` until `EndOfMessage`. **No max message size**, so memory is unbounded per frame. UTF-8 is decoded **per chunk**, so a multibyte char split across chunks is corrupted | always | one frame at a time per socket | none | `token` cancel (CS:530) | CS:805-816 |
| socket → main (`NextTick`) | **unbounded** (framework queue). There is no plugin-side counter | none. Every request except `testReachability`/unknown is enqueued | framework drains it (believed to drain everything each frame; unverified). No count or time budget in the plugin | **none.** No deadline. A request Takaro has already timed out still runs and still mutates. Only `IsCurrent(generation)` drops jobs from a replaced connection (CS:1165) | Unload bumps the generation (CS:526), so queued jobs become no-ops. A job that has started runs to completion synchronously on the main thread | CS:1159-1181 |
| Captured data | `JObject args` is owned by the closure. No game objects are captured before execution | — | — | — | — | CS:1159 |
| Outbound send | **unbounded fire-and-forget**: one `Task.Run` per message (events, responses, identify). No count or byte cap and no outbox | `ws.State == Open` checked on the caller thread only (CS:977). Events also require `_connected` (CS:1068) | `lock(_sendLock)` is a **Monitor** (CS:460, 985), held across a **blocking** `SendAsync(...).GetAwaiter().GetResult()` (CS:989-994), not an `await`. Each waiting task occupies a thread-pool thread blocked in `Monitor.Enter` | **no send timeout.** The only exits are `_cts` cancel (Reconnect/StartConnection/Unload) or a socket error. The token is read **at send time** as `_cts?.Token` (CS:993), so it can be a newer connection's token | Unload cancels `_cts` and disposes `ws` (CS:530-531). Pending tasks then throw, are caught, and log one `Send failed` each (CS:996-998). Their closures keep the old plugin instance alive until they finish | CS:968-1002 |
| Ordering | **Not guaranteed even when nothing is blocked**: separate thread-pool tasks plus a non-FIFO Monitor. With a blocked send, N events → N blocked pool threads (pool injection is slow) → receive continuations can starve too | — | — | — | — | CS:983-985 |
| Reconnect request | one slot (`Interlocked.Exchange`), started by the 1 s main-thread tick | latest wins | 1 per tick | backoff 5 s ×1.5, capped at 300 s | Unload clears it (CS:527) | CS:440-442, 641-658, 947-964 |
| Timers | `Tick` 1 s; ban sweep 60 s | — | — | — | destroyed in Unload (CS:528,532). Map `timer.Once` / shutdown `timer.Once` rely on framework teardown | CS:501,505,1566,2041 |

Constants: `InitialReconnectDelay=5000`, `MaxReconnectDelay=300000`, `BackoffMultiplier=1.5` (CS:440-442), `ConfigCheckEverySeconds=5` (CS:427), `BanSweepInterval=60f` (CS:476), `MapTileCacheSize=1024` (CS:1555), receive buffer `8192` (CS:805).

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Freshness / invalidation | Ref |
|---|---|---|---|---|---|
| Players online | game (`BasePlayer.activePlayerList`) | none; read live on main | — | live | CS:1247 |
| Single player | game (`FindByID` ?? `FindSleeping`). **Sleepers count as found** | none | — | live | CS:1224 |
| Last location | game when online or sleeping, else `_lastPosition` | `Dictionary<string,Vector3>`, main thread only | memory | written on connect, disconnect and getPlayerLocation (CS:1260,2050,2063). **No timestamp, no eviction** (grows per distinct player until `Unload` clears it, CS:534). A stale value is returned as if live. An unknown player returns `{0,0,0}` instead of null (CS:1279), so missing data looks like the origin | CS:467,1252-1280 |
| Inventory | game (main/belt/wear containers) | none | — | live; `quality` is always `""` | CS:1282-1310 |
| Item catalog | game `ItemManager.itemList` | none (rebuilt per call) | — | immutable per boot; caching is possible | CS:1312-1325 |
| Entity catalog | game `GameManifest` + prefabs | none (rebuilt per call) | — | immutable per boot | CS:1327-1353 |
| Locations | game `TerrainMeta.Path.Monuments` | none | — | per world | CS:1693-1716 |
| World map | game render | `MapRaster` (immutable) + tile LRU-ish FIFO of 1024 base64 strings | PNG at `<data>/TakaroConnector_map_<size>_<seed>_<hash>.png` | keyed on size, seed, `MapFileName`, `Checksum`, so a wipe or new seed re-renders; old PNGs are deleted | CS:413-423,1555-1615,1686-1689 |
| Identity (server) | connector | config `IdentityToken` (a UUID) | config file, atomic `File.Replace` | survives upgrade and wipe | CS:575,610-637 |
| Identity (player) | SteamID64 (`gameId`=`steamId`, `platformId=steam:<id>`); NPCs filtered | — | — | stable across wipes | CS:871-897 |
| Permanent bans | **game** `ServerUsers` | none | `ServerUsers.Save()` called on main thread (game-owned file) | game-owned | CS:1852-1853,1867-1868,1904 |
| Timed bans | **connector** `_timedBans` | `Dictionary<ulong,TimedBan>` | `DataFileSystem.WriteObject("TakaroConnector_bans")`, whole file rewritten on main thread on every change or prune | swept every 60 s and on login. **Enforced only while the plugin is loaded** (`CanUserLogin`, CS:2023). An unparseable expiry is dropped (unbanned) (CS:2008-2009). Removes any native ban for the id (CS:1835) | CS:471-501,1832-1850,1952-2035 |
| Event cursor | none (protocol v0) | — | — | — | CS:1066-1084 |
| Pending events / outbox | **none**; only in-flight `Task.Run` closures | memory | no | lost on disconnect, unload or crash | CS:1068,983 |

## 7. C6 delivery

| Aspect | Behaviour | Ref |
|---|---|---|
| Outbox bound | No outbox. In-flight send tasks are unbounded (§5) | CS:983 |
| While disconnected / before identify | Event **dropped silently** (`if (!_connected) return`). `_connected` turns true only on `identifyResponse` | CS:1068, 1036 |
| Socket closing | Not `Open`, so dropped without a log line | CS:977 |
| Ack semantics | None. v0 has no event id or `eventAck`. A completed local `SendAsync` is the only "success" and is not remote receipt | CS:1070-1078, 989-994 |
| Half-open detection | None (no ping deadline or receive timeout). A stuck TCP write blocks the lock until the OS gives up or a config change cancels it | CS:761-776, 985-994 |
| Reconnect | Backoff 5 s→300 s on the main-thread tick. Auth close codes 1008/4001/4003 stop retries for that generation | CS:826-830, 947-964, 641-658 |
| Replay | none. Requests from a replaced connection are dropped and Takaro is expected to re-send them (CS:1163-1168). Mutations are not replayed | CS:1159-1169 |
| Game crash | All in-flight events lost. In-memory `_lastPosition` lost. Timed bans survive in the data file, and permanent bans survive in `ServerUsers`. A timed ban written immediately before a crash is safe only if `WriteObject` completed (it is not atomic in-repo; framework-owned) | CS:1983 |
| Clean shutdown | `shutdown` replies first, then `quit` 2 s later (`timer.Once`). The reply runs on a thread-pool task, so it is not guaranteed to finish within 2 s | CS:2037-2042 |

## 8. C8 shared vs game-specific

| Piece | Copy of? | Diverged? | Ref |
|---|---|---|---|
| WebSocket client / send / receive | **Not a copy.** Rust is the only C# connector with this pattern. Valheim also uses `ClientWebSocket` but with an async `SemaphoreSlim sendLock` + `SendTimeout 10s` + a `PendingEventQueue(capacity: 1000)` + heartbeat/pong timeout + v1 `eventAck`. 7D2D uses WebSocketSharp, a `lock(_sendLock)` with a single sender over `BlockingCollection` + `OutboundLedger`, and `MAX_PENDING_MESSAGES = 500` | Rust is the oldest design and lacks every bound the siblings have | valheim/mod/src/Takaro.Valheim.Plugin/TakaroWebSocketRunner.cs:27,31-36,735-756; 7d2d/mod/src/WebSocket/WebSocketTransport.cs:7,93,99,104,140 |
| Receive framing | Rust loops until `EndOfMessage` (8 KB chunks). **Valheim does not loop** (single 32 KB `ReceiveAsync`, no `EndOfMessage` check), so the two are behaviourally different | yes | CS:812-816 vs TakaroWebSocketRunner.cs:589-597 |
| Config banner (`***` rule) | Same idea and string in all three, each re-implemented | textually diverged | CS:715-724; TakaroWebSocketRunner.cs:477-488; WebSocketTransport.cs:704-712 |
| Config file model (`IsFreshConfig`, `ResolveSettings`, `WithIdentity`) | Rust-only (grep finds no sibling hits) | — | CS:23-202 |
| Backoff constants 5 s / 1.5× | Same values as the Zomboid/Minecraft Java configs; not shared code | — | CS:440-442; zomboid/.../TakaroWebSocketClient.java:1059 |
| Shared harness | Uses the generic `checks_lifecycle.check_reconnect` from maintenance | shared | maintenance/src/takaro_maint/games/rust/verify.py:204 |

No `diff -q` candidate exists: Rust has no file-level copy of any sibling. **Recommendation:** share acceptance fixtures (burst, blocked send, unload with pending work) rather than code, because the runtimes differ (Oxide/Carbon single-file compile vs BepInEx assemblies vs WebSocketSharp).

Docs to fix later:
- DEVELOPMENT.md:21 says "All 17 Takaro actions"; 19 exist (CS:1148-1149).
- DEVELOPMENT.md:33,335 name the retired `carbon-25454815`.
- DEVELOPMENT.md:294-297 says hooks are unproven while README.md:139-178 marks them ✅.
- connector.json:6 lists env vars as a requirement.

## 9. Existing tests covering C4/C6, and gaps

| Test | What it covers | Ref |
|---|---|---|
| `verify` check `reconnect` | Server-side close 1001 → re-identify within budget → ping + `testReachability` on the new socket | maintenance/src/takaro_maint/verify/checks_lifecycle.py:108-146; rust/verify.py:204; targets/carbon-25902425.json:137 |
| `verify` check `stop` | shutdown → saved → connector unloaded → quit | maintenance/src/takaro_maint/games/rust/verify.py:288 |
| `tests/config/run.sh` | Pure config region: fresh, upgrade, env precedence, half-saved file, identity put-back, JWT redaction. No socket | games/rust/tests/config/Program.cs:36-127; .github/workflows/rust.yml:107 |
| `tests/map`, `tests/names` | Map geometry and entity names only | .github/workflows/rust.yml:102,112 |

**Gaps (none of these exist):**
- send-lock contention or blocked-send burst
- outbound task count or memory
- event ordering
- events while disconnected (loss is by design, but untested)
- unload/hot-reload with pending `NextTick` jobs or send tasks
- request executed after the Takaro timeout
- `NextTick` depth or per-frame cost
- >8 KB / fragmented / multibyte frames
- half-open socket
- timed-ban expiry across restart
- `_lastPosition` staleness
- Oxide load of any kind

Runtime CI is off for releases (`verification.executed: null`, DEVELOPMENT.md:338-340).

## 10. Top 3 risks

| # | Sev | Risk | Proposed follow-up |
|---|---|---|---|
| 1 | **high** | The outbound path is unbounded and blocking. Each message is a `Task.Run` that parks a thread-pool thread on a Monitor held across a sync `SendAsync().GetResult()`. There is no send timeout and no half-open detection, and ordering is not guaranteed even at idle. A slow or half-open Takaro link leads to thread-pool growth, memory growth, reordered or late events, and starved receive continuations (CS:460, 975-1002, 761) | Replace it with one bounded outbound queue and a single async sender with a send timeout and drop/disconnect-on-overflow (Valheim's `SendAsync` + `PendingEventQueue` and 7D2D's `MAX_PENDING_MESSAGES=500` are the in-repo precedents). Add a blocked-send burst fixture first |
| 2 | **med** | Main-thread work is unbounded and has no deadline. `NextTick` has no admission, cap or deadline, so a timed-out mutation still runs. Catalogs (`listEntities` prefab scan, `listItems`), map-tile PNG encode and response serialization all run on the main thread on every call (CS:1159-1181, 1312-1353, 1672-1679, 1063) | Cache item and entity catalogs as immutable JSON once after `OnServerInitialized`. Stamp requests with a receive time and skip expired *reads*. Count the queue depth and log it. Measure per-frame cost before choosing a budget |
| 3 | **med** | State and delivery are lossy and ambiguous. Events are silently dropped whenever `!_connected` (no outbox, v0, no acks). Timed bans are connector-owned and enforced only while the plugin is loaded. `_lastPosition` has no freshness or eviction and returns `{0,0,0}` for unknown players (CS:1068, 2023-2035, 1252-1280) | Decide on v1 + a bounded outbox together with the transport fix. Document or mirror timed bans (e.g. write a native ban with notes and lift it on expiry). Return null or a "stale" marker instead of the origin |

Also noted, low severity: per-chunk UTF-8 decode and no max frame size (CS:805-816); `connector.json:6` env requirement is stale; Oxide compile relies on Carbon publicizing internals (csproj:21), which is unverified for Oxide.
