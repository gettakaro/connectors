# Takaro 7D2D Mod - Implementation Status

## Executive Summary

This document tracks the mod-7d2d implementation against the [official Takaro specification](https://docs.takaro.io/advanced/adding-support-for-a-new-game).

**Implementation Completeness: 100% (15/15 core functions + 6/6 events)**

## 📊 Quick Status Overview

| Category | Status | Progress |
|----------|--------|----------|
| Core Functions | 15/15 Complete | 100% ✅ |
| Game Events | 6/6 Complete | 100% ✅ |
| Infrastructure | Complete | 100% ✅ |
| Future Features (`listEntities`, `listLocations`) | 0/2 Complete | 🔮 |

---

## 🏗️ Architecture

The mod is built around an **event-driven state mirror** so that Takaro can
hammer the connector with read requests without touching the game simulation.

```
        Game main thread                              Background threads
┌─────────────────────────────────────┐   ┌────────────────────────────────────────┐
│ ModEvents handlers + Harmony chat   │   │ WebSocketTransport (websocket-sharp)   │
│ ModEvents.GameUpdate ──► dispatcher │   │   recv → RequestRouter                 │
│ pump + PositionSampler (~3s)        │   │   ReadHandlers  → LiteDB (mirror)      │
│ cheap POCO snapshots → enqueue ─────┼──►│   ActionHandlers→ MainThreadDispatcher │
└─────────────────────────────────────┘   │                  (await TCS)           │
                                          │ DbWriter thread → LiteDatabase         │
                                          └────────────────────────────────────────┘
```

**Threading invariants:**

1. The WebSocket thread never touches game APIs. Read requests are answered
   from the LiteDB mirror; action requests are marshalled onto the game main
   thread via `MainThreadDispatcher` and awaited.
2. The game main thread never does DB I/O. Event handlers and the sampler
   capture plain POCO snapshots and enqueue them; the `DbWriter` background
   thread performs all LiteDB writes.
3. One shared **in-memory** `LiteDatabase` (LiteDB 5, `:memory:`): the
   DbWriter thread writes, the WebSocket thread reads. All access holds
   `Database.SyncRoot` — LiteDB's engine is not reliably safe under concurrent
   reads and writes, and both sides run off the game thread so the lock never
   blocks the game.

**Mirror lifecycle:** the database is **in-memory and ephemeral** — rebuilt
from game truth on every boot. Nothing in the mirror has value across
restarts: reads only serve online players, and items/bans are reseeded at
`GameStartDone`. (The memory backend also sidesteps LiteDB 5 disk-engine
failures under Mono — "ReadFull must read PAGE_SIZE bytes" during
WAL/checkpoint.) Seeding happens *before* the WebSocket connects, so requests
never observe a cold mirror.

### Per-endpoint data flow

| Action | Served from | Updated by | Staleness bound |
|---|---|---|---|
| `testReachability` | constant | — | 0 |
| `getPlayers`, `getPlayer` | `players` collection (Online=true) | spawn/disconnect events; sampler refreshes ping | identity exact; ping ≤3s |
| `getPlayerLocation` | `players.X/Y/Z` | PositionSampler (~3s) | ≤3s |
| `getPlayerInventory` | `inventories` collection | join + `ModEvents.SavePlayerData` | client playerdata sync interval (~30s) — identical to a live read of `cInfo.latestPlayerData`, which is also just the last-received PlayerDataFile |
| `listItems` | `items` collection | seeded once at GameStartDone (static) | 0 |
| `listBans` | `bans` collection | seed; refreshed after Takaro ban/unban; 60s resync catches console bans | ≤60s for console-issued bans |
| `giveItem`, `sendMessage`, `kickPlayer`, `banPlayer`, `unbanPlayer`, `teleportPlayer` | live game APIs inside a main-thread dispatcher closure | — | live |
| `executeConsoleCommand`, `shutdown` | `SdtdConsole.ExecuteAsync` (async by design) | — | live |

---

## 🔧 Core Functions (15/15)

Read requests (mirror-backed, `src/WebSocket/ReadHandlers.cs`):

- **`testReachability`** ✅ — returns `connectable: true`
- **`getPlayers`** ✅ — online players from the mirror
- **`getPlayer`** ✅ — single player lookup by gameId
- **`getPlayerLocation`** ✅ — last sampled position, integer coordinates
- **`getPlayerInventory`** ✅ — inventory/bag/equipment from the last received PlayerDataFile
- **`listItems`** ✅ — full item catalog with localized names/descriptions
- **`listBans`** ✅ — merged AdminTools.Blacklist (timed bans w/ reason+expiry) and Platform.BlockedPlayerList (permanent blocks)

Action requests (main-thread dispatched, `src/WebSocket/ActionHandlers.cs`):

- **`giveItem`** ✅ — quality support, player state validation (spawned, alive)
- **`sendMessage`** ✅ — global and whisper modes
- **`executeConsoleCommand`** ✅ — async execution with result capture
- **`kickPlayer`** ✅ — `GameUtils.KickPlayerForClientInfo` with optional reason
- **`banPlayer`** ✅ — timed bans via AdminTools.Blacklist, permanent via BlockedPlayerList; kicks online players; refreshes the bans mirror
- **`unbanPlayer`** ✅ — BlockedPlayerList first, AdminTools fallback; refreshes the bans mirror
- **`teleportPlayer`** ✅ — NetPackageTeleportPlayer with world-bounds clamping
- **`shutdown`** ✅ — vanilla `shutdown` command, null payload per spec

### 🔮 Future enhancements

- **`listEntities`** — nearby entities; not critical for server management
- **`listLocations`** — named locations/landmarks

---

## 📡 Game Events (6/6)

All published via `src/WebSocket/GameEventPublisher.cs` (non-blocking outbound queue):

- **`player-connected`** ✅ — `PlayerSpawnedInWorld` (join/enter types)
- **`player-disconnected`** ✅ — `PlayerDisconnected`, excludes shutdown disconnects
- **`chat-message`** ✅ — Harmony patch on `NetPackageChat.ProcessPackage`; global/whisper/friends/team channels
- **`entity-killed`** ✅ — non-player kills with entity type and weapon when available
- **`player-death`** ✅ — death position and attacker info when available
- **`log`** ✅ — Unity `Application.logMessageReceived`, filtered against feedback loops

---

## 📁 File Structure

```
src/
├── API.cs                      # Mod entry point: ModEvents wiring, Harmony chat patch
├── Shared.cs                   # DTOs (TakaroPlayer/Item/Ban) and transforms
├── CommandResult.cs            # IConsoleConnection → TaskCompletionSource bridge
├── ServiceRegistry.cs          # Ordered service init/destroy
├── Interfaces/IService.cs
├── Commands/Debug.cs           # takaro-debug console command
├── Config/ConfigManager.cs     # Config.xml management
├── Persistence/
│   ├── Database.cs             # In-memory LiteDB instance, collections, access lock
│   └── Records.cs              # PlayerRecord, InventoryRecord, BanRecord, ItemRecord
├── Services/
│   ├── LogService.cs           # File + console logging
│   ├── DbWriter.cs             # Background writer thread (all DB writes)
│   ├── MainThreadDispatcher.cs # WS thread → game main thread marshalling
│   ├── StateMirror.cs          # Mirror reads (WS thread) + snapshot writes (main thread)
│   └── PositionSampler.cs      # ~3s position/ping sampling + 60s ban resync
└── WebSocket/
    ├── WebSocketTransport.cs   # Connection, identify, heartbeat, reconnect, send queue
    ├── RequestRouter.cs        # Message parsing, dispatch, error boundary
    ├── ReadHandlers.cs         # Mirror-backed read requests
    ├── ActionHandlers.cs       # Main-thread-dispatched action requests
    ├── GameEventPublisher.cs   # Game event publishing
    └── WebSocketMessage.cs     # Message envelope
```

---

## 🔍 Key Implementation Details

### Player Identification
- Uses EOS CrossplatformId as primary gameId (`EOS_` prefix stripped)
- Supports Steam (`Steam_`), Xbox (`XBL_`) platform IDs

### Connection & Authentication
- WebSocket endpoint from `Config.xml` (default `wss://connect.takaro.io/`)
- Identity + registration token system; identity token auto-generated (UUID)
- Reconnect with exponential backoff (cap 300s); 30s heartbeat
- Outbound messages drained by a dedicated sender thread — event publishing
  from the game thread never blocks on socket I/O

### Dependencies
- `websocket-sharp` (built from source) and `LiteDB 5.0.21` (NuGet, net45
  target) are fetched by the `deps` docker-compose service into
  `_data/7dtd-binaries/` and ship in the mod folder alongside `Takaro.dll`
