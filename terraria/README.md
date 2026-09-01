# Takaro Terraria Connector

Terraria support in Takaro is made of two parts that run on the Terraria
dedicated server host:

- `src/TakaroTerrariaEvents` — a server-side TShock plugin that emits structured
  `TAKARO_EVENT` markers for events TShock REST does not expose, and registers
  admin-only helper commands for coordinate-based location and teleport.
- `bridge/` — a TypeScript sidecar that connects outbound to Takaro over
  WebSocket and maps Takaro actions onto the TShock REST API.

The plugin alone cannot talk to Takaro, and the bridge alone cannot report
deaths, NPC kills, player coordinates, or coordinate teleports. Install both for
full coverage.

## Architecture

TShock runs inside the Terraria dedicated server and exposes a REST API. The
bridge stays outside the game process, calls that REST API, and connects
outbound to Takaro, so no inbound port has to be opened for Takaro.

```text
Takaro  <--websocket--  bridge/  --REST-->  TShock  --hooks-->  plugin
                             ^                                     |
                             +--------- tails TShock log ----------+
```

## Plugin

### Build

Install the TShock reference DLLs once:

```bash
terraria/scripts/setup-environment.sh
```

Build the plugin:

```bash
terraria/scripts/build-mod.sh
```

The build output is written to:

```text
terraria/_data/build/TakaroTerrariaEvents/TakaroTerrariaEvents.dll
```

### Package

```bash
terraria/scripts/build-release.sh 0.1.0 dist
```

This creates `dist/takaro-terraria-plugin.zip`.

### Install

1. Install TShock on the Terraria dedicated server.
2. Copy `TakaroTerrariaEvents.dll` into the TShock server plugin directory.
3. Restart the server.
4. Confirm the TShock log contains `Takaro Terraria Events plugin loaded`.

### Runtime commands

```text
/takaropos <player>
/takarotp <player> <x> <y>
/takaroinv <player>
```

These are server-side TShock commands intended for connector automation, and
all of them require the `takaro.admin` TShock permission.

A REST user in the `superadmin` group already satisfies this through TShock's
wildcard permission, which is the common setup and needs no extra configuration.
For a more narrowly scoped user, grant `takaro.admin` explicitly: without it
`teleportPlayer` fails, `getPlayerLocation` reports `0,0,0`, and
`getPlayerInventory` returns an empty list, all rather than failing loudly.

## Bridge

### Build

```bash
cd terraria/bridge
npm ci
npm test
npm run build
```

### Package

```bash
terraria/scripts/build-bridge-release.sh 0.1.0 dist
```

This creates `dist/takaro-terraria-bridge.zip`.

### Install

1. Enable REST in `tshock/config.json` with `RestApiEnabled=true` and
   `RestApiPort=7878`.
2. Create an application REST token for a TShock user that holds
   `takaro.admin` (a `superadmin` user already does).
3. Extract the bridge zip on the server host and run `npm ci --omit=dev`.
4. Copy `TakaroConfig.example.txt` to `TakaroConfig.txt` and fill in the Takaro
   registration token and the TShock REST values.
5. Start the bridge with `npm start`.

Keep real registration tokens and REST tokens out of version control.

### Local endpoints

```text
GET /health     bridge, Takaro identification, and TShock reachability
GET /coverage   per-action and per-event support status
```

## Coverage

Every Takaro action has one explicit outcome, registered in
`bridge/src/takaro/coverage.ts`. Status meanings:

- **Supported** — works against a live server.
- **Not applicable** — the concept does not exist in Terraria, or TShock exposes
  no way to reach it. A Takaro-valid empty or disabled response is returned so
  callers do not break.
- **Not built yet** — technically reachable, but not implemented here. These are
  the real roadmap items.

### Actions

| Action | Status | Notes |
| --- | --- | --- |
| `testReachability` | Supported | Token test plus `/v2/server/status`. |
| `getPlayers` | Supported | From TShock REST. |
| `getPlayer` | Supported | From TShock REST. |
| `getPlayerLocation` | Supported | Plugin command `/takaropos`. |
| `getPlayerInventory` | Supported | Plugin command `/takaroinv`. |
| `sendMessage` | Supported | `/v2/server/broadcast`. Global and per-recipient. |
| `executeConsoleCommand` | Supported | Allowlisted by exact match and prefix. |
| `giveItem` | Supported | `/give`, with name-to-item-code resolution. |
| `teleportPlayer` | Supported | Plugin command `/takarotp`. |
| `kickPlayer` | Supported | TShock console command. |
| `banPlayer` | Supported | `/bans/create`. |
| `unbanPlayer` | Supported | `/v2/bans/destroy`. |
| `listBans` | Supported | `/v2/bans/list`. |
| `listItems` | Supported | 6147-entry catalog built from the server assemblies. |
| `shutdown` | Supported | `/v2/server/off`, gated behind `enableShutdown`. |
| `listEntities` | Not applicable | Terraria NPCs spawn from world state; there is no queryable entity registry. |
| `listLocations` | Not applicable | Terraria has no named-location concept for Takaro to list. |
| `getMapInfo` | Not applicable | Returns a disabled map DTO. TShock exposes no map metadata. |
| `getMapTile` | Not applicable | TShock does not render map tiles. |

### Events

| Event | Status | Source |
| --- | --- | --- |
| `player-connected` | Supported | Derived from TShock player snapshots. |
| `player-disconnected` | Supported | Derived from TShock player snapshots. |
| `player-death` | Supported | Plugin `TAKARO_EVENT` marker. |
| `entity-killed` | Supported | Plugin `TAKARO_EVENT` marker. |
| `chat-message` | Supported | Parsed from the TShock log (best effort). |
| `log` | Supported | Tailed from configured TShock log files. |

Set `logFiles` in the bridge config to the active TShock log, otherwise the
log-derived events above are not delivered.

`entity-killed` reports a `weapon`, taken from the killer's held item at the
moment the kill fires. Terraria records no damage source on NPC death, so this
is a proxy rather than exact attribution: a projectile fired earlier can land
after the player swaps weapons, and minion, sentry, or damage-over-time kills
may credit an item that dealt none of the damage. It reports `unknown` when no
killer or held item resolves.

### Items and inventory

Terraria has items, and both directions work. The bridge ships a static catalog
of 6147 items extracted from the server assemblies and resolves a display name
such as `Wood` to the numeric code TShock's `/give` expects.

Inventory reading is plugin-backed. `/takaroinv` reports every container a
player owns, skips empty slots, and aggregates duplicate item types across all
of them into a single entry by summing stacks:

| Container | Contents |
| --- | --- |
| `inventory` | main slots |
| `armor` | armor, accessories, and their vanity slots |
| `dye` | dye slots |
| `miscEquips` | pet, light pet, mount, and grapple slots |
| `miscDyes` | dyes for those misc slots |
| `trashItem` | trash slot |
| `bank`, `bank2`, `bank3`, `bank4` | Piggy Bank, Safe, Defender's Forge, Void Vault |
| `Loadouts` | stored equipment loadouts |

Loadouts do not double-count. `EquipmentLoadout.Swap` exchanges items with
`player.armor` and `player.dye` rather than copying them, so the active
loadout's own arrays hold only empty items while it is equipped and the
empty-slot check drops them. Only the inactive loadouts contribute entries.

Excluded are Terraria's transient engine arrays and its cached
accessory-effect items, such as `starCloakItem`, which are internal state
rather than possessions and would otherwise be reported as phantom items.

Takaro does not call `listItems` on demand. It runs a `syncItems` job when a
game server is registered, hourly thereafter, and on manual trigger, and that
job checks reachability first. If the connector is not attached at registration
time the initial sync is skipped and Takaro's item table for that server stays
empty until the next successful sync. Attach the bridge before registering the
server, or trigger the job manually afterwards.

## Safety

Raw console execution is allowlisted by exact command and by prefix. Shutdown is
separately gated behind `enableShutdown=true` and is off by default.

## Version support

The plugin builds against the TShock release in `terraria/scripts/setup-environment.sh`.
TShock must match the Terraria server protocol version, and Terraria clients must
match the server. A Terraria client newer than the TShock build is rejected at
join time with `You are not using the same version as this server.`
