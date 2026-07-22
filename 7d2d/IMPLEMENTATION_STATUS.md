# Takaro 7D2D Mod - Implementation Status

## Executive Summary

This document tracks the mod-7d2d implementation against the [official Takaro specification](https://docs.takaro.io/advanced/adding-support-for-a-new-game).

**Qualification status: 5/23 Coverage Cells are Live-supported.**

## 📊 Quick Status Overview

| Category | Status | Progress |
|----------|--------|----------|
| Contract/build foundation | Locally verified | 251 contract assertions; V3 Release build |
| Function source paths | 17 implemented | Five Live-supported; one Schema-fallback; eleven unqualified |
| Function placeholders | 0 stubs | Entity and placed-POI catalogues are mirror-backed |
| Event source paths | 6 implemented, unproven | Live proof required |
| Live-supported | 5/23 | Four read cells plus `giveItem`; no connector-wide promotion claim |

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
restarts: reads only serve online players, while items, entities, locations,
and bans are reseeded at `GameStartDone`. (The memory backend also sidesteps
LiteDB 5 disk-engine
failures under Mono — "ReadFull must read PAGE_SIZE bytes" during
WAL/checkpoint.) Seeding happens *before* the WebSocket connects, so requests
never observe a cold mirror.

### Per-endpoint data flow

| Action | Served from | Updated by | Staleness bound |
|---|---|---|---|
| `testReachability` | cached game-ready lifecycle state | game start/shutdown | lifecycle-bound; Live-supported |
| `getPlayers`, `getPlayer` | `players` collection (Online=true) | spawn/disconnect events; sampler refreshes ping | identity exact; ping ≤3s; `getPlayers` Live-supported, `getPlayer` direct-live only |
| `getPlayerLocation` | `players.X/Y/Z` | PositionSampler (~3s) | ≤3s; direct-live only |
| `getPlayerInventory` | `inventories` collection | join + `ModEvents.SavePlayerData` | client playerdata sync interval (~30s); direct-live only |
| `listItems` | `items` collection | seeded once at GameStartDone (static) | 0; Live-supported |
| `listEntities` | `entities` collection | seeded from `EntityClass.list.Dict` at GameStartDone | 0; Live-supported |
| `listLocations` | `locations` collection | seeded from placed `DynamicPrefabDecorator` POIs at GameStartDone | 0; direct-live proven, Takaro Schema-fallback |
| `listBans` | `bans` collection | seed; refreshed after Takaro ban/unban; 60s resync catches console bans | ≤60s for console-issued bans |
| `giveItem` | first-party player-proximate world drop inside a main-thread dispatcher closure | — | Live-supported |
| `sendMessage`, `kickPlayer`, `banPlayer`, `unbanPlayer`, `teleportPlayer` | game API calls inside a main-thread dispatcher closure | — | unproven |
| `executeConsoleCommand`, `shutdown` | `SdtdConsole.ExecuteAsync` (async by design) | — | unproven |

---

## Function source classification

Read requests (mirror-backed, `src/WebSocket/ReadHandlers.cs`):

- **`testReachability`** — Live-supported; reports cached game lifecycle readiness
- **`getPlayers`** — Live-supported; online players from the mirror
- **`getPlayer`** — implemented and direct-live proven; no Takaro-owned caller proof
- **`getPlayerLocation`** — implemented and direct-live proven; no Takaro-owned caller proof
- **`getPlayerInventory`** — implemented and direct-live proven; no Takaro-owned caller proof
- **`listItems`** — Live-supported; localized item catalogue
- **`listEntities`** — Live-supported; spawnable living non-player entity catalogue
- **`listLocations`** — Schema-fallback; placed POI catalogue is directly
  live-proven, but Takaro core has no callable Generic `listLocations` path
- **`listBans`** — implemented, unproven; merged timed and permanent ban sources

Action requests (main-thread dispatched, `src/WebSocket/ActionHandlers.cs`):

- **`giveItem`** — Live-supported; combined direct and exact-current-build proof
  covers the Takaro-owned first-party world drop, exact vanilla pickup
  quantity, correlated readback, and full-inventory refusal on V3.0.1
- **`sendMessage`** — implemented, unproven; global and recipient branches
- **`executeConsoleCommand`** — implemented, unproven; asynchronous result capture
- **`kickPlayer`** — implemented, unproven; optional reason
- **`banPlayer`** — implemented, unproven; timed/permanent paths and mirror refresh
- **`unbanPlayer`** — implemented, unproven; permanent/timed paths and mirror refresh
- **`teleportPlayer`** — implemented, unproven; world-bounds clamping
- **`shutdown`** — implemented, unproven; null response payload

The curated runtime proofs and review history are recorded in the incubation
workspace through El-Limon/gamingconnectors PRs #14, #16, #19, and #21. The
Takaro platform dependency for `listLocations` is tracked in El-Limon issue
#18.

---

## Event source classification

All published via `src/WebSocket/GameEventPublisher.cs` (non-blocking outbound queue):

- **`player-connected`** — implemented, unproven; `PlayerSpawnedInWorld`
- **`player-disconnected`** — implemented, unproven; excludes shutdown disconnects
- **`chat-message`** — implemented, unproven; Harmony network-package patch
- **`entity-killed`** — implemented, unproven; non-player kills
- **`player-death`** — implemented, unproven; death and attacker data
- **`log`** — implemented, unproven; Unity log stream with feedback filtering

---

## 📁 File Structure

```
src/
├── API.cs                      # Mod entry point: ModEvents wiring, Harmony chat patch
├── Shared.cs                   # Player/item/ban/entity/location DTOs and transforms
├── CommandResult.cs            # IConsoleConnection → TaskCompletionSource bridge
├── ServiceRegistry.cs          # Ordered service init/destroy
├── Interfaces/IService.cs
├── Commands/Debug.cs           # takaro-debug console command
├── Config/ConfigManager.cs     # Config.xml management
├── Persistence/
│   ├── Database.cs             # In-memory LiteDB instance, collections, access lock
│   └── Records.cs              # Player, inventory, ban, item, entity, location records
├── Services/
│   ├── LogService.cs           # File + console logging
│   ├── DbWriter.cs             # Background writer thread (all DB writes)
│   ├── MainThreadDispatcher.cs # WS thread → game main thread marshalling
│   ├── PlayerProximateItemDelivery.cs # First-party replicated item drops
│   ├── StateMirror.cs          # Mirror reads (WS thread) + snapshot writes (main thread)
│   └── PositionSampler.cs      # ~3s position/ping sampling + 60s ban resync
└── WebSocket/
    ├── WebSocketTransport.cs   # Connection, identify, heartbeat, reconnect, send queue
    ├── RequestRouter.cs        # Message parsing, dispatch, error boundary
    ├── ReadHandlers.cs         # Mirror-backed read requests
    ├── ActionHandlers.cs       # Main-thread-dispatched action requests
    ├── GiveItemHandler.cs      # Validated item delivery and response cardinality
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
