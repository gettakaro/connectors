# Architecture

## Single Plugin Design

Everything lives in one file: `rust/plugin/TakaroConnector.cs`. Carbon compiles it at runtime — no build step.

The plugin uses the `Oxide.Plugins` namespace and inherits `RustPlugin` for compatibility with both Carbon and Oxide frameworks.

## Key Components

| Component | Responsibility |
|-----------|----------------|
| WebSocket lifecycle | `Init()` → connect, `Unload()` → disconnect, reconnect with exponential backoff |
| Event emission | Game hooks → `SendGameEvent()` → WebSocket JSON to Takaro |
| Action handling | WebSocket request → `NextTick()` (main thread) → game API → response |
| Configuration | Environment variables: `TAKARO_WS_URL`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_DEBUG` |

## Event Flow (Game → Takaro)

```
Game Event (e.g. OnPlayerConnected) → Hook method → SendGameEvent() → WebSocket JSON → Takaro Backend
```

Supported events: `player-connected`, `player-disconnected`, `chat-message`, `player-death`, `entity-killed`, `log`

## Action Flow (Takaro → Game)

```
Takaro Request → WebSocket → OnWsMessage() → HandleRequest() → NextTick() (main thread) → ExecuteAction() → SendResponse()
```

16 actions: testReachability, getPlayer, getPlayers, getPlayerLocation, getPlayerInventory, listItems, listEntities, listLocations, executeConsoleCommand, sendMessage, giveItem, teleportPlayer, kickPlayer, banPlayer, unbanPlayer, listBans, shutdown

## Threading Model

- WebSocket runs on a background thread (Task.Run)
- Game API calls must run on Unity's main thread
- `NextTick()` schedules work on the main thread
- Responses are sent back on the WebSocket thread via `Task.Run`

## JSON

Uses `Newtonsoft.Json` (JObject/JArray) — shipped with the Rust server by Facepunch. No external dependencies needed.

## Rust Game APIs Used

| API | Purpose |
|-----|---------|
| `BasePlayer.activePlayerList` | Get online players |
| `BasePlayer.FindByID(steamId)` | Find player by SteamID64 |
| `player.inventory.containerMain/Belt/Wear` | Player inventory |
| `player.transform.position` | Player location |
| `player.Teleport(Vector3)` | Teleport |
| `player.Kick(reason)` | Kick |
| `player.ChatMessage(msg)` | DM a player |
| `ItemManager.itemList` | All item definitions |
| `ItemManager.Create(def, amount)` | Create item instance |
| `ServerUsers.Set/Remove/GetAll` | Ban management |
| `ConsoleSystem.Run()` | Execute console commands |
| `ConsoleNetwork.BroadcastToAllClients()` | Broadcast chat |
| `TerrainMeta.Path.Monuments` | Map landmarks |
