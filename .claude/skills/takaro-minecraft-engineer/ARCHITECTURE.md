# Architecture

## Adapter Pattern

The core defines a `GameAdapter` interface with 30+ methods across three categories:

- **Logging**: logInfo, logWarning, logDebug
- **Queries**: getPlayer, getPlayers, getPlayerLocation, getPlayerInventory, listItems, listEntities, listLocations
- **Actions**: giveItem, sendMessage, executeConsoleCommand, teleportPlayer, kickPlayer, banPlayer, unbanPlayer, listBans, shutdown

Each platform (Paper, NeoForge, Fabric) implements this interface using platform-specific APIs.

## Key Classes

### Core Module

| Class | Responsibility |
|-------|----------------|
| `GameAdapter` | Interface defining all game server capabilities |
| `TakaroConnector` | Orchestrator — wires WebSocket client to adapter and config |
| `TakaroWebSocketClient` | WebSocket client handling auth, request parsing, response sending; implements `EventEmitter` |
| `EventEmitter` | Interface for emitting game events to Takaro |
| `TakaroConfig` | Configuration with env var override support |
| `Model classes` | `PlayerInfo`, `PlayerLocation`, `InventoryItem`, `GameItem`, `GameEntity`, `GameLocation`, `BanEntry`, `CommandResult` |

### Platform Modules

| Module | Entry Point | Adapter | Event Listener |
|--------|-------------|---------|----------------|
| Paper | `TakaroPaperPlugin` (extends `JavaPlugin`) | Same class | `TakaroPaperEventListener` |
| NeoForge | `TakaroNeoForgeMod` (`@Mod`) | `NeoForgeGameAdapter` | Inline event handlers |
| Fabric | `TakaroFabricMod` (`DedicatedServerModInitializer`) | `FabricGameAdapter` | Fabric callback registration |

## Event Flow (Game → Takaro)

```
Game Event → Platform Listener → EventEmitter method → WebSocket JSON → Takaro Backend
```

Supported events: `player-connected`, `player-disconnected`, `chat-message`, `player-death`, `entity-killed`, `log`

## Action Flow (Takaro → Game)

```
Takaro Request → WebSocket → TakaroWebSocketClient.onMessage() → GameAdapter method (on main thread) → Response
```

16 actions supported. All adapter calls run on the platform's main thread via `runOnMainThread()`.

## Adding a New Platform

1. Create new module directory with `build.gradle.kts` depending on `:core`
2. Implement `GameAdapter` interface (30+ methods)
3. Create event listener translating platform events to `EventEmitter` calls
4. Handle configuration (platform-specific format)
5. Use Shadow plugin to relocate dependencies to `io.takaro.libs.*`
6. Add to `settings.gradle.kts`

## Dependencies

- **java-websocket** — WebSocket client
- **gson** — JSON serialization
- **log4j-api** — Logging (compile-only, provided by platforms)
- All dependencies are Shadow-relocated to avoid conflicts with platform jars
