# Project Zomboid: source-level architecture (issue #298)

All findings are **source-only** at HEAD `3859fd8`. Nothing was run. Paths are relative to the repo root.
Abbreviations: `A/` = `games/zomboid/mod/agent/src/main/java/io/takaro/zomboid/agent/`,
`C/` = `games/zomboid/mod/core/src/main/java/io/takaro/zomboid/core/`, `WSC` = `C/TakaroWebSocketClient.java`.

## 1. Architecture on main

| Item | Value | Evidence |
|---|---|---|
| Placement | In-process **JVM agent** (`premain`), no sidecar | `A/TakaroAgent.java:28-105`; `games/zomboid/DEVELOPMENT.md:184-192` |
| Runtime | The game's bundled JRE, Java 25 (pinned) | `catalog/zomboid/targets/linux-42.21.0.json:65-67` |
| Loader | `-javaagent:` through `JAVA_TOOL_OPTIONS` (preferred), or `vmArgs` in `ProjectZomboid64.json`, or a launch option | `games/zomboid/DEVELOPMENT.md:203-213`; `games/zomboid/README.md:57-69` |
| Hooking | ByteBuddy Advice on `RCONServer.update` (tick), `GameServer.receivePlayerConnect/disconnectPlayer`, `ChatServer.sendMessage`, `IsoPlayer/IsoZombie.onKilled`, `ZLogger.write` | `A/HookInstaller.java:87-136` |
| Connector start | On the first tick of the real server JVM, not in premain (the launch chain spawns 3 JVMs) | `A/TakaroAgent.java:13-19,70-85`; `A/hooks/Bridge.java:105-116,142-157` |
| Transport | Java-WebSocket 1.6.0 (`WebSocketClient`), shaded to `io.takaro.zomboid.libs.org.java_websocket` | `C/TakaroWebSocketClient.java:9,24`; `games/zomboid/mod/gradle/libs.versions.toml:7`; `games/zomboid/mod/agent/build.gradle.kts:153` |
| TLS verification | Library/JVM default. The repo has no `SSLContext`, `TrustManager`, `HostnameVerifier` or `setSocketFactory` override; the grep over `games/zomboid/mod` (non-test) is empty. Certificate checks use the bundled JRE truststore. Hostname checking is the library default and was not verified at runtime. | grep result; `WSC:53-59` |
| Wire protocol | v0 and v1 (identify offers `[0,1]` when `protocol=1`); v1 adds the event outbox, acks and map | `WSC:381-411`; `C/EventOutbox.java` |
| Default catalog target | `linux-42.21.0` (`default: true`, **status `candidate`**; no target is `maintained`). `linux-42.20.4` is `retired`. | `catalog/zomboid/targets/linux-42.21.0.json:3,7-9`; `catalog/zomboid/targets/linux-42.20.4.json:7-9` |
| Connector version | `1.4.0` (README header still says "version **1.0.0**", which is stale) | `games/zomboid/version.txt:1`; `games/zomboid/README.md:3` |
| Target guard | Hashes the server jar in premain and refuses to install hooks on a mismatch unless `TAKARO_TARGET_POLICY=warn/off` | `A/TakaroAgent.java:34-36`; `A/TargetGuard.java:39,74-104,120` |
| Runtime CI | None: `runtime: false`. CI runs only the JUnit suites against the pinned jar. | `.github/workflows/zomboid.yml:90-91,131-132` |

## 2. Drift vs the issue (ZOM-1..4, written at `147acb21`)

| Hypothesis (issue) | Status at HEAD | Evidence |
|---|---|---|
| ZOM-1 "Verify the pinned JVM/server and `-javaagent` installation path; record renter startup-control requirements" | **Still true** but the target moved: 42.20.4 is retired and 42.21.0 is the `candidate` default (Java 25). Install paths now resolve from the jar location (`TakaroPaths`), not the hard-coded `/home/steam/Zomboid/Takaro`. | `catalog/zomboid/targets/linux-42.20.4.json:9`; `A/BanStore.java:53-55` (old rev used a hard-coded path, `git diff 147acb21 HEAD`) |
| ZOM-2 "Delay the tick queue … exceed the response timeout … count which mutations execute after the timeout … bounded alternative" | **Still true, unchanged.** `MainThreadQueue.java` is byte-identical to the old rev (the diff is empty). It is unbounded, has no budget and does not cancel on timeout (§5). | `A/MainThreadQueue.java:17,26-41`; `WSC:669-698` |
| ZOM-3 "Profile event serialization, per-call logging, ban/expiry JSON writes from tick/hook paths; pending-write shutdown, storage errors" | **Still true.** Event JSON is built on hook threads. `AgentLog.log` opens, appends and closes the file on every line on the caller thread. `BanStore.save` does a non-atomic `Files.writeString` on the main thread. | `WSC:192-249`; `A/AgentLog.java:34-51`; `A/BanStore.java:81-92` |
| ZOM-4 "immutable player reconciliation, catalog freshness, last-location eviction, identity reuse, timed-ban metadata consistency" | **Still true / partly changed.** The registry is an immutable `Map.copyOf` snapshot. `lastLocations` is still never evicted. Item cache TTL is 10 min. Timed-ban metadata can still diverge from game bans (§6). Change: local-account identity scoping was added (`LocalAccounts` + identityToken). | `A/PlayerRegistry.java:20-26`; `A/ZomboidAdapter.java:145-158,163,176-186`; `A/LocalAccounts.java:10-15`; `A/Pz.java:133-135` |
| Starting evidence "wire timeout and ban-store tests" | **Ban-store tests exist; no wire-timeout test exists.** Every test adapter runs `runOnMainThread` inline, so the 10 s timeout and `MainThreadQueue` are never exercised. | `games/zomboid/mod/agent/src/test/.../BanStoreTest.java:16,31,42,55`; `ActionRoutingTest.java:523`, `ProtocolV1Test.java:386`, `PingPongTest.java:64`, `ConfigReloadTest.java:212`, `TakaroConnectorTest.java:29` |
| New since the old rev | Protocol v1 `EventOutbox` (did not exist at `147acb21`), config hot-reload and retire, map service, target guard on 42.21.0 | `C/EventOutbox.java` (`git show 147acb21:` fails); `C/TakaroConnector.java:44-54` |

## 3. C1: install (source/docs-only)

| Step | Requirement | Evidence |
|---|---|---|
| File | Put one jar at `<Zomboid data dir>/Takaro/TakaroConnector.jar` (outside the install dir, so it survives `validate`) | `games/zomboid/README.md:42-53` |
| Loader | `JAVA_TOOL_OPTIONS=-javaagent:<jar>` on the server process. Fallback: `vmArgs` in `ProjectZomboid64.json`, which SteamCMD `validate` reverts. | `games/zomboid/README.md:55-69` |
| Writable state | `Takaro/TakaroConfig.txt` (created on the first tick with a new identityToken), `takaro-agent.log`, `bans.json`, `map/` cache | `games/zomboid/DEVELOPMENT.md:217-233`; `A/AgentLog.java:27`; `A/BanStore.java:53-55`; `A/MapService.java:29-31` |
| Config | Token via the file (hot-reloaded every 5 s) or `TAKARO_*` env | `games/zomboid/README.md:73-101`; `A/ConfigLoader.java:32,167-182` |
| Restart | Needed for install and upgrade ("never swap the jar under a running server"). A token change needs no restart. | `games/zomboid/README.md:134-138,79-82` |
| Network | Outbound WSS to `wss://connect.takaro.io/` | `games/zomboid/README.md:88` |
| Classification | **JVM agent** (startup-argument class). This is not a plugin-folder drop-in: "there is no `Mods/` folder". | `games/zomboid/README.md:17-19`; `games/zomboid/connector.json:7` ("Ability to set JAVA_TOOL_OPTIONS or edit vmArgs") |
| Host control needed | The renter must be able to set a process env var or edit the JVM args file, and re-apply it after updates when using `vmArgs`. Panels that expose only FTP plus game settings cannot load it. | same |

Renter status: **requires host cooperation or a startup-parameter/env field** (no rental evidence in the repo).

## 4. C3: thread trace (source only)

| Path | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| **getPlayers** (request) | Main thread: `WSC:518-519` → `handleOnMainThread` → `MainThreadQueue` → `Bridge.tick` drain → `Pz.getPlayers` iterates `GameServer.udpEngine.connections` (`A/Pz.java:111-126`) | **Main thread**: `handleGetPlayers` builds the `JsonArray` inside the queued supplier (`WSC:762-769`) | ForkJoin common pool via `whenCompleteAsync` → `send()` (`WSC:681-697,1030-1036`) | none | Debug only, on the pool thread (`WSC:1032-1034`) |
| **giveItem** (mutation) | Main thread: `Pz.giveItem` → `ScriptManager.FindItem`, `ItemContainer.AddItems`, `GameServer.sendAddItemsToContainer` (`A/Pz.java:288-305`; `A/ZomboidAdapter.java:205-211`) | Response `null`/empty, built on the pool thread (`WSC:689-696`) | Pool thread | none | Failure: `logWarning` → `AgentLog` file append on the pool thread (`WSC:687`) |
| **teleportPlayer** (mutation) | Main thread: `GameServer.sendTeleport` (`A/Pz.java:273-280`) | as giveItem | Pool thread | none | as giveItem |
| **banPlayer** (mutation) | Main thread: `ServerWorldDatabase.banUser/banSteamID` (SQLite) + kick (`A/Pz.java:310-328`; `A/ZomboidAdapter.java:246-257`) | pool | pool | **Main thread**: `BanStore.put` → `save()` rewrites `bans.json` (`A/BanStore.java:94-97,81-92`) | main thread |
| **player-connected** (event) | Hook `receivePlayerConnect` only marks dirty (`A/hooks/Bridge.java:247-260`). The next tick runs `Reconciler.tick` → `Pz.getPlayers` on the main thread (`Bridge.java:125-132`; `A/Reconciler.java:58-98`). | **Main thread**: `emitPlayerConnected` → `playerInfoToJson` + outbox payload (`WSC:192-197,266-281`; `C/EventOutbox.java:51-66`) | **Main thread** calls `send()`. Java-WebSocket enqueues to its writer thread (library behaviour, not in repo source). | **Main thread**: `AgentLog.log` appends to `takaro-agent.log` per line (`A/Reconciler.java:74`; `A/AgentLog.java:34-51`) and `Bridge.java:252` "join detected" | main thread |
| **chat-message** (event) | Hook thread of `ChatServer.sendMessage` (the game's thread; assumed main, not verified in source) reads the `ChatMessage` (`A/hooks/Bridge.java:222-244`) | Hook thread (`WSC:206-213`) | Hook thread `send()` | none | none on the success path |

Live game objects on non-game threads:
- The `ChatMessage`, `IsoPlayer` and killer objects are used only inside the hook invocation.
- Workers receive copied `PlayerInfo` records (`A/PlayerRegistry.java:20-26`).
- `MapService` runs on the `takaro-map` daemon thread and reflectively reads `ServerOptions.instance.getOption("Map")` off the main thread (`A/MapService.java:71-93,190-198`). This is a minor off-thread game read.
- `getMapImage` is served on the WS thread from the cached byte array (`WSC:563-566,708-730`). It holds no game objects.

## 5. C4: queues

| Queue | Capacity | Admission | Per-tick drain | Timeout behaviour | Shutdown |
|---|---|---|---|---|---|
| `MainThreadQueue.queue` (`ConcurrentLinkedQueue<Runnable>`) | **Unbounded** (`A/MainThreadQueue.java:17`) | Always `offer` (`:31`). Runs inline when the caller is already on the main thread (`:26-30`). | **No count or time budget.** `while ((r = queue.poll()) != null)` (`:36-41`) also runs tasks enqueued during the drain. It is called every `RCONServer.update` (`A/hooks/Bridge.java:118-121`). | **A timed-out job still runs** (see below) | No drain at shutdown. The shutdown hook only closes the socket (`A/TakaroAgent.java:98-105`). Queued tasks are dropped with the JVM. |
| Request future | one per request | n/a | n/a | `future.orTimeout(10, TimeUnit.SECONDS)` (`WSC:681`). The constant is hard-coded and not configurable. | n/a |
| `EventOutbox.pending` (`ArrayDeque`) | `DEFAULT_CAPACITY = 1000` (`C/EventOutbox.java:23`) | Always accepted. On overflow it drops the oldest and counts `dropped++` (`:59-62`). | n/a | n/a | Memory only, lost on JVM exit |
| Java-WebSocket outbound queue | Library internal (unbounded `LinkedBlockingQueue` in 1.6.0; library knowledge, not repo source) | `send()` from the main, hook, pool and WS threads | n/a | none | `closeBlocking()` (`WSC:179-188`) |
| Event rate limiters | Token buckets: entity-killed 50 burst / 20 per s, log 60 burst / 30 per s. Drops are counted. | Drop on empty | n/a | n/a | n/a |

Limiter evidence: `A/hooks/Bridge.java:47-48,317-324,376-383`.

**Can a timed-out mutation still execute? Yes.** The code path:

```java
// C/TakaroWebSocketClient.java:669-698
CompletableFuture<JsonElement> future = new CompletableFuture<>();
adapter.runOnMainThread(() -> {            // → MainThreadQueue.offer (A/MainThreadQueue.java:31)
    try { JsonElement result = handler.get(); future.complete(result); }
    catch (Throwable e) { future.completeExceptionally(e); }
});
future.orTimeout(10, TimeUnit.SECONDS).whenCompleteAsync((result, error) -> {
    ... "The game did not answer within 10 seconds" ... sendError(requestId, ...);
});
```

- `orTimeout` only completes the future exceptionally. Nothing removes the `Runnable` from `MainThreadQueue` and no cancelled flag exists (`A/MainThreadQueue.java:36-41` polls unconditionally).
- When the main thread resumes, `Bridge.tick` → `drain()` runs the handler. `Pz.giveItem`, `teleport`, `banUser` (DB plus `bans.json`), `kick` and even `shutdown` (`WSC:561`) take effect after Takaro has already received a `game_error` "did not answer".
- The late `future.complete(result)` returns `false` (no second response). The effect is silent, with no log line.
- A Takaro or module retry of the same action then double-applies, for example a double item grant.
- Java captures by reference, so there is no dangling stack data (unlike the native runners). The risk is purely semantic: a mutation lands after a timeout and is applied twice on retry.
- The same applies to requests queued on a socket that was later retired, closed or replaced. Tasks still run, and `send()` on a closed socket throws inside the discarded `whenCompleteAsync` stage, so the failure is silent.
- Sibling difference: Minecraft's copy has **no** timeout at all (`games/minecraft/mod/core/src/main/java/io/takaro/minecraft/core/TakaroWebSocketClient.java:607-632`).

## 6. C5: state inventory

| State | Authority | Connector store | Persistence | Invalidation |
|---|---|---|---|---|
| Players online | Game `udpEngine.connections` (`A/Pz.java:111-126`) | `PlayerRegistry` immutable snapshot plus `Reconciler.previous` (`A/PlayerRegistry.java:16-26`; `A/Reconciler.java:27`) | memory | Every 2 s or on dirty (`A/hooks/Bridge.java:30,125-132`). Reseeded silently on (re)connect (`A/ZomboidAdapter.java:76-79`; `A/Reconciler.java:49-55`). |
| Last location | Game (`IsoPlayer.getX/Y/Z`) | `lastLocations` `ConcurrentHashMap`, a fallback for offline players (`A/ZomboidAdapter.java:145-158`) | memory | **Never evicted**; grows with every unique player for the process lifetime |
| Inventory | Game, read live per request (`A/Pz.java:457-477`) | none | n/a | n/a |
| Item catalog | `ScriptManager.getAllItems` (`A/Pz.java:429-451`) | `itemCache`, TTL 10 min (`A/ZomboidAdapter.java:163-186`) | memory | TTL only. Entities and locations are rebuilt on every call (`:189-196`). |
| Identities | Steam id, or `LocalAccounts` scope = identityToken on `-nosteam` (`A/Pz.java:133-135,151`; `A/LocalAccounts.java`) | identityToken in `TakaroConfig.txt` | file (atomic tmp+move) | `ConfigFiles.java:147-152`. An emptied identity is put back (`games/zomboid/DEVELOPMENT.md:223-226`). |
| Bans | **Game** `ServerWorldDatabase` (bannedid table plus whitelist role) (`A/Pz.java:310-418`) | `BanStore` `bans.json` (reason, steamId, expiresAt) | file, **non-atomic** `Files.writeString` (`A/BanStore.java:87`) | `listBans` merges BanStore first (`A/ZomboidAdapter.java:267-289`). An external admin unban leaves a stale BanStore row that is still reported as banned. Expiry: `Reconciler.expireBans` removes the row **even if `unbanUser` threw** (`A/Reconciler.java:100-111`), which leaves the game ban permanent with lost metadata. An unparseable `expiresAt` becomes permanent (`A/BanStore.java:126-128`). |
| Event cursor / stream | `EventOutbox` random `streamId` per process, `seq` from 1 (`C/EventOutbox.java:35-41,51-66`) | memory, owned by `TakaroConnector` and kept across socket replacement (`C/TakaroConnector.java:18-19`) | none | A restart starts a new stream (by design, `C/EventOutbox.java:16-19`) |
| Pending events / outbox | as above, up to 1000 | memory | none | Acked cumulatively (`C/EventOutbox.java:74-84`) |
| Map image | Game `pyramid.zip` | `Takaro/map/<revision>.jpg` | file | Keyed by the zip hash (`A/MapService.java:29-31,137-139`) |

## 7. C6: delivery

| Topic | Behaviour | Evidence |
|---|---|---|
| Outbox bound | 1000 events. Overflow drops the oldest, and Takaro sees a seq gap. | `C/EventOutbox.java:16-19,23,59-62` |
| Ack semantics | v1: cumulative `eventAck {streamId, upTo}` drops acked events. `pong` (`WSC:482-493`) is a transport heartbeat only and does not touch the outbox. v0: events are sent only if `isOpen()` and otherwise **lost**. | `WSC:322-328,266-281` |
| Reconnect replay | v1: after `identifyResponse`, every unacked event is resent in seq order. If Takaro picks v0, the outbox is drained in the v0 shape with no acks. | `WSC:465-474,302-320` |
| Events before identify | Buffered (v1) | `WSC:271-279`; test `ProtocolV1Test.eventsBeforeIdentifyAreBufferedThenSentWithStreamAndSeq` |
| Join/leave during an outage | Reconciler keeps emitting into the outbox while the socket is down (the emitter object is stable). On reconnect `reseed` resets the baseline without events, so no duplicates and no loss in v1. | `A/ZomboidAdapter.java:76-79,82-88`; `A/Reconciler.java:49-55` |
| Game crash / JVM exit | Lost: the whole outbox (≤1000), queued main-thread requests (their futures never answer, so Takaro times out), and an in-flight `bans.json` write, which may truncate because the write is non-atomic. | `C/EventOutbox.java` (memory); `A/TakaroAgent.java:98-105`; `A/BanStore.java:87` |
| Reconnect policy | 5 s start, ×1.5, max 300 s. No reconnect on 1008/4001/4003 until the config changes. | `C/TakaroConfig.java:20-22`; `WSC:152-163,1043-1062` |

## 8. C8: shared code

| Zomboid file | Sibling | Status |
|---|---|---|
| `C/*` (core: wire client, outbox, adapter, config) | `games/minecraft/mod/core/src/main/java/io/takaro/minecraft/core/*` | **Copied** ("M1 lifts games/minecraft/mod/core here", `games/zomboid/mod/core/build.gradle.kts:5`). **Heavily diverged** after package normalisation: `TakaroWebSocketClient` 1063 vs 932 lines, 885 diff lines; `EventOutbox` 100 vs 75 (71); `TakaroConfig` 120 vs 122 (124); `GameAdapter` 68 vs 55 (39). |
| `EventOutbox` capacity | Minecraft `CAPACITY = 5000` (`games/minecraft/.../EventOutbox.java:19`) vs Zomboid 1000 | diverged constant |
| `handleOnMainThread` | Minecraft has no timeout (`games/minecraft/.../TakaroWebSocketClient.java:607-632`); Zomboid has 10 s `orTimeout` | diverged semantics, same "no cancel" flaw |
| Agent-only (game-specific) | `A/Pz.java`, `HookInstaller`, `Bridge`, `Reconciler`, `BanStore`, `TargetGuard`, `MapService` | Game-specific. Candidates for shared code: `MainThreadQueue` (generic), `TokenBucket`, `BanStore` (timed-ban overlay pattern). |

## 9. Existing tests touching C4/C6, and gaps

| Test | Covers |
|---|---|
| `core/src/test/.../ProtocolV1Test.java:97` `eventsBeforeIdentifyAreBufferedThenSentWithStreamAndSeq` | outbox buffering before identify |
| `ProtocolV1Test.java:113` `unacknowledgedEventsAreResentAfterReconnectAndAckedOnesAreNot` | reconnect replay plus ack |
| `ProtocolV1Test.java:132` `outboxDropsTheOldestOnOverflow` | outbox bound |
| `PingPongTest.java:36,46` `pingProducesPong`, `pingEchoesRequestId` | heartbeat |
| `ConfigReloadTest.java:119` `replacedClientDoesNotReconnectBehindTheNewOne`, `:59`, `:135` | socket retire/replace lifecycle |
| `TakaroConnectorTest.java:160,179` `configChangeAfterShutdownDoesNothing`, `shutdownWithNoConnectionDoesNotThrow` | shutdown |
| `agent/src/test/.../BanStoreTest.java:16,31,42,55` | ban round-trip, persistence, expiry selection, unparseable expiry |
| `TokenBucketTest.java` | event rate limiter |

**Gaps:**
- No test of `MainThreadQueue`.
- No test of the 10 s timeout (all fakes run `runOnMainThread` inline: `ActionRoutingTest.java:523`, `ProtocolV1Test.java:386`, `PingPongTest.java:64`, `ConfigReloadTest.java:212`, `TakaroConnectorTest.java:29`).
- No test of execute-after-timeout or the drain budget.
- No `Reconciler` test, so no coverage of expiry when unban fails.
- No test of BanStore storage errors or atomicity.
- No `lastLocations` eviction test.
- No runtime CI (`.github/workflows/zomboid.yml:131-132`).

## 10. Top risks

| # | Severity | Risk | Proposed follow-up |
|---|---|---|---|
| 1 | **High** | A timed-out mutation (giveItem, ban, kick, teleport, shutdown) still executes after Takaro got an error. A retry double-applies it. The queue is unbounded with no drain budget (`WSC:669-698`; `A/MainThreadQueue.java:17,36-41`). | Add a per-task deadline/cancel flag checked at dequeue (skip expired mutations, log them), a bounded queue with rejection, and a per-tick time budget. Add a JUnit test with a real `MainThreadQueue` and a paused drain. |
| 2 | **Med** | Main-thread I/O: per-line `AgentLog` file open+append, `BanStore` full-file rewrite (non-atomic), SQLite ban queries in `listBans`, and event/response JSON built on the tick or hook thread (`A/AgentLog.java:34-51`; `A/BanStore.java:81-92`; `A/Pz.java:374-418`; `WSC:762-769`) | Move logging to an async appender and make BanStore writes atomic (tmp+move like `ConfigFiles`). Off the tick path, keep only snapshot capture on the main thread and serialize on a worker. |
| 3 | **Med** | Ban metadata drift. Expiry removes the BanStore row even when the game unban fails. An external admin unban leaves a stale row reported by `listBans`. `lastLocations` is never evicted (`A/Reconciler.java:100-111`; `A/ZomboidAdapter.java:158,267-289`). | Remove the row only after a successful unban (retry otherwise). Reconcile BanStore against the game DB on `listBans` or a timer. Bound or evict `lastLocations`. |

Also noted (low):
- No runtime CI.
- The default target is only `candidate`.
- README version and DEVELOPMENT "premain opens the WebSocket" are stale (`games/zomboid/README.md:3`; `games/zomboid/DEVELOPMENT.md:184-185` vs `A/TakaroAgent.java:13-19`).
