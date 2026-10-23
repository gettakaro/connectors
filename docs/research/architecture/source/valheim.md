# Valheim: source-only architecture analysis (issue #298)

**source-only.** Nothing was built, run or live-tested. All references are `path:line` at HEAD `3859fd8`, relative to the repo root.
The old revision is `147acb21` ("OLD"). `V=games/valheim/mod/src`.

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | In-process managed BepInEx plugin inside the dedicated server; no sidecar. It refuses to run in a client process | `$V/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs:11-41` |
| Optional 2nd component | Graphical-client BepInEx "inventory companion" with no Takaro credentials | `catalog/valheim/game.json:42-45` |
| Runtime | Unity Mono, plugin TFM `net472` (+`net8.0` for tests), Core `netstandard2.0` | `$V/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj:3`, `$V/Takaro.Valheim.Core/Takaro.Valheim.Core.csproj:3` |
| Loader | BepInExPack Valheim 5.4.2351 + Harmony `PatchAll` | `catalog/valheim/targets/linux-1.0.20.json:65-75`, `ValheimTakaroPlugin.cs:84-85` |
| Transport | `System.Net.WebSockets.ClientWebSocket` (Mono BCL), JSON with System.Text.Json 8.0.5 | `$V/Takaro.Valheim.Plugin/TakaroWebSocketRunner.cs:1,330`, `linux-1.0.20.json:108-111` |
| TLS verification | Platform default. There is no `ServerCertificateValidationCallback` or `ServicePointManager` override anywhere in `$V` (grep found nothing) | — |
| Default catalog target | `linux-1.0.20`: `default: true`, `support.status: candidate`. No target is `maintained` | `catalog/valheim/targets/linux-1.0.20.json:3,7,9` |
| Connector version | `4.2.0` (`version.txt`). README still says "version **4.0.0**" (doc drift) | `games/valheim/version.txt:1`, `games/valheim/README.md:3` |
| Protocol | Generic v0 and v1. v1 uses `streamId`/`seq` and `eventAck` | `TakaroWebSocketRunner.cs:46-49,166,646-664` |

## 2. Drift vs the issue

| Issue hypothesis (quoted) | Status | Evidence |
|---|---|---|
| VAL-1 "Run server-only and optional companion modes separately… ensure the server-only baseline remains accurately described." | **Changed.** `companionMode` (OLD `ConnectorConfig.cs:3,20`) was removed in #353 (`6763f05`). The server is now always server-only, and an optional inventory companion affects only `getPlayerInventory` (`client-reported`) and kill vetoes. The rest of the question still applies: the companion is a second component, and the two modes still need separate runs | `games/valheim/capabilities.json:2,39,74-75`, `README.md:135-157` |
| VAL-2 "Measure the bounded main-thread queue… lifecycle polling's per-player location requests… A 64-action drain count does not establish a time budget." | **Still true.** The scheduler is unchanged since OLD (`git diff --stat` is empty). The drain is count-only (64) with no time budget. Lifecycle polling still schedules 1 + N main-thread items every 5 s | `$V/Takaro.Valheim.Core/MainThreadActionScheduler.cs:40-41,102-133`, `TakaroWebSocketRunner.cs:30,505-527` |
| VAL-3 "…receive handling ignores message completion." | **Still true, identical to OLD.** There is one 32 KiB `ReceiveAsync`, and `EndOfMessage` is never read (§5a). The send lock is still `SemaphoreSlim(1,1)`. A per-event `Task.Run` path now feeds a bounded outbox | `TakaroWebSocketRunner.cs:587-598` vs OLD `:179-187` |
| VAL-4 "companion inventory/position freshness… generations… duplicates… credential separation… validation" | **Partly obsolete.** The companion no longer reports position: positions come only from the server (`peer.m_refPos`/public position). Inventory freshness (30 s), session nonce, rising sequence, world reset and rate limits are implemented and unit-tested. Credential separation holds in source | `ValheimServerAdapter.cs:17,607-620`, `CompanionInventoryCache.cs:28-31,64-121`, `CompanionSessionRegistry.cs:156-165` |

## 3. C1 install (source/docs only)

| Step | Detail | Ref |
|---|---|---|
| Prereq | BepInExPack Valheim 5.4.2351 in the server; start the server once | `README.md:16-17` |
| Files | Unzip `TakaroValheim/` into `BepInEx/plugins/` (managed DLLs only) | `README.md:39-50` |
| Config | `BepInEx/config/com.takaro.valheim.cfg` is created on first start. Paste the registration token there; no restart is needed. The connector also writes `com.takaro.valheim.known-players.json` and a map raster under `BepInEx/cache` | `README.md:54-57`, `ValheimTakaroPlugin.cs:45,56,69`, `ValheimWorldMap.cs:180-196` |
| Launch args/env | None. A `-name` argument is read only for the default `serverName` | `ValheimTakaroPlugin.cs:48` |
| Network | Outbound `wss://connect.takaro.io/` only | `README.md:19` |
| Upgrade | Replace the plugin folder and delete `BepInEx/cache/chainloader_typeloader.dat` | `README.md:127-131` |
| Client side | Optional companion zip in the player's own `BepInEx/plugins` | `README.md:139-142` |

**Classification:** managed plugin DLL (BepInEx). Install needs the host to allow BepInEx, which many Valheim hosts expose as a "modded/BepInEx" toggle. Upload access to `BepInEx/plugins` and `BepInEx/config` is also required. It needs no native library, `LD_PRELOAD`, or extra process. Renter installability is **documented but untested**.

## 4. C3 thread trace (source only)

The main thread is Unity `Update()`. It drains 64 actions per frame (`ValheimTakaroPlugin.cs:92-98`).

| Phase | getPlayers (via lifecycle poll and Takaro request) | giveItem / teleportPlayer | chat-message event |
|---|---|---|---|
| Trigger | Receive loop or lifecycle loop, both on pool threads (`TakaroWebSocketRunner.cs:375,707`) | Receive loop, pool thread | Harmony prefix on `ZRoutedRpc.RPC_RoutedRPC`, main thread (`ValheimServerEventBridge.cs:380-391`) |
| Game access | Main thread via `ScheduleAsync` (`Adapter.cs:131-133`): `ZNet.GetPlayerList()` (`ValheimServerAdapter.cs:85`) | Main thread: prefab lookup, then `DropItemStack` or `InvokeRoutedRPC("RPC_TeleportTo")` (`ValheimServerAdapter.cs:192-217,423-429`) | Main thread: **every** routed RPC is deserialized before the hash filter (`ValheimServerEventBridge.cs:61-66`); peer resolution happens there too (`:128-134`) |
| Snapshot | Copied to `TakaroPlayer` records on the main thread | Anonymous result object with copied values (`ValheimServerAdapter.cs:224-232`) | `EventFactory.ChatMessage` object on the main thread (`:139`) |
| JSON build | Pool thread (`TryCreateActionResponse`, `TakaroWebSocketRunner.cs:709`) | Pool thread | Pool thread: `Task.Run` then `CreateGameEvent` (`TakaroWebSocketRunner.cs:142-144`) |
| Network send | Pool thread, under `sendLock` (`:736-756`) | Same | Pool thread (`FlushPendingEventsAsync` `:150-181`) |
| Disk write | **Main thread** when a player's name changes: `KnownPlayerNames.Observe` → `File.WriteAllText`/`Delete`/`Move` from `ToTakaroPlayer` (`ValheimPlayerResolver.cs:34`, `KnownPlayerNames.cs:63-73`) | — | — |
| Logging | **Main thread:** `LogInfo` on every getPlayers, every 5 s (`ValheimServerAdapter.cs:87`), plus `LogDebug` per player mapped (`ValheimPlayerResolver.cs:31`) | Main thread `LogInfo` (`ValheimServerAdapter.cs:223,430`) | Pool thread (`ValheimServerEventBridge.cs:367-371`) |
| Live objects on workers | None found. Results are records/anonymous objects with copied primitives. `GiveItem` returns `player` (a `TakaroPlayer` record) | — | `PendingKills` holds records and is main-thread only (`:22,228-259`) |

Extra main-thread costs:
- **Not-found prefab scan.** For an unknown `itemCode`, `TryFindItemDropPrefab` concatenates all `ZNetScene.m_prefabs` and scans them (`ValheimServerAdapter.cs:625-648`).
- **Catalog traversal.** `listItems`/`listEntities` traverse every prefab with `GetComponent`, unchunked (`:313-367`).
- **World map build.** This is the one place with a real per-frame time budget: 4 ms via `Stopwatch` (`ValheimWorldMap.cs:21,91-92`).

## 5. C4 queues

| Queue / stage | Capacity | Admission | Drain | Timeout / expiry | Shutdown | Ref |
|---|---|---|---|---|---|---|
| Inbound socket → request handling | None. Handling is **serial**: one request awaits main-thread dispatch and its response send before the next `ReceiveAsync` | None: unread frames sit in the TCP/WebSocket buffer | 1 at a time | No per-request deadline. The only token is the connection token | Socket disposed | `TakaroWebSocketRunner.cs:587-726` (`await dispatcher.DispatchAsync` `:707`) |
| `QueuedMainThreadActionScheduler` | `DefaultCapacity = 256` | Reject with `InvalidOperationException` when `pending.Count >= capacity` | `DefaultMaxActionsPerDrain = 64` per `Update`. Count only, no time budget | Cancelled **before** start: completes as cancelled without running (`:187-195,233-238`). Once started it **runs to completion** even if cancelled meanwhile; only the result is replaced by Cancelled (`:196-200`) | `Dispose` faults all pending items (`:135-157`) | `MainThreadActionScheduler.cs:40-157` |
| Lifecycle poll | 1 getPlayers + N getPlayerLocation items, awaited **sequentially** every 5 s. That is at least N+1 frames of latency per cycle. Location results are used only as a boolean presence filter | — | — | — | Cancelled per connection | `TakaroWebSocketRunner.cs:30,505-527`, `Events.cs:83-105` |
| Event outbox `PendingEventQueue` | `capacity: 1000` | **Drop oldest** when full (`Dropped++`). `Dropped` is never logged in production (grep finds only a test) | Flushed in full on each trigger | No age expiry | Memory only; lost on `Dispose` or crash | `TakaroWebSocketRunner.cs:31`, `PendingEventQueue.cs:56-68` |
| Per-event `Task.Run` | **Unbounded** count. Each waits on `flushLock` (`SemaphoreSlim(1,1)`) with a non-cancellable default token | — | — | Bounded in time only indirectly: each send is limited to 10 s, after which the socket is aborted | `flushLock.Dispose()` while tasks wait → `ObjectDisposedException` (logged) | `TakaroWebSocketRunner.cs:141-146,152,283`, `ValheimServerEventBridge.cs:352-374` |
| Send lock | `SemaphoreSlim(1,1)`, **held across `await socket.SendAsync`**, bounded by `SendTimeout = 10 s` → `Abort()` | FIFO-ish waiters, unbounded | — | Timed-out send task left orphaned; its exception is observed | Disposed in `Dispose` | `TakaroWebSocketRunner.cs:27,36,736-801` |
| Companion RPC | Per-peer token bucket: 10 cap, 5/s refill. Kill verdicts: 8 cap, 4/s; 256 peers; 64 KiB envelope; 256 stacks | Reject | Synchronous in the RPC handler (main thread) | — | Cleared on world switch or peer removal | `InventoryCompanionBridge.cs:71-72,268-283`, `CompanionProtocol.cs:32-34`, `CompanionRateLimiter.cs:18` |
| `PendingKills` (main thread) | Unbounded `List`; entries expire after 3 s | — | Scanned in full every frame while non-empty | 3 s | Static; not cleared on shutdown | `ValheimServerEventBridge.cs:21-22,228-259` |

Consequences derived from source:
- **The 256 capacity is effectively unreachable.** With serial receive, at most about 2 items are ever queued: 1 request plus 1 lifecycle item. Overload instead shows up as unbounded latency in the socket buffer, where admission control does not apply.
- **A request whose work starts after Takaro gave up still runs.** No request deadline exists, so a `giveItem` that waited behind a stalled frame executes later, and its response goes to an expired requestId. This risks duplicate grants if a module retries. Starting jobs cannot outlive caller stack data, because they capture only the immutable `TakaroRequest` (`Adapter.cs:131-133`).
- **Heartbeat checks sit behind main-thread work.** `HeartbeatAsync` runs only after a lifecycle pass completes (`TakaroWebSocketRunner.cs:557-558`). A frozen main thread therefore blocks both pong-timeout detection and any queued `testReachability`. The dispatcher-level guarantee "reachability answers while stalled" (`Adapter.cs:111-115`) holds only if no other request is in flight on the serial receive loop.

### 5a. Receive loop (VAL-3 focus), quoted at HEAD

```
587 private async Task ReceiveLoopAsync(ClientWebSocket socket, ConnectorConfig current, CancellationToken cancellationToken)
589     var buffer = new byte[32 * 1024];
590     while (socket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
592         var result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken);
593         if (result.MessageType == WebSocketMessageType.Close)
595             return;
598         var message = Encoding.UTF8.GetString(buffer, 0, result.Count);
599..685    substring classification: ping / pong / connected / identifyResponse / eventAck / eventError / error / request
687         var request = TakaroProtocol.ParseRequest(message);
```
(`$V/Takaro.Valheim.Plugin/TakaroWebSocketRunner.cs:587-687`). OLD `:179-187` has the same buffer, `ReceiveAsync`, Close check and `GetString`.

| Question | Answer (source-only) |
|---|---|
| Honours `EndOfMessage`? | **No.** It is never read. The observation is **still true and unchanged since OLD.** |
| Assembles fragments / >32 KiB? | **No.** Each `ReceiveAsync` chunk is handled as a whole message. The `ClientWebSocket` contract allows a partial result even below 32 KiB, so this is not limited to oversized frames. A request's first chunk is invalid JSON, so `ParseRequest` (`TakaroProtocol.cs:84`) throws. That throw is not caught in the loop and escapes to `RunAsync`'s generic catch, which drops the connection and reconnects with backoff (`TakaroWebSocketRunner.cs:422-427`). A continuation chunk usually matches no `"type":` substring and is silently skipped (`:681-685`). A multi-byte UTF-8 character split across chunks is decoded to replacement characters. |
| Max message bound | Implicit 32 KiB per read; there is no explicit limit or rejection. |
| Close frame | Returns at once without `CloseOutputAsync` or logging the close status/description (`:593-596`). `RunAsync` then loops **without backoff**, because `attempt` was reset to 0 at `:372` and no exception occurred. A server that closes after identify-accept could cause a tight reconnect loop. Identify rejection itself is handled via `IdentifyRejectedException` (`:637-641`). |
| Classification | Case-insensitive substring match on the raw text, ping first (`:599-604`). Any frame containing an unescaped `"type":"ping"` anywhere, for example in a nested object, is answered as a ping. |
| Sibling comparison | Rust **does** loop until `EndOfMessage` (`games/rust/mod/TakaroConnector.cs:805-816`), but decodes each chunk separately (same UTF-8 split hazard) and has no size cap. 7d2d uses websocket-sharp, which assembles messages itself (`games/7d2d/mod/src/WebSocket/WebSocketTransport.cs:576`). |

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Freshness / invalidation | Ref |
|---|---|---|---|---|---|
| Players online | `ZNet.GetPlayerList()` | Lifecycle snapshot (`PlayerLifecycleEventTracker`), a runner field kept across reconnects | Memory | 5 s poll. The first snapshot emits nothing, so players already online at plugin start get no `player-connected` | `Events.cs:127-152`, `TakaroWebSocketRunner.cs:28` |
| Last location | Server `peer.m_refPos` / public position | `PlayerPositionCache`, 30 s | Memory | Keyed to the world; `SwitchWorld` on each call | `ValheimServerAdapter.cs:17,76,103-141,607-620` |
| Inventory | **Client-reported** companion snapshot | `CompanionInventoryCache` per peer and session nonce | Memory | 30 s. `observedAt` is **server receive time**. Older observations are rejected; world switch and peer removal clear it | `CompanionInventoryCache.cs:28-31,64-121`, `CompanionServerMessageHandler.cs:209-211` |
| Item / entity catalog | `ZNetScene.m_prefabs` | None; traversed on each request | — | Live | `ValheimServerAdapter.cs:313-367` |
| Identities / names | Platform id from the game | `KnownPlayerNames` JSON file | File, delete-then-move (not atomic on crash) | Never invalidated | `KnownPlayerNames.cs:43-73` |
| Bans | Valheim `bannedlist` via `znet.Ban`/`Banned` | `banAliases` dictionary | Game file; aliases in memory only | No timed bans; reason dropped | `ValheimServerAdapter.cs:18,465-534`, `capabilities.json:79` |
| Event cursor | Connector `streamId` (one per process) + `seq` | `PendingEventQueue` | Memory; a new `streamId` per process | Reset on restart | `TakaroWebSocketRunner.cs:49`, `PendingEventQueue.cs:17-18` |
| Pending events | Connector | `PendingEventQueue`, 1000 frames, drop oldest | Memory | — | §5 |
| Companion sessions / kill verdicts | Server-issued nonce, client-reported verdicts | Session registry; verdict store (32 per peer, 256 peers, 30 s) | Memory | World switch clears | `CompanionKillVerdictStore.cs:24-25`, `InventoryCompanionBridge.cs:329-350` |

## 7. C6 delivery

| Aspect | Behaviour | Ref |
|---|---|---|
| Outbox bound | 1000 frames, drop oldest, silent | `PendingEventQueue.cs:60-64` |
| Ack v0 | A pong after a checkpoint ping releases frames written before it. This proves receipt, not processing | `PendingEventQueue.cs:126-163`, `TakaroWebSocketRunner.cs:205-220` |
| Ack v1 | Only `eventAck {streamId, upTo}` releases frames. A pong does not (`releaseFrames: protocolVersion == 0`). `eventError` is logged only; Takaro acks it | `TakaroWebSocketRunner.cs:210,646-672`, `PendingEventQueue.cs:94-108` |
| Reconnect replay | Unconfirmed frames are reset and re-sent under the same `streamId`/`seq` after identify | `TakaroWebSocketRunner.cs:335-339,628-636` |
| Dead link | Ping every 10 s; abort if no pong within 15 s. Identify timeout 30 s. Connect and send limited to 15 s / 10 s | `:33-37,225-271` |
| Lifecycle across downtime | Polling runs only while a socket is open. The diff after reconnect emits join/leave with the reconnect timestamp. A join+leave inside the outage is lost | `:375,499-571`, `Events.cs:127-170` |
| Game crash | Outbox, cursor, lifecycle snapshot and inventory cache are all in memory and lost. A new `streamId` starts on the next process | `TakaroWebSocketRunner.cs:31,49` |

## 8. C8 shared vs game-specific

| Component | Copy of? | Diff | Ref |
|---|---|---|---|
| `TakaroWebSocketRunner` (transport and wire client) | **No sibling copy.** C# siblings: 7d2d (websocket-sharp, `WebSocketTransport`/`TransportSession`) and Rust (single-file `ClientWebSocket`). Conan, VEIN, Enshrouded, Dragonwilds, Dune, SCUM and ARK are C++ | Different designs: Rust assembles on `EndOfMessage`, Valheim does not | `games/7d2d/mod/src/WebSocket/`, `games/rust/mod/TakaroConnector.cs:803-830` |
| `ProtocolHandshake` (`WithStreamPosition`, `TryParseEventAck`, `ParseIdentifyResponse`) | **Parallel port** of 7d2d: same logic, System.Text.Json vs Newtonsoft | `diff -q` differs. The public API differs too: Valheim adds `BuildCapabilityManifest`/`IsDeclared`/`ToProtocolErrorCode`; 7d2d has `TryParseEventError` | `$V/Takaro.Valheim.Core/ProtocolHandshake.cs:102,150,159` vs `games/7d2d/mod/src/WebSocket/ProtocolHandshake.cs:147,193,208` |
| `MainThreadActionScheduler`, `PendingEventQueue` | Valheim-only (grep finds no other game) | — | — |
| Companion protocol/session/cache | Game-specific; must stay so | — | `$V/Takaro.Valheim.Companion*` |

Candidate for sharing: a C# "Generic v1 wire client" (frame assembly, classification by parsed `type`, outbox with `eventAck`). It has two real callers, 7d2d and Valheim; Rust is a third if Oxide constraints allow. Shared acceptance fixtures should come first, especially fragmented and oversized frames.

## 9. Existing tests (C4/C6) and gaps

| Test | Covers | Ref |
|---|---|---|
| `BackgroundDispatchWaitsForDrainAndExecutesExactlyOnceOnTheDrainingThread` | Main-thread marshalling | `games/valheim/tests/Takaro.Valheim.Core.Tests/MainThreadActionSchedulerTests.cs:11` |
| `CancellationBeforeDrainCompletesWithoutInvokingTheAction` | Expiry before start | `…/MainThreadActionSchedulerTests.cs:56` |
| `BoundedQueueRejectsOverflowAndDisposeCompletesPendingCallers` | Admission and shutdown | `…/MainThreadActionSchedulerTests.cs:71` |
| `ReachabilityAnswersWhileTheGameLoopIsStalled` | Dispatcher level only | `…/RequestDispatcherTests.cs:11` |
| `WrittenEventsStayQueuedUntilAPongConfirmsThem`, `APongOnlyConfirmsFramesWrittenBeforeItsPing`, `UnconfirmedEventsAreResentAfterAReconnect`, `PendingEventsDropTheOldestWhenFull` | v0 outbox | `…/ServerEventPolicyTests.cs:162,182,199,214` |
| `StreamPositionsAreAssignedOnEnqueueAndSurviveAReconnect`, `OnlyEventAckReleasesFramesOnVersionOne` | v1 cursor | `…/ProtocolV1Tests.cs:231,251` |
| `VersionOneEventsCarryAStreamPositionAndAreResentUnderItAfterAReconnect`, `EventAckReleasesEventsSoTheyAreNotResent` | Runner with an `HttpListener` fake on `ws://`, whole frames only | `…/RunnerProtocolV1Tests.cs:23,48,259-313` |
| `RejectedTokenShowsABannerAndACorrectedTokenSkipsTheBackoff`, `SettingsChangesRacingDisposeAreHarmless` | Reconnect and dispose | `…/RunnerConfigReloadTests.cs:46,121` |
| `SequenceMustIncreaseWithinSession`, `ReconnectReplacesNonceAndRejectsOldSession`, `RemovePeerAndSwitchWorldClearSessionState`, `StaleSnapshotIsAnErrorButAConfirmedEmptySnapshotIsAnEmptyList`, `StaleWorldRememberCannotClearOrOverwriteNewWorldObservation` | Companion freshness, generation, duplicates | `…/CompanionSessionRegistryTests.cs:187,119,381`, `…/InventoryCompanionServerTests.cs:133`, `…/PlayerPositionCacheTests.cs:72` |

**Gaps:**
- No test of fragmented, >32 KiB or split-UTF-8 inbound frames; the fake server always sends `endOfMessage: true` (`RunnerConfigReloadTests.cs:239,260`).
- No test of Close-frame reconnect backoff.
- No test of a slow receiver or send-timeout behaviour.
- No test that a request started after a Takaro-side timeout is not executed or duplicated.
- No test that runner-level reachability and heartbeat survive a stalled main thread.
- No per-frame time-budget assertion for `Drain`.
- No test for growth of the unbounded per-event `Task.Run`.
- No TLS (`wss://`) path in tests.

## 10. Top 3 risks

| # | Risk | Severity | Follow-up |
|---|---|---|---|
| 1 | The receive loop ignores `EndOfMessage` and has a fixed 32 KiB buffer (`TakaroWebSocketRunner.cs:589-598`). Partial or oversized frames cause a connection drop through an uncaught `ParseRequest` exception, or are silently skipped. Unchanged since OLD | **high** | Accumulate frames until `EndOfMessage` into a buffer with an explicit cap (for example 1 MiB → close 1009), decode once, and classify by parsed `type`. Add fragmented, oversized and split-UTF-8 fake-server tests |
| 2 | Inbound request handling is serial and awaits the main thread (`:707`), with no per-request deadline. A stall queues work invisibly in the socket. Requests (including mutations such as `giveItem`) run after Takaro has timed out. Reachability and heartbeat sit behind it (`:557-558`). The 256-slot admission queue never actually applies | **high** | Dispatch requests concurrently into the bounded scheduler with a per-request deadline checked at start (expired → not executed, `timeout` response). Answer ping and reachability from the receive thread. Run the heartbeat on its own timer |
| 3 | Main-thread cost is unbounded per frame: a 64-count drain with no time budget, lifecycle 1+N items every 5 s with `LogInfo` per poll, `KnownPlayerNames` file I/O, a full prefab scan on unknown items or catalog requests, and deserialization of every routed RPC (`MainThreadActionScheduler.cs:41`, `ValheimServerAdapter.cs:87,313-367,625-648`, `KnownPlayerNames.cs:63-73`, `ValheimServerEventBridge.cs:61-66`). The outbox also drops silently (`Dropped` is never logged) | **med** | Add a `Stopwatch` drain budget (as `ValheimWorldMap.cs:21` already does). Collapse the lifecycle poll into one main-thread snapshot (players + positions). Move name persistence off-thread. Cache the prefab index once per world. Log and expose `Dropped` |
