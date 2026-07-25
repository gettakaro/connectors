# Takaro 7D2D Mod - Implementation Status

> **Qualification status:** Source presence is classified separately from
> runtime support. All twenty-three Coverage Cells are Live-supported. The
> V3.1.0 forward-compatibility refresh includes a vanilla client and the
> generated Takaro `listLocations` route.

## Executive Summary

This document tracks the mod-7d2d implementation against the [official Takaro specification](https://docs.takaro.io/advanced/adding-support-for-a-new-game).

**Qualification status: 23/23 Coverage Cells are Live-supported.**

## 📊 Quick Status Overview

| Category | Status | Progress |
|----------|--------|----------|
| Contract/build foundation | Locally verified | 329 contract assertions; V3 Release build |
| Function source paths | 17 implemented | Seventeen Live-supported |
| Function placeholders | 0 stubs | Entity and placed-POI catalogues are mirror-backed |
| Event source paths | 6 implemented | Six Live-supported |
| Live-supported | 23/23 | Seventeen functions and six events |

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
restarts: ordinary player reads serve only online players, while items,
entities, locations, and bans are reseeded at `GameStartDone`. The location
read alone retains the last sample for a bounded 30 seconds after disconnect
so Takaro can enrich the lifecycle event. (The memory backend also sidesteps
LiteDB 5 disk-engine
failures under Mono — "ReadFull must read PAGE_SIZE bytes" during
WAL/checkpoint.) Seeding happens *before* the WebSocket connects, so requests
never observe a cold mirror.

### Per-endpoint data flow

| Action | Served from | Updated by | Staleness bound |
|---|---|---|---|
| `testReachability` | cached game-ready lifecycle state | game start/shutdown | lifecycle-bound; Live-supported |
| `getPlayers`, `getPlayer` | `players` collection (Online=true) | spawn/disconnect events; sampler refreshes ping | identity exact; ping ≤3s; Live-supported |
| `getPlayerLocation` | `players.X/Y/Z` | PositionSampler (~3s); bounded disconnect snapshot | ≤3s online; ≤30s after disconnect for Takaro enrichment; Live-supported |
| `getPlayerInventory` | `inventories` collection | join + `ModEvents.SavePlayerData` | client playerdata sync interval (~30s); Live-supported through Takaro inventory sync |
| `listItems` | `items` collection | seeded once at GameStartDone (static) | 0; Live-supported |
| `listEntities` | `entities` collection | seeded from `EntityClass.list.Dict` at GameStartDone | 0; Live-supported |
| `listLocations` | `locations` collection | seeded from placed `DynamicPrefabDecorator` POIs at GameStartDone | 0; Live-supported through the generated Takaro route |
| `listBans` | `bans` collection | seed; refreshed after Takaro ban/unban; 60s resync catches console bans | ≤60s for console-issued bans; timed path Live-supported |
| `giveItem` | first-party player-proximate world drop inside a main-thread dispatcher closure | — | Live-supported |
| `banPlayer`, `unbanPlayer` | game API calls inside a main-thread dispatcher closure | — | timed and permanent paths Live-supported |
| `sendMessage`, `kickPlayer`, `teleportPlayer` | game API calls inside a main-thread dispatcher closure | — | Live-supported |
| `executeConsoleCommand` | `SdtdConsole.ExecuteAsync` plus native-result classifier | — | Live-supported for valid, unknown, and invalid-argument paths |
| `shutdown` | `SdtdConsole.ExecuteAsync` (async by design) | — | Live-supported |

---

## Function source classification

Read requests (mirror-backed, `src/WebSocket/ReadHandlers.cs`):

- **`testReachability`** — Live-supported; reports cached game lifecycle readiness
- **`getPlayers`** — Live-supported; online players from the mirror
- **`getPlayer`** — Live-supported; recipient-scoped message metadata proves the Takaro-owned caller and DTO validation path
- **`getPlayerLocation`** — Live-supported; a unique move and restoration were persisted by Takaro player sync
- **`getPlayerInventory`** — Live-supported; post-deployment validated observations were persisted by Takaro inventory sync
- **`listItems`** — Live-supported; localized item catalogue
- **`listEntities`** — Live-supported; spawnable living non-player entity catalogue
- **`listLocations`** — Live-supported; the generated Takaro API route returned
  368 validated placed POIs while an unmodified V3.1.0 client was online
- **`listBans`** — Live-supported for the timed-ban path; merged timed and permanent ban sources

Action requests (main-thread dispatched, `src/WebSocket/ActionHandlers.cs`):

- **`giveItem`** — Live-supported; combined direct and exact-current-build proof
  covers the Takaro-owned first-party world drop, exact vanilla pickup
  quantity, correlated readback, and full-inventory refusal on V3.0.1
- **`sendMessage`** — Live-supported; global and recipient branches are visible in the vanilla client
- **`executeConsoleCommand`** — Live-supported; valid output remains successful,
  while unknown commands and V3 invalid-argument output preserve `rawResult`
  and return a bounded failure
- **`kickPlayer`** — Live-supported; the optional reason is visible in the vanilla client and the player leaves game state
- **`banPlayer`** — Live-supported for timed and permanent paths; UTC input is converted to the game-local deadline and persistence is verified before kick
- **`unbanPlayer`** — Live-supported for timed and permanent paths; game-owned removal, empty Takaro list, and successful vanilla reconnect proven
- **`teleportPlayer`** — Live-supported; spawned-state rejection, bounded movement, and exact restoration are client-log proven
- **`shutdown`** — Live-supported; Takaro success, native save/cleanup, normal WebSocket close, and actual process exit proven

The complete live-proof record is maintained in the El-Limon/gamingconnectors
incubation workspace. The callable platform route is pending review in
[`gettakaro/takaro` PR #3093](https://github.com/gettakaro/takaro/pull/3093);
production installations require that platform change to be merged and
deployed.

---

## Event source classification

All published via `src/WebSocket/GameEventPublisher.cs` (non-blocking outbound queue):

- **`player-connected`** — Live-supported; native spawn and Takaro event are correlated
- **`player-disconnected`** — Live-supported; stable identity snapshot, bounded
  post-disconnect location enrichment, exact single Takaro event, and zero
  online-player readback; shutdown disconnects remain excluded
- **`chat-message`** — Live-supported; unique vanilla-client message is correlated through native and Takaro event timestamps
- **`entity-killed`** — Live-supported; real vanilla-client non-player kill with
  player attribution and non-empty held weapon
- **`player-death`** — Live-supported; real vanilla-client fall death with
  linked player and position
- **`log`** — Live-supported; native server stream plus bounded one-shot
  server-message echo suppression proven against a temporary Takaro hook

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
│   ├── BanExpiry.cs             # UTC ↔ game-local timed-ban conversion
│   ├── ConsoleCommandOutcome.cs # native console failure classification
│   ├── ProtocolDiagnostics.cs   # bounded Takaro error diagnostics
│   ├── PlayerLocationReadWindow.cs # bounded post-disconnect location access
│   ├── ServerMessageEchoGuard.cs # bounded one-shot native chat echo guard
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
