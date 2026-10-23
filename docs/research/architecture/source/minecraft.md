# Minecraft (Paper, Fabric, NeoForge): issue #298 architecture analysis

**source-only.** Built from reading HEAD `3859fd8` only: no builds, no game runs, no network. Where a claim depends on platform or library internals (Bukkit scheduler, `MinecraftServer` task loop, Java-WebSocket 1.6.0) rather than repo code, it is labelled **[platform]** or **[library]** and needs runtime confirmation.

Path aliases (all relative to repo root):
- `C/` = `games/minecraft/mod/core/src/main/java/io/takaro/minecraft/core/`
- `P/` = `games/minecraft/mod/paper/src/main/java/io/takaro/minecraft/paper/`
- `F/` = `games/minecraft/mod/fabric/src/main/java/io/takaro/minecraft/fabric/`
- `N/` = `games/minecraft/mod/neoforge/src/main/java/io/takaro/minecraft/neoforge/`
- `T/` = `games/minecraft/mod/core/src/test/java/io/takaro/minecraft/core/`
- `TWS` = `C/TakaroWebSocketClient.java`

---

## 1. Architecture on main

| Item | Paper | Fabric | NeoForge |
|---|---|---|---|
| Placement | In-process Bukkit plugin; `JavaPlugin implements GameAdapter` (`P/TakaroPaperPlugin.java:25`) | In-process mod, `DedicatedServerModInitializer` (`F/TakaroFabricMod.java:24`) | In-process mod, `@Mod("takaro")` (`N/TakaroNeoForgeMod.java:25`) |
| Sidecar | none | none | none |
| Start hook | `onEnable` (`P/TakaroPaperPlugin.java:31-44`), plugin.yml has no `load:` key, so the default (POSTWORLD) applies (`games/minecraft/mod/paper/src/main/resources/plugin.yml:1-5`) | `SERVER_STARTED` (`F/TakaroFabricMod.java:32,142-147`) | `ServerStartedEvent` (`N/TakaroNeoForgeMod.java:36-44`) |
| Install dir | `plugins/` (`catalog/minecraft/targets/paper-1.21.11.json:67`) | `mods/` (`catalog/minecraft/targets/fabric-26.2.json:95`) | `mods/` (`catalog/minecraft/targets/neoforge-1.21.11.json:72`) |
| Java runtime | 21 (`paper-1.21.11.json:29`) | 25 (`fabric-26.2.json:57`) | 21 (`neoforge-1.21.11.json:37`) |
| Config file | `plugins/TakaroMinecraft/config.yml` (`P/TakaroPaperPlugin.java:40-41`) | `config/takaro.json` (`F/TakaroFabricMod.java:143`) | `config/takaro.properties` (`N/TakaroNeoForgeMod.java:40`) |
| Folia | **not declared**: plugin.yml has no `folia-supported` (`plugin.yml:1-5`), and `grep -ri folia games/minecraft` finds nothing | n/a | n/a |
| Default target | `paper-1.21.11`, `default: true`, maintained (`paper-1.21.11.json:7,9`) | `fabric-26.2`, `default: true` (`fabric-26.2.json:7,9`); the newest maintained Fabric target `fabric-26.3` is `default: false` (`fabric-26.3.json:7,9`); `fabric-26.1.2` backport (`fabric-26.1.2.json:7`) | `neoforge-1.21.11`, `default: true` (`neoforge-1.21.11.json:7,9`) |
| Game default platform | `defaultPlatform: "fabric"` (`catalog/minecraft/game.json:7`) | | |

Shared by all three loaders:

| Item | Value |
|---|---|
| Shared core | `C/` (`TakaroConnector`, `TakaroWebSocketClient`, `EventOutbox`, `ConfigFile`, `WorldMap`, `TargetGuard`). Every loader delegates to it through `GameAdapter` (`C/GameAdapter.java:10-55`). |
| Transport | `org.java-websocket:Java-WebSocket` 1.6.0 (`games/minecraft/mod/gradle/libs.versions.toml:6,15`), shaded and relocated to `io.takaro.libs.websocket` (`games/minecraft/mod/targets/paper-1.21.11/build.gradle.kts:16`, `targets/fabric-26.2/build.gradle.kts:41`, `targets/neoforge-1.21.11/build.gradle.kts:29`). JSON library is Gson 2.14.0 (`libs.versions.toml:7`). |
| TLS | No custom `SSLSocketFactory` or trust manager anywhere in `C/` (grep for `SSL\|TLS\|trust` returns nothing). It runs on the JVM default trust store. **[library]** Java-WebSocket ≥1.5 sets HTTPS endpoint identification in `onSetSSLParameters`, so hostname verification is on by default. The default URL is `wss://connect.takaro.io/` (`C/TakaroConfig.java:12`). |
| Connector version | `0.5.0` (`games/minecraft/version.txt`). README still says "version **0.1.0**" (`games/minecraft/README.md:3`), which is doc drift. |
| Protocol | v0 by default. v1 (streamId/seq/eventAck) is offered **only when `map=true`** (`TWS:412-418`), and `mapEnabled` defaults to false (`C/TakaroConfig.java:23`). |

## 2. Drift vs the issue (MIN-1..4)

| Hypothesis (quoted from the issue) | Status at HEAD | Evidence |
|---|---|---|
| MIN-1 "Run startup/protocol and representative real-client gameplay coverage on each maintained loader family… Do not treat Folia… as supported unless declared and tested." | **Changed.** There are now 5 maintained targets (`fabric-26.3` is new since 147acb2; `git ls-tree 147acb2 catalog/minecraft/targets` lists 4). The catalog records real-client gameplay for fabric-26.1.2, 26.2, paper and neoforge, and an e2e run for fabric-26.3 (`fabric-26.3.json:12`). Folia is still undeclared. README still says Paper and NeoForge are untested, which contradicts the catalog. | `paper-1.21.11.json:10`; `README.md:199-201,245-246`; `plugin.yml:1-5` |
| MIN-2 "Profile player/inventory queries, item/entity catalog construction, and JSON building. Determine the minimum safe main-thread snapshot… and the lifecycle events needed to invalidate catalog/world caches." | **Still true.** The item and entity catalogues are rebuilt from the registry on every request, on the main thread, with no cache, so there is nothing to invalidate (`P/TakaroPaperPlugin.java:133-165`, `F/FabricGameAdapter.java:116-146`, `N/NeoForgeGameAdapter.java:119-149`). The JSON tree is built on the main thread too (`TWS:607-616,650-657`). New since 147acb2: WorldMap caches (LRU rasters and tiles, invalidated by region-file mtime). | `C/map/WorldMap.java:38-39,69-72,296-318` |
| MIN-3 "Burst requests, delay scheduled work, and disconnect Takaro while work is queued… Test fragmentation/reconnection through the current wire client." | **Still true, partly changed.** There is still no timeout, admission limit or pending-request map (`TWS:607-632`; the old revision was identical at 147acb2 TWS:345-365). New since then: a silence watchdog (`TWS:42,76-94`), a v1 outbox that is gated on the map setting (`C/EventOutbox.java:19`), and config hot-reload that replaces the socket (`C/TakaroConnector.java:143-161`). The library is the same, Java-WebSocket 1.6.0 (unchanged since 147acb2). | see §5 |
| MIN-4 "Exercise last-position cache TTL/eviction, world/dimension changes, disconnect enrichment, plugin reload where supported, and server restart… retain native ban ownership." | **Still true.** `lastKnownLocations` is unchanged since 147acb2: an unbounded map with no TTL or eviction. Bans are still native. Server-stop disconnect enrichment is Fabric-only (`F/TakaroFabricMod.java:110-140`). Paper `/reload` is the only hot reload (`games/minecraft/scripts/reload.sh:8-22`). | `P/TakaroPaperPlugin.java:29`, `F/FabricGameAdapter.java:32`, `N/NeoForgeGameAdapter.java:31` |

## 3. C1 install (renter steps)

| Step | Paper | Fabric | NeoForge |
|---|---|---|---|
| Files | 1 jar into `plugins/` (`README.md:66-73`) | 1 jar into `mods/`, plus the Fabric API jar, plus a Fabric loader ≥ declared min (`README.md:16-17,72-73`; `games/minecraft/mod/fabric/src/main/resources/fabric.mod.json` `depends`) | 1 jar into `mods/` (`README.md:68`) |
| Loader | Paper build 132 (`paper-1.21.11.json` inputs.loader) | Fabric launcher 0.19.5 / API 0.160.0 (`fabric-26.2.json` inputs) | NeoForge 21.11.45 (`neoforge-1.21.11.json` inputs) |
| Launch args / env | none required. `TAKARO_*` env vars are optional overrides (`C/TakaroConfig.java:65-90`). No `-javaagent`, no LD_PRELOAD. | same | same |
| Writable state | `plugins/TakaroMinecraft/config.yml`. The connector writes it with the generated `identity_token` (`C/config/ConfigFile.java:68-75,141,152-167`). | `config/takaro.json` | `config/takaro.properties` (path relative to the CWD, `N/TakaroNeoForgeMod.java:40`) |
| Restart | Required for install and upgrade (`README.md:192-195`). The token and identity are hot-reloaded with no restart (config poll every 5 s: `C/TakaroConnector.java:19,78-79`). | same | same |
| Outbound | WSS to `connect.takaro.io:443` (`C/TakaroConfig.java:12`) | same | same |
| Update preservation | The config sits outside the jar (`README.md:193-195`). The catalog preserves `plugins/TakaroMinecraft/` (`paper-1.21.11.json` preserve). | `config/` preserved | `config/` preserved |
| Classification | Managed JVM plugin jar (equivalent to a "managed plugin DLL"). Not an arbitrary native library, not a JVM agent, no extra process. | Managed mod jar | Managed mod jar |
| Host control needed | File upload to `plugins/`, restart, edit a config file. These are standard panel controls. | Host must offer a Fabric server type with Fabric API and Java 25. | Host must offer a NeoForge type with Java 21. |
| Renter status | documented but untested (no rental evidence in repo) | documented but untested | documented but untested |

## 4. C3 thread trace (source-only)

Request path, shared by all loaders: the WS read thread **[library]** runs `onMessage` → `handleRequest` (`TWS:131-198,488-575`) → `handleOnMainThread` (`TWS:607-632`). That submits a closure that captures only `requestId` and the parsed `JsonObject args`, not game objects. The closure runs on the game thread and calls the adapter and the JSON tree builders. Its `CompletableFuture` completes there, and `whenCompleteAsync` (`TWS:617`) then serialises and calls `send` on the **ForkJoin common pool**.

| Stage | getPlayers | giveItem / teleportPlayer | player-connected / chat-message |
|---|---|---|---|
| Hop mechanism | Paper `Bukkit.getScheduler().runTask` (`P/TakaroPaperPlugin.java:84-86`). Fabric `server.execute` (`F/FabricGameAdapter.java:70`). NeoForge `server.execute` (`N/NeoForgeGameAdapter.java:73`). | same | none: the event fires on the platform's thread |
| Game access | main thread: `Bukkit.getOnlinePlayers()` (`P/TakaroPaperPlugin.java:96-101`), `server.getPlayerList().getPlayers()` (`F/FabricGameAdapter.java:78-83`, `N/NeoForgeGameAdapter.java:81-86`) | main thread: `addItem` / `teleport` (`P/TakaroPaperPlugin.java:174-181,205-212`); `getInventory().add` / `teleportTo` (`F/FabricGameAdapter.java:153-161,187-194`; `N/NeoForgeGameAdapter.java:156-164,190-197`) | Join: Paper `PlayerJoinEvent` (`P/TakaroPaperEventListener.java:23-28`), Fabric `JOIN` (`F/TakaroFabricMod.java:41-47`), NeoForge `PlayerLoggedInEvent` (`N/TakaroNeoForgeMod.java:55-63`); all main thread **[platform]**. Chat: Paper `AsyncChatEvent` runs on an **async chat thread** **[platform]** (`P/TakaroPaperEventListener.java:40-52`); Fabric `CHAT_MESSAGE` (`F/TakaroFabricMod.java:59-69`) and NeoForge `ServerChatEvent` (`N/TakaroNeoForgeMod.java:76-88`) run on the server thread **[platform]**. |
| Snapshot copy | `PlayerInfo` record of strings and ints (`P/TakaroPaperPlugin.java:279-286`, `F/FabricGameAdapter.java:257-265`) | n/a | `PlayerInfo` / strings on the event thread |
| JSON build | Gson tree on main thread (`TWS:650-657,865-876`). String serialisation on the common pool (`TWS:617-631,899-914`). | null result → `JsonNull`/`{}` on the common pool (`TWS:626-630`) | Gson tree **and** `toString()` on the event thread (`TWS:251-278,353-362`). Under v1, `outbox.add` (synchronized) also runs on the event thread (`TWS:330`). |
| Network send | common pool → `send()` | common pool | event thread calls `send()` (`TWS:361`). **[library]** `send` only enqueues frames; the write thread does the socket I/O. |
| Disk write | none | none | none. Config writes happen on start (main) and on the `takaro-config-watch` thread (`C/TakaroConnector.java:73-79`, `C/config/ConfigFile.java:171-181`). |
| Logging | debug only (`TWS:498-500,910-912`), common pool | warning on failure, common pool (`TWS:619`) | debug `WS SEND` on the event thread (`TWS:358-360`) |
| Workers holding live game objects | **None** in the request path. | none | none |

Off-thread game access found:
- `applyConfig` → `connectLocked` → `mapLocked` runs on the `takaro-config-watch` thread (`C/TakaroConnector.java:143-161,216,108-124`). When the map is first enabled at runtime it calls `adapter.mapDimensions()` / `mapColors()`, which use `Bukkit.getWorlds()` and `Material.createBlockData()` (`P/TakaroPaperPlugin.java:290-322`) or `server.getAllLevels()` and registry iteration (`F/FabricGameAdapter.java:269-298`), **off the main thread**.
- `getMapTile` reads region files on the `takaro-map-render` and `takaro-map-redraw` threads (`C/map/WorldMap.java:66-67,184-212`). These are files only, not game objects, but the server may be writing them at the same moment.

## 5. C4 queues

| Queue / stage | Capacity | Admission | Drain per tick | Timeout | Timed-out job still runs? | Shutdown with pending |
|---|---|---|---|---|---|---|
| Paper `BukkitScheduler.runTask` (`P/TakaroPaperPlugin.java:85`) | unbounded **[platform]** | none in repo; every request submitted (`TWS:609`) | all due tasks next tick, no count/time budget **[platform]** | **none** (`TWS:607-632`, no `orTimeout`) | **Yes.** A task delayed past Takaro's 10 s window (`TWS:54`) still mutates (give/teleport/ban), then sends a late response. | Plugin disable cancels the plugin's tasks **[platform]**. The future never completes, so no response is sent. After disable, `runTask` throws `IllegalPluginAccessException`, which is swallowed by `onMessage`'s catch (`TWS:195-197`) and logged as "Failed to parse message". |
| Fabric/NeoForge `server.execute` (`F/FabricGameAdapter.java:70`, `N/NeoForgeGameAdapter.java:73`) | unbounded **[platform]** (`BlockableEventLoop` queue) | none | runs between ticks; vanilla has no per-request budget **[platform]** | none | **Yes** (same as Paper) | Tasks queued after the server stops never run; no response. |
| Pending-request map | **does not exist.** No requestId map and no dedupe. A Takaro retry of the same mutation would run twice. | – | – | – | – | – |
| `whenCompleteAsync` → ForkJoin common pool (`TWS:617`) | unbounded **[library/JDK]** | – | – | – | – | – |
| Outbound: `send()` (`TWS:361,896,913`) | Java-WebSocket `outQueue` is an unbounded `LinkedBlockingQueue` **[library]**. Frame writes are serialised by the library's write lock **[library]**. | Check-then-act: `isOpen()` then `send()` (`TWS:331-332,345-350`). A close between the two throws `WebsocketNotConnectedException` into the **game/event thread**. | – | – | – | `shutdown()` waits ≤2 s for buffered data, then ≤2 s for close (`TWS:224-247`). On Paper and NeoForge this runs on the main thread (`P/TakaroPaperPlugin.java:46-51`, `N/TakaroNeoForgeMod.java:46-51`). Fabric adds a 750 ms sleep first (`F/TakaroFabricMod.java:133-138`). |
| Map tile renderer (`C/map/WorldMap.java:66,197-212`) | single thread, unbounded executor queue; `inflight` dedupes identical keys (`WorldMap.java:63,197`) | none | – | 6 s wait (`TWS:55`, `WorldMap.java:206-208`) | Yes, the render continues and fills the cache (harmless, read-only) | `shutdownNow` (`WorldMap.java:234-237`) |
| `CompletableFuture.runAsync` for tiles (`TWS:589`) | common pool | none | – | – | – | – |
| Reconnect / watchdog (`TWS:25-29,76-81,916-931`) | single daemon thread. `reconnectBlocking` blocks it, so the watchdog is paused while connecting. | – | watchdog period `min(15 s, limit/4)`, limit 180 s (`TWS:42,79`) | – | – | `shutdownNow` (`TWS:227`) |
| Config watcher (`C/TakaroConnector.java:73-81`) | single thread; 5 s poll, 300 s map refresh (`:19-20`) | – | – | – | – | `shutdownNow` (`:238`) |

Lifetime: closures capture only Gson `args` and `requestId`, so no stack or game data can dangle. The risk is semantic, not memory safety: there is no expiry check before a mutation runs.

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Freshness / invalidation |
|---|---|---|---|---|
| Players online | game player list | none; live query per request (`P/TakaroPaperPlugin.java:96-101`, `F/FabricGameAdapter.java:78-83`) | – | live |
| Last location | game, while online | `ConcurrentHashMap lastKnownLocations`, separate per loader (`P/TakaroPaperPlugin.java:29`, `F/FabricGameAdapter.java:32`, `N/NeoForgeGameAdapter.java:31`) | memory only; reset on restart and on Paper reload | Written only when getPlayerLocation is called or on quit (`P/TakaroPaperEventListener.java:36`, `F/TakaroFabricMod.java:55`, `N/TakaroNeoForgeMod.java:72`). The quit warm-up is skipped when the emitter is null (`P/TakaroPaperEventListener.java:32-33`). **No TTL, no eviction**: it grows with every distinct player located, and an offline player's stale position is returned indefinitely (`P/TakaroPaperPlugin.java:106`). |
| Inventory | game | none; live (`P/TakaroPaperPlugin.java:113-129`). Fabric and NeoForge return non-equipment items only (`F/FabricGameAdapter.java:102`). | – | An offline player returns `[]`, indistinguishable from an empty inventory (`P/TakaroPaperPlugin.java:116`, `F/FabricGameAdapter.java:100`). |
| Item/entity catalog | registry | none; rebuilt every request on main thread | – | always fresh; no cache to invalidate |
| World map | region files on disk | LRU 64 rasters / 512 tiles (`C/map/WorldMap.java:38-39`) plus frames | memory | mtime-based redraw (`WorldMap.java:296-318`). Frame growth is checked every 300 s (`C/TakaroConnector.java:94-105`). The dimension list is fixed at first build (`C/TakaroConnector.java:107-124`), so worlds added later are missed. |
| Identity | connector | `identity_token` in the config file (`C/config/ConfigFile.java:141,152-167`), written atomically (`:171-181`) | file | restored if a user deletes it (`ConfigFile.java:143-145`) |
| Bans / timed bans | **game native** (Paper `ProfileBanList`; vanilla `UserBanList`) (`P/TakaroPaperPlugin.java:222-246`, `F/FabricGameAdapter.java:204-226`) | none | the game's `banned-players.json` | expiry is enforced by the game |
| Event cursor | connector | random `streamId` per process, `nextSeq` in memory (`C/EventOutbox.java:27,39-44`) | none; restart or Paper reload starts a new stream | – |
| Pending events | connector | `EventOutbox` (5000 cap) **only on the v1/map path** (`TWS:328-344`) | memory | lost on crash, restart or Paper reload |

## 7. C6 delivery

| Aspect | Behaviour |
|---|---|
| Default config (map off, v0) | **No outbox.** An event raised while the socket is closed is dropped silently (`TWS:345`). Events between `onOpen` and `identifyResponse` are sent before identify (the `identified` flag is not checked on this path: `TWS:345-350`). |
| v1 path (map=true) | `EventOutbox` cap 5000, drops the oldest (`C/EventOutbox.java:19,46-50`). The `dropped()` counter is never logged or read (`EventOutbox.java:72`; no caller in `C/`). |
| Ack semantics | v1: cumulative `eventAck{streamId,upTo}` (`TWS:159-165`, `EventOutbox.java:55-60`). v0: the local send counts as delivered (`TWS:335-337`). App-level `ping` is **not handled**; it falls to "Unknown message type" (`TWS:191-193`). The Zomboid sibling answers ping with pong (`games/zomboid/mod/core/src/main/java/io/takaro/zomboid/core/TakaroWebSocketClient.java:116,477-490`). |
| Reconnect replay | v1 resends all unacked events after identify, from the WS read thread (`TWS:477-485`). New events from the game thread can interleave, so seq order on the wire is not guaranteed. Exponential backoff runs 5 s → ×1.5 → max 300 s (`C/TakaroConfig.java:18-20`, `TWS:916-931`). Codes 1008/4001/4003 stop reconnecting (`TWS:206-212`). The silence watchdog drops the socket after 180 s with no inbound traffic (`TWS:42,84-94`). |
| Mutations replayed? | No: requests are never replayed. Late execution after Takaro's timeout is possible (§5). |
| Server stop: player-disconnected | Fabric emits for every online player at `SERVER_STOPPING`, then sleeps 750 ms (`F/TakaroFabricMod.java:110-140`). **NeoForge does not** (`N/TakaroNeoForgeMod.java:46-51`). `closeClientLocked` nulls the emitter first (`C/TakaroConnector.java:229`), so a logout fired later during stop is dropped. Paper relies on `PlayerQuitEvent`. **[platform]** CraftBukkit disables plugins before `removeAll`, so the event is likely lost too. Runtime check needed. |
| Game crash | Everything in memory is lost: the outbox, the stream, the location cache. Online-player reconciliation relies on Takaro polling `getPlayers`. |

## 8. C8 shared code

| Code | Shared with | Divergence |
|---|---|---|
| `C/` across Paper, Fabric and NeoForge | one source tree; each loader implements only `GameAdapter` and event wiring | – |
| `F/FabricGameAdapter.java` vs `N/NeoForgeGameAdapter.java` | copy-paste | Body diff is ~8 lines: class name, `modVersion` (FabricLoader vs `ModList`), `item.getName(item.getDefaultInstance())` vs `item.getName()`, one comment. Candidate for a shared vanilla-mapped adapter base. |
| `F/TakaroFabricMod.java` vs `N/TakaroNeoForgeMod.java` | parallel structure; the death and kill handlers are near-identical (`F:71-107`, `N:90-128`) | Fabric has the stop-time disconnect emission and NeoForge lacks it (§7). |
| `C/EventOutbox.java` vs `games/zomboid/mod/core/src/main/java/io/takaro/zomboid/core/EventOutbox.java` | same lineage | **Diverged**: MC cap 5000 vs Zomboid `DEFAULT_CAPACITY = 1000`; Zomboid has a `Pending` record, `drain()` for v0 and returns its ack count (`diff`: 71 changed lines). |
| `TWS` vs Zomboid `TakaroWebSocketClient.java` (932 vs 1063 lines) | same lineage: same `handleOnMainThread` and dispatch switch (`TWS:519-570`; Zomboid `:516-561`) | **Diverged**: Zomboid adds `orTimeout(10, SECONDS)` (Zomboid TWS `:681`), ping→pong (`:477-490`) and a chunked map image (`:27`). MC adds `getMapTile` and the watchdog. 885 changed lines after renaming. |
| `TakaroConnector`, `TakaroConfig`, `GameAdapter` vs Zomboid | same names, largely rewritten (249, 124 and 39 diff lines) | Zomboid's main-thread hop is its own `MainThreadQueue` (`games/zomboid/mod/agent/src/main/java/io/takaro/zomboid/agent/MainThreadQueue.java:15-41`): unbounded and fully drained per tick. |
| Build | both use Java-WebSocket 1.6.0 and Gson 2.14.0 (`games/zomboid/mod/gradle/libs.versions.toml:7-8`) | – |
| Hytale | no `games/hytale` at HEAD (`ls games/`) | – |

## 9. Existing tests and gaps

| Test | Covers |
|---|---|
| `T/ProtocolV1Test.java:131` `eventsCarryStreamAndSeqAndAreResentUntilAcked` | v1 hold before identify, seq, ack, resend on reconnect (C6) |
| `T/ProtocolV1Test.java:163` `whenTakaroPicksV0EventsStayPlain` | v0 fallback |
| `T/ProtocolV1Test.java:233` `aSilentOpenConnectionIsDroppedSoItReconnects` | watchdog (C6) |
| `T/ConfigReloadContractTest.java:175` `shutdownStopsWatchingAndConnecting`; `:187` `concurrentConfigChangesAndShutdownLeaveNoSocketBehind` | shutdown and replace races (C4 lifetime) |
| `T/ConfigReloadContractTest.java:135` `rejectedTokenBannerThenCorrectedTokenReconnectsAtOnce` | reconnect after a token fix |
| `T/ActionRoutingTest.java:44-301` (19 tests) | dispatch and argument shape. `runOnMainThread` runs **synchronously** (`T/ActionRoutingTest.java:374`, `T/TakaroConnectorTest.java:38`), so no delay is ever modelled. |
| `games/minecraft/mod/core/src/test/java/io/takaro/minecraft/core/map/WorldMapTest.java:175,203` | map cache invalidation on save and delete |

Gaps: no test for any of these:
- a delayed or blocked main thread (late mutation, no timeout)
- burst admission
- outbox overflow (`CAPACITY`, `dropped`)
- send after close (`WebsocketNotConnectedException` on the event thread)
- `lastKnownLocations` growth or staleness
- a ping message
- a fragmented inbound frame
- server-stop disconnect per loader
- Paper reload

There are no Fabric or NeoForge unit tests (`games/minecraft/mod/{fabric,neoforge}/src` contain only `main`). Paper has only `PaperNamesTest`.

## 10. Top 3 risks

| # | Risk | Severity | Follow-up |
|---|---|---|---|
| 1 | Requests that Takaro has already timed out still mutate the game. There is no timeout, expiry or requestId dedupe, and the main-thread queues are unbounded on all 3 loaders (`TWS:607-632`, `P/TakaroPaperPlugin.java:85`, `F/FabricGameAdapter.java:70`). A lag spike plus a retry (shop delivery, give or teleport) can apply a mutation twice or after the error was reported. | high | Add a per-request deadline that is checked on the main thread before the mutation runs, `orTimeout` on the response (as Zomboid does), and a bounded in-flight count with an `overloaded` error. Add a delayed-scheduler unit test. |
| 2 | With the default config (map off, so v0), events are dropped silently whenever the socket is down (`TWS:345`). The v1 outbox exists only behind `map=true` (`TWS:328`, `C/TakaroConfig.java:23`). NeoForge (and probably Paper) also lose player-disconnected on server stop (`N/TakaroNeoForgeMod.java:46-51`, `C/TakaroConnector.java:229`). | high | Decouple the v1 offer and the outbox from the map flag, port Fabric's stop-time disconnect emission to NeoForge and Paper, and log `outbox.dropped()`. |
| 3 | Thread-safety edges: (a) `isOpen()`→`send()` check-then-act throws `WebsocketNotConnectedException` into game and event threads (`TWS:331-332,345-350`); (b) toggling the map on at runtime calls `mapDimensions`/`mapColors` (Bukkit and registry access) from the config-watch thread (`C/TakaroConnector.java:143-161,108-124`); (c) `lastKnownLocations` is unbounded and never expires (`P/TakaroPaperPlugin.java:29`). | med | Wrap event sends in try/catch with outbox retention; build the map's dimension and colour snapshot on the main thread via `runOnMainThread`; evict the location cache on quit plus a TTL, or bound it. |

Lower-priority notes:
- Folia is undeclared, which is correct while untested.
- The default Fabric target is 26.2 while 26.3 is also maintained (`fabric-26.3.json:7`).
- README is stale on version and on Paper/NeoForge test status (`README.md:3,199-201`).
- App-level ping is unhandled (`TWS:191-193`).
