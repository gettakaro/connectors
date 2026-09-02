# Valheim Takaro Connector

The Valheim integration has two owned, role-specific BepInEx packages: the Takaro connector runs only on the dedicated server, while the optional or required companion runs only in the graphical client and reports client-owned gameplay observations to that server. The server plugin still refuses graphical-client processes, and the companion refuses dedicated-server/batch processes.

The companion's client-reported inventory, chat, death, and attributed-kill paths are `live-supported` by exact server, graphical-client, and Takaro proof. See [COMPANION.md](COMPANION.md) for its trust boundary and operational guide, and [the owned-companion validation ledger](qa/2026-07-12-owned-companion-validation.md) for the evidence.

## Quick Start

Three decisions, in order:

1. **Install the server package.** Every capability in the "server alone" column below
   works from this step on. Put `TakaroValheim` under `BepInEx/plugins/` on the dedicated
   server, set the registration token in `BepInEx/config/com.takaro.valheim.cfg`, restart.
2. **Choose `companionMode`.** It decides what happens to players who do not install the
   client companion — see [Choosing a companion mode](#choosing-a-companion-mode). The
   default is `required`; switch to `optional` unless you can put the companion on every
   player's client.
3. **Distribute the companion** (`TakaroValheimCompanion`) to players if you chose
   `optional` or `required`. It carries no token or credential; players drop it under
   their own `BepInEx/plugins/`.

**Upgrading:** the server package and the companion share a wire protocol (currently
**2**) and must be upgraded together. A companion on an older protocol cannot read the
new server's hello, so under `required` it is kicked and logged as
`reason=MissingCompanion … older than protocol 2`; under `optional` it simply stops
reporting. Ship the companion update to players first, or run `optional` for one release.

## What You Need

Valheim has no RCON and no remote admin API, so the connector is a BepInEx plugin that
runs **inside the dedicated server process**. Some of what Takaro wants to know is not
visible to that process at all, which is why there is a second, optional package for the
graphical client.

### On the dedicated server (always required)

- BepInExPack Valheim.
- The `TakaroValheim` server package.
- A Takaro registration token, set in `BepInEx/config/com.takaro.valheim.cfg`.
- Outbound access to `wss://connect.takaro.io/`. The connector dials out; nothing needs
  to be port-forwarded to it.

### On each player's Valheim client (only for client-owned data)

- BepInExPack Valheim.
- The `TakaroValheimCompanion` package.
- No token, and no Takaro credential of any kind.

### Which half provides what

What each Takaro capability does with the server package alone, and what changes when the
player's client runs the companion. Status is the `capabilities.json` value; every
`live-supported` row has a dated ledger under `qa/`.

| Capability | Server package alone | With the companion on that player's client | Status |
| --- | --- | --- | --- |
| `testReachability`, `getPlayers` | Works | — | `live-supported` |
| `getPlayerLocation` | Works (server-known position, never fabricated) | — | `live-supported` |
| `teleportPlayer` | Works (built-in `RPC_TeleportTo`) | — | `live-supported` |
| `kickPlayer`, `banPlayer`, `unbanPlayer`, `listBans` | Work. Ban reason is **not** stored (Valheim's ban list holds ids only) | — | `live-supported` |
| `shutdown` | Works; responds first, then quits cleanly | — | `live-supported` |
| `executeConsoleCommand` | Works for allowlisted commands only | — | `live-supported` |
| `listItems`, `listEntities` | Work | — | `live-supported` |
| `giveItem` (and every shop delivery) | **World drop** at the player's feet — lootable by others, losable while falling or swimming | **Lands in the inventory.** Merges into existing stacks; a full bag drops only the shortfall with a chat notice | `live-supported` |
| `getPlayerInventory` | Not available — the server never fabricates `[]` | Client-reported snapshots (untrusted for security or economy decisions) | `live-supported` |
| `sendMessage` | Returns `companion_server_chat_unavailable` | Rendered into the player's normal chat | `live-supported` |
| `chat-message` event | Not observed (Valheim chat never reaches the server) | Reported by the companion, bound to the real peer | `live-supported` |
| `player-death`, `entity-killed` events | Not emitted (routed identity is not server-owned) | Reported by the companion | `live-supported` |
| `player-connected`, `player-disconnected`, `log` events | Work | — | `live-supported` |
| `getPlayer` | Implemented; Takaro exposes no route to prove it | — | `unsupported` |
| `listLocations` | Implemented; Takaro's standard route throws `NotImplementedError` | — | `schema-fallback` |
| `getMapInfo`, `getMapTile` | Not possible from a dedicated server | — | `unsupported` |

Client-reported data (the companion column) enriches automation but is **not** authoritative
identity, anti-cheat, security, economy, or moderation evidence — see
[COMPANION.md](COMPANION.md).

Two entries deserve their reasons stated plainly, because no amount of server-side work
removes them:

- **Inventory is client-owned.** Valheim loads a player's inventory from their
  `PlayerProfile` into the client's own `Player`/`Humanoid` object. A dedicated server
  holds no inventory state for a remote player, so there is nothing for it to read.
- **Normal chat never reaches the server.** Valheim targets chat at character *owners*
  drawn from `ZNet.GetPlayerList()`. A player's own line can stay entirely client-local,
  so the dedicated server observes no chat RPC to forward.

`sendMessage` needs the companion for a related reason: outbound messages are
rendered into Valheim's real chat history by the companion. Without one, the connector
returns `companion_server_chat_unavailable` rather than silently doing nothing.

### Choosing a companion mode

`companionMode` in the server config decides what happens to a player who has no
companion:

| Mode | Vanilla clients | What you give up |
| --- | --- | --- |
| `disabled` | Join normally; the companion RPC is never registered | Everything in the companion column of the matrix above |
| `optional` | Join normally and stay connected | Client-owned data only from players who installed the companion |
| `required` (default) | Disconnected after a 30-second grace period, with a visible explanation | Nothing, but every player must install the companion |

Pick `optional` if you want Takaro's server-owned features for everyone and the richer
data from whoever opts in. Pick `required` only if you can actually distribute the
companion to your players. Pick `disabled` if you want a pure server-side integration
and accept the reduced surface.

## Project Shape

- `src/Takaro.Valheim.Core` contains the game-independent protocol, configuration, models, and request dispatcher.
- `src/Takaro.Valheim.Companion.Protocol` contains the bounded shared wire contract.
- `src/Takaro.Valheim.Plugin` contains the dedicated-server BepInEx adapter.
- `src/Takaro.Valheim.Companion` contains the owned graphical-client BepInEx companion. It references only the protocol project and has no Takaro cloud transport.
- `tests/Takaro.Valheim.Core.Tests` contains protocol, behavior, packaging, and capability-registry tests.
- `capabilities.json` is the machine-readable support registry.

## Install

Use the two role-specific archives; never mix their DLLs.

### Dedicated server

1. Install BepInExPack Valheim on the dedicated server.
2. Extract `takaro-valheim-plugin.zip` and copy `TakaroValheim` into `BepInEx/plugins/` on that server.
3. Start the server once and edit `BepInEx/config/com.takaro.valheim.cfg`.
4. Set the registration token from the Takaro game-server connector setup.
5. Choose `companionMode=disabled|optional|required`.
6. Restart the dedicated server so the connector loads the saved configuration.

### Graphical client

1. Install BepInExPack Valheim in the graphical client.
2. Extract `takaro-valheim-companion.zip` and copy `TakaroValheimCompanion` to `BepInEx/plugins/TakaroValheimCompanion`.
3. Restart Valheim. No Takaro token or cloud credential belongs in the client.

Upgrade and Remove/rollback instructions are in [COMPANION.md](COMPANION.md). Never copy `TakaroValheim.dll` into the client or `Takaro.Valheim.Companion.dll` into the dedicated server.

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
- `companionMode` (default `required`)

The graphical client companion separately reads `companionCommandPrefixes` (default `$`) from `com.takaro.valheim.companion.cfg`. This is intentionally client-side; the server cannot change a client's local chat interception policy.

The default WebSocket endpoint is `wss://connect.takaro.io/`. The plugin disables itself before Harmony patching or connector startup when it detects a non-dedicated process.

## Capability Status

The support registry continues to use only three statuses, independently of its ownership/source metadata:

- `live-supported`: the path has valid historical live evidence.
- `schema-fallback`: the connector action/schema is available or live-proven, but Takaro's standard route cannot yet expose it end to end.
- `unsupported`: the path is unavailable or still lacks exact live proof.

Ownership values are `server-owned`, `client-reported`, `upstream-blocked`, or `unsupported`. Client-reported data is untrusted and must not be used as authoritative identity, security, anti-cheat, economy, or moderation evidence.

### Actions

| Action | Status | Source and behavior |
| --- | --- | --- |
| `testReachability` | `live-supported` | Reports connector reachability. |
| `getPlayers` | `live-supported` | Reads the Valheim dedicated-server player list. |
| `getPlayer` | `unsupported` | Filtering exists, but the final Takaro response shape still needs independent live proof. |
| `getPlayerLocation` | `live-supported` | Uses only a real peer/public position or a fresh 30-second server-observed last-known position; an unavailable lookup is rejected through a schema-valid payload error. |
| `getPlayerInventory` | `live-supported` | A negotiated companion provides bounded canonical client-reported snapshots, including a confirmed empty inventory. Exact live proof observed repeated successful Takaro polls and a Wood change from 13 to 14; without a companion the server never fabricates `[]`. |
| `giveItem` | `live-supported` | Delivers into the player's inventory through a negotiated companion, dropping only what does not fit at their feet. Without a companion it falls back to stack-split world drops near the player's server-known position. See Server-Owned Action Semantics. |
| `sendMessage` | `live-supported` | Routes only through an active negotiated companion into the normal Valheim chat history. A July 14 live Takaro request reached one compatible peer and rendered an explicit `opts.senderNameOverride` of `con`; a missing or blank value displays as `Takaro`. |
| `executeConsoleCommand` | `live-supported` | Runs only exact or prefix-allowlisted commands. |
| `listItems` | `live-supported` | Lists item prefabs visible to the server. |
| `listEntities` | `live-supported` | Lists non-player character prefabs visible to the server. |
| `listLocations` | `schema-fallback` | The official raw Generic Connector action/schema live-returned 11,293 nested `ILocationDTO` objects, but the standard Takaro route at `0c63cf1c` throws `NotImplementedError` before requesting them. |
| `getMapInfo` | `unsupported` | Returns an immediate schema-valid payload error; the dedicated server does not expose client map metadata. |
| `getMapTile` | `unsupported` | Returns an immediate payload error; the dedicated server does not expose rendered client map tiles. |
| `teleportPlayer` | `live-supported` | Routes Valheim's built-in `RPC_TeleportTo` to the server-known character ZDO. |
| `kickPlayer` | `live-supported` | Sends Valheim's built-in `Kicked` RPC and logs the supplied reason. It never calls `ZNet.Disconnect(peer)` directly, which is what previously crashed the headless server. |
| `banPlayer` | `live-supported` | Writes the player identifier into Valheim's official ban list and disconnects them with the built-in `Kicked` RPC, which does not crash the headless server. **The ban reason is discarded**: Valheim's ban list stores only one identifier per line, so a reason supplied by Takaro is read back as `""`. |
| `unbanPlayer` | `live-supported` | Removes the identifier from Valheim's official ban list; `listBans` then returns an empty array. |
| `listBans` | `live-supported` | Reads Valheim's official ban entries. |
| `shutdown` | `live-supported` | Writes its success response before quitting, then shuts down on Unity's main thread. Valheim performs a clean `ZNet` shutdown and the server process exits. |

### Events

| Event | Status | Source and behavior |
| --- | --- | --- |
| `log` | `live-supported` | Emits connector log events. |
| `player-connected` | `live-supported` | Derived from dedicated-server player snapshots after a real server position is observed; turn-3 Takaro searches persisted both tested connections. |
| `player-disconnected` | `live-supported` | Derived from the same snapshot tracker; turn-3 Takaro searches persisted both tested disconnections. |
| `chat-message` | `live-supported` | Exact Takaro proof persisted one ordinary local chat line and one `$tplist` command input; the command executed successfully and returned its two expected whispers without duplicating either player-originated input. |
| `player-death` | `live-supported` | Exact Takaro proof persisted the controlled local-player death once with the bound player, real position, timestamp, and message; routed vanilla `OnDeath` diagnostics remain non-emitting. |
| `entity-killed` | `live-supported` | Exact Takaro proof persisted one player-attributed Greyling death with the bound player, timestamp, entity, and `Unarmed` weapon. |

## Server-Owned Action Semantics

`giveItem` delivers into a player's inventory when that player runs a negotiated
companion, and falls back to a world drop otherwise. A world drop is not a private
mutation: other players can collect the spawned objects.

**This applies to shop deliveries too**, since a shop claim delivers through `giveItem`.
On a server whose players have no companion, purchased goods land on the ground: lootable
by anyone nearby until collected, and losable if the buyer is falling or swimming. That is
worth knowing before running an economy on `companionMode=disabled`.

**A player with the companion gets the item in their inventory instead.** Protocol 2
adds an `item-grant` message: the server asks that player's companion to place the items,
and the companion — which can see the live inventory — puts in whatever fits and drops
only the remainder at the player's feet, telling them in chat which happened. So a
purchase normally lands in the bag, and the world drop survives as the safety net rather
than the default.

Players **without** a companion keep the world drop exactly as before. That covers
`companionMode=disabled`, vanilla clients under `optional`, and any companion whose
session did not negotiate the capability. Nothing regresses for them.

The adapter accepts at most 1,000 items and 100 world-drop stacks per request, validates quality, resolves prefab codes or display/name tokens, splits oversized stacks, and returns an error when no server-owned player position is known.

`teleportPlayer` requires a server-known character ZDO ID. It uses Valheim's built-in teleport RPC and returns `character_unavailable` when that identity is missing.

Player location never returns a fabricated origin. A live peer/public observation is cached for 30 seconds so Takaro can enrich a disconnect with the player's real last-known position; the cache is player-keyed, expires, and clears when Valheim replaces its network/world instance. Player-connected emission waits until such a real observation exists. If no current or fresh observation exists, the connector sends the position DTO's required numeric fields plus `payload.error`. At Takaro source commit `0c63cf1c`, the app connector validates that payload and `Generic.requestFromServer` rejects `payload.error` before returning a position. Root-level response metadata is not used by that consumer.

Remote inventory remains unavailable at the dedicated-server-only boundary, so a missing, disabled, or expired companion never becomes a fabricated empty array. A negotiated companion can instead submit a bounded canonical snapshot bound to its actual server peer. The server distinguishes that confirmed empty snapshot from missing/expired client state. This companion-backed path is live-supported by exact Takaro polling and inventory-change persistence.

Other failure-capable actions return immediately. At Takaro source commit `0c63cf1c`, validation-free actions such as `giveItem`, messaging, teleport, moderation, and shutdown accept `{ error: "code: message" }`, which `Generic.requestFromServer` rejects without waiting for a timeout. Validated object actions add only their required DTO fields before the same payload error; `testReachability` instead returns `connectable:false` with an actionable reason because that route bypasses the Generic error check. Array-validated actions cannot carry a top-level JSON error; their ordinary server-owned paths return arrays, and any actual failed array path is suppressed rather than fabricating an empty result.

The connector distinguishes a confirmed empty collection from an unavailable Valheim runtime source. `getPlayers`, `listItems`, `listEntities`, `listLocations`, and `listBans` return `[]` only when their required server singleton and collection exist. During world startup or reload, `runtime_unavailable` is suppressed for these array DTOs and lifecycle polling preserves its prior snapshot instead of fabricating an empty server or a false disconnect. A missing `getPlayer` match returns an immediate `player_not_found` payload error.

Outbound `sendMessage` delivery requires an active negotiated companion and is rendered into the normal Valheim chat history. Each request may supply Takaro's `opts.senderNameOverride`; the trimmed value is used for that message, while a missing or blank value displays as `Takaro`. It never falls back to the HUD overlay APIs, so a missing or incompatible companion produces an immediate `companion_server_chat_unavailable` error. Item-drop confirmations remain separate player-visible HUD notifications and are not treated as inbound chat.

## Evidence Boundary

Historical dedicated-server evidence from June 21-22, 2026 covers several entries marked `live-supported`, including vanilla-client player location, world-drop item delivery, built-in teleport, moderation, and delayed shutdown. The July 10 turn-3 run additionally persisted two complete player connect/disconnect cycles. Turn 4 re-proved a vanilla `Hehe` handshake, real position and teleport, lifecycle persistence, visible messaging/item/cron behavior, and an official raw `listLocations` response containing 11,293 nested locations without any client plugin. The standard Takaro `listLocations` route remained unavailable, so that action is `schema-fallback`, not `live-supported`.

The July 14 chat-only validation deployed release archives built from connector commit `82546ddd49c6`, negotiated companion protocol 1, and live-routed Takaro `sendMessage` requests to the connected client. The client rendered the messages in normal chat, not the HUD overlay, and logged the per-request sender `con` when the request supplied `opts.senderNameOverride`. See [`qa/2026-07-14-server-chat-validation.md`](qa/2026-07-14-server-chat-validation.md).

Turn 5 then live-proved immediate invalid-input failures, inventory non-mutation, lifecycle persistence, and the vanilla-client server boundary against its exact commit and artifact hashes. Turn 6 pinned the exhaustive action surface to an exact deployed artifact and re-proved pre-ready non-fabrication, immediate unsupported map errors, a vanilla connect/disconnect lifecycle, the real `85/36/-2` position across disconnect, and inventory non-mutation. Turn 7's exact prerelease artifact was rejected by BepInEx before startup; a numeric-version control isolated that failure to loader metadata. Turn 8 live-loaded its exact prerelease artifact, and turn 9 passed locale-stable packaging plus the safe live exerciser at real position `140/33/-2`. Turn-9 verification nevertheless found two release blockers: Valheim adapter calls were not marshalled to Unity's main thread, and Windows compile-reference fallback could replace a configured live server tree. Turn 10 addresses those findings with a bounded `Update()`-drained action scheduler and an owned reference-cache boundary. Historical server-only evidence remains in [the 2026-07-10 ledger](qa/2026-07-10-server-only-validation.md). Fresh companion evidence is recorded separately in [the 2026-07-12 owned-companion ledger](qa/2026-07-12-owned-companion-validation.md).

On 2026-09-02 the deployed `2.0.1` artifact was run against the reusable Takaro
connector acceptance checklist with a real graphical client attached. That run moved
`kickPlayer`, `banPlayer`, `unbanPlayer`, and `shutdown` from `unsupported` to
`live-supported`, re-proved the module command loop end to end, and characterised all
three `companionMode` values against a vanilla client. It also found that `banPlayer`
discards the ban reason. See
[`qa/2026-09-02-acceptance-validation.md`](qa/2026-09-02-acceptance-validation.md).

Later the same day the protocol-2 `item-grant` delivery was live-proven on
`2.0.0-dev.28a4566`: a `giveItem` and an in-game shop purchase both landed in the
player's inventory, a full bag dropped only the shortfall with exact counts, thirty
concurrent grants did not stall the main thread, and a protocol-1 companion against the
protocol-2 server was kicked while the server survived. The follow-up build
`2.0.0-dev.422148d` re-proved the enforcement wording that names the out-of-date
companion cause. See
[`qa/2026-09-02-item-grant-validation.md`](qa/2026-09-02-item-grant-validation.md).

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

Build the real companion against graphical-client references:

```bash
dotnet build src/Takaro.Valheim.Companion/Takaro.Valheim.Companion.csproj \
  -f net472 \
  -p:EnableValheimCompanionBuild=true \
  -p:BepInExReferencePath=/path/to/client/BepInEx/core \
  -p:ValheimReferencePath=/path/to/Valheim/valheim_Data/Managed
```

## Release Build

From `valheim/`:

```bash
./scripts/setup-environment.sh
./scripts/build-release.sh 0.1.0 dist
```

`setup-environment.sh` writes game compile references only to `VALHEIM_REFERENCE_CACHE_DIR`, which defaults to `_data/server`. A valid Managed directory can be reused read-only from any configured location. An invalid non-empty directory is writable only when it carries the setup script's completed ownership marker; otherwise setup refuses before invoking SteamCMD and directs the caller to a separate cache. The legacy `VALHEIM_SERVER_DIR` variable remains a safe fallback for read-only valid references or explicitly owned/empty caches, but it must not point setup at a live dedicated-server installation.

The release produces `takaro-valheim-plugin.zip` and `takaro-valheim-companion.zip`. The first contains the dedicated-server plugin, Core, Protocol, and required runtime dependencies. The second contains only the graphical-client companion, Protocol, and required runtime dependencies. Both exclude host-provided game, Unity, BepInEx, Harmony, Jotunn, debug, host, and role-inappropriate files.

The release version argument must be valid SemVer with major, minor, and patch values no greater than 65534. The exact full SemVer remains in assembly informational/package metadata, the packaged README, and `manifest.json`. BepInEx 5 parses its loader-facing attribute with `System.Version`, so that one value is deliberately normalized to numeric `major.minor.patch`; stable versions such as `1.0.0` therefore match exactly, while a version such as `1.0.0-rc.1+build.2` loads as `1.0.0`. Numeric assembly/file metadata uses `major.minor.patch.0`. The build generates both compile-time values under the intermediate output directory and does not edit tracked source files.

Environment setup downloads SteamCMD completely to a sibling temporary archive, extracts into a sibling staging directory, writes a completion marker only after validation, and publishes by directory rename with rollback. A markerless executable is repaired, a completed cache is reused, unrelated owned-cache files are preserved, and failure/signal cleanup cannot leave a partial executable trusted by the next run. It requires the host `file` utility to identify every required Valheim and BepInEx DLL as a real `PE32 ... Mono/.Net assembly`; marker blobs, empty files, and arbitrary text cannot satisfy validation. Valheim compile references are likewise built in a sibling staging directory, validated there, marked as an owned cache only after validation, and published by directory rename with rollback. Linux and Windows fallback therefore never inject or replace files inside an unowned live server tree.
