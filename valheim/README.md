# Valheim Takaro Connector

The Valheim connector is a dedicated-server-only BepInEx plugin implementing the Takaro Generic Connector Protocol. Install it only on the Valheim dedicated server. A Takaro client mod, client snapshot bridge, and custom client action RPCs are not supported.

## Project Shape

- `src/Takaro.Valheim.Core` contains the game-independent protocol, configuration, models, and request dispatcher.
- `src/Takaro.Valheim.Plugin` contains the dedicated-server BepInEx adapter.
- `tests/Takaro.Valheim.Core.Tests` contains protocol, behavior, packaging, and capability-registry tests.
- `capabilities.json` is the machine-readable support registry.

## Installation

1. Install BepInExPack Valheim on the dedicated server.
2. Copy the `TakaroValheim` release folder into `BepInEx/plugins/` on that server.
3. Start the server once and edit `BepInEx/config/com.takaro.valheim.cfg`.
4. Set the registration token from the Takaro game-server connector setup.
5. Restart the dedicated server so the connector loads the saved token and configuration.
6. Keep the Valheim client free of `TakaroValheim.dll`.

Never commit registration or identity tokens.

## Runtime Configuration

The plugin reads these BepInEx settings:

- `registrationToken`
- `serverName`
- `identityToken`
- `takaroWsUrl`
- `logLevel`
- `enableLogEvents`
- `commandAllowlistExact`
- `commandAllowlistPrefixes`

The default WebSocket endpoint is `wss://connect.takaro.io/`. The plugin disables itself before Harmony patching or connector startup when it detects a non-dedicated process.

## Capability Status

The registry uses only three statuses:

- `live-supported`: the server-owned path has historical live evidence.
- `schema-fallback`: the connector action/schema is available or live-proven, but Takaro's standard route cannot yet expose it end to end.
- `unsupported`: the path is unavailable or lacks valid server-only proof.

### Actions

| Action | Status | Dedicated-server behavior |
| --- | --- | --- |
| `testReachability` | `live-supported` | Reports connector reachability. |
| `getPlayers` | `live-supported` | Reads the Valheim dedicated-server player list. |
| `getPlayer` | `unsupported` | Filtering exists, but the final Takaro response shape still needs independent live proof. |
| `getPlayerLocation` | `live-supported` | Uses only a real peer/public position or a fresh 30-second server-observed last-known position; an unavailable lookup is rejected through a schema-valid payload error. |
| `getPlayerInventory` | `unsupported` | Remote inventories are client-owned. The connector deliberately sends no response instead of persisting a fabricated empty inventory. |
| `giveItem` | `live-supported` | Creates stack-split world drops near the player's server-known position. |
| `sendMessage` | `live-supported` | Uses Valheim's built-in routed HUD message calls without a custom client RPC. |
| `executeConsoleCommand` | `live-supported` | Runs only exact or prefix-allowlisted commands. |
| `listItems` | `live-supported` | Lists item prefabs visible to the server. |
| `listEntities` | `live-supported` | Lists non-player character prefabs visible to the server. |
| `listLocations` | `schema-fallback` | The official raw Generic Connector action/schema live-returned 11,293 nested `ILocationDTO` objects, but the standard Takaro route at `0c63cf1c` throws `NotImplementedError` before requesting them. |
| `getMapInfo` | `unsupported` | Returns an immediate schema-valid payload error; the dedicated server does not expose client map metadata. |
| `getMapTile` | `unsupported` | Returns an immediate payload error; the dedicated server does not expose rendered client map tiles. |
| `teleportPlayer` | `live-supported` | Routes Valheim's built-in `RPC_TeleportTo` to the server-known character ZDO. |
| `kickPlayer` | `unsupported` | A built-in `Kicked` RPC implementation exists, but exact live support remains unproven and approval-gated. |
| `banPlayer` | `unsupported` | Official ban behavior is implemented, but exact live support remains unproven and approval-gated. |
| `unbanPlayer` | `unsupported` | Official ban-list removal is implemented, but exact live support remains unproven and approval-gated. |
| `listBans` | `live-supported` | Reads Valheim's official ban entries. |
| `shutdown` | `unsupported` | Delayed `Application.Quit()` is implemented, but exact live support remains unproven and approval-gated. |

### Events

| Event | Status | Dedicated-server behavior |
| --- | --- | --- |
| `log` | `live-supported` | Emits connector log events. |
| `player-connected` | `live-supported` | Derived from dedicated-server player snapshots after a real server position is observed; turn-3 Takaro searches persisted both tested connections. |
| `player-disconnected` | `live-supported` | Derived from the same snapshot tracker; turn-3 Takaro searches persisted both tested disconnections. |
| `chat-message` | `unsupported` | Vanilla-client inbound chat has no proven dedicated-server route; tracked in [issue #69](https://github.com/gettakaro/connectors/issues/69). |
| `player-death` | `unsupported` | Routed `OnDeath` payloads are diagnostic-only; packet sender/target identity and actual death state are not server-owned proof. |
| `entity-killed` | `unsupported` | No emitter or death Harmony patch is active; prior proof used rejected client forwarding. |

## Server-Owned Action Semantics

`giveItem` is a world-drop operation, not a private inventory mutation. Other players can collect the spawned objects. The adapter accepts at most 1,000 items and 100 world-drop stacks per request, validates quality, resolves prefab codes or display/name tokens, splits oversized stacks, and returns an error when no server-owned player position is known.

`teleportPlayer` requires a server-known character ZDO ID. It uses Valheim's built-in teleport RPC and returns `character_unavailable` when that identity is missing.

Player location never returns a fabricated origin. A live peer/public observation is cached for 30 seconds so Takaro can enrich a disconnect with the player's real last-known position; the cache is player-keyed, expires, and clears when Valheim replaces its network/world instance. Player-connected emission waits until such a real observation exists. If no current or fresh observation exists, the connector sends the position DTO's required numeric fields plus `payload.error`. At Takaro source commit `0c63cf1c`, the app connector validates that payload and `Generic.requestFromServer` rejects `payload.error` before returning a position. Root-level response metadata is not used by that consumer.

Remote inventory is permanently unsupported at the dedicated-server boundary. Its DTO must be an array, so there is no place for the same payload error. Returning `[]` would falsely persist an empty inventory. The connector therefore sends no response frame, logs that suppression at most once per minute per failure, and relies on Takaro's current 10-second pending-request timeout. This is an explicit compatibility limitation and a deliberate exception to the protocol's normal always-respond guidance.

Other failure-capable actions return immediately. At Takaro source commit `0c63cf1c`, validation-free actions such as `giveItem`, messaging, teleport, moderation, and shutdown accept `{ error: "code: message" }`, which `Generic.requestFromServer` rejects without waiting for a timeout. Validated object actions add only their required DTO fields before the same payload error; `testReachability` instead returns `connectable:false` with an actionable reason because that route bypasses the Generic error check. Array-validated actions cannot carry a top-level JSON error; their ordinary server-owned paths return arrays, and any actual failed array path is suppressed rather than fabricating an empty result.

The connector distinguishes a confirmed empty collection from an unavailable Valheim runtime source. `getPlayers`, `listItems`, `listEntities`, `listLocations`, and `listBans` return `[]` only when their required server singleton and collection exist. During world startup or reload, `runtime_unavailable` is suppressed for these array DTOs and lifecycle polling preserves its prior snapshot instead of fabricating an empty server or a false disconnect. A missing `getPlayer` match returns an immediate `player_not_found` payload error.

Outbound messages and item confirmations use base-game `Message` and `ShowMessage` calls. They do not require a Takaro client plugin and are not treated as inbound chat.

## Evidence Boundary

Historical dedicated-server evidence from June 21-22, 2026 covers several entries marked `live-supported`, including vanilla-client player location, world-drop item delivery, built-in teleport, moderation, and delayed shutdown. The July 10 turn-3 run additionally persisted two complete player connect/disconnect cycles. Turn 4 re-proved a vanilla `Hehe` handshake, real position and teleport, lifecycle persistence, visible messaging/item/cron behavior, and an official raw `listLocations` response containing 11,293 nested locations without any client plugin. The standard Takaro `listLocations` route remained unavailable, so that action is `schema-fallback`, not `live-supported`.

Turn 5 then live-proved immediate invalid-input failures, inventory non-mutation, lifecycle persistence, and the vanilla-client server boundary against its exact commit and artifact hashes. Turn 6 pinned the current exhaustive action surface to an exact deployed artifact and re-proved pre-ready non-fabrication, immediate unsupported map errors, a vanilla connect/disconnect lifecycle, the real `85/36/-2` position across disconnect, and inventory non-mutation. Turn-7 review fixes require fresh branch verification and do not inherit that runtime proof. Current evidence belongs in [the 2026-07-10 server-only validation ledger](qa/2026-07-10-server-only-validation.md). Routed-packet chat/player-death and client-forwarded entity-kill evidence are explicitly excluded from trusted event claims.

## Local Development

Run the reference-free build and tests:

```bash
dotnet test Takaro.Valheim.sln
```

Build the real plugin against dedicated-server references:

```bash
dotnet build src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -f net472 \
  -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath=/path/to/BepInEx/core \
  -p:ValheimReferencePath=/path/to/valheim_server_Data/Managed
```

## Release Build

From `valheim/`:

```bash
./scripts/setup-environment.sh
./scripts/build-release.sh 0.1.0 dist
```

The release artifact is `takaro-valheim-plugin.zip`. It contains the dedicated-server plugin, core library, and required runtime dependencies; host-provided game, Unity, BepInEx, and Harmony assemblies are excluded.

The release version argument must be valid SemVer. It is compiled into BepInEx plugin metadata, exact assembly informational/package metadata, the packaged README, and `manifest.json`; numeric assembly/file metadata uses the corresponding `major.minor.patch.0`. The build generates its compile-time version source under the intermediate output directory and does not edit tracked source files.

Environment setup downloads SteamCMD completely to a unique temporary archive before extraction and removes temporary files on both success and failure. It requires the host `file` utility to identify every required Valheim and BepInEx DLL as a real `PE32 ... Mono/.Net assembly`; marker blobs, empty files, and arbitrary text cannot satisfy validation. Valheim is updated in a sibling staging directory, validated there, and published by directory rename with rollback so an interrupted or failed update cannot leave a partially copied final install.
