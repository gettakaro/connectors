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
```

Both are server-side TShock commands intended for connector automation, and both
require the `takaro.admin` TShock permission.

Grant that permission to the TShock user the bridge authenticates as, otherwise
`teleportPlayer` fails and `getPlayerLocation` reports `0,0,0` instead of the
player's real position.

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
   `takaro.admin`.
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
`bridge/src/takaro/coverage.ts`.

Live-supported: reachability, players, player lookup, plugin-backed player
location, item catalog, broadcast and sendMessage, allowlisted raw command, give
item, plugin-backed coordinate teleport, kick, ban, unban, list bans, and guarded
shutdown.

Schema fallbacks, because TShock REST does not expose the state:
`getPlayerInventory`, `listEntities`, `listLocations`, and `getMapInfo`.

Unsupported: `getMapTile`, because the REST API does not render map tiles.

`listItems` is backed by a static Terraria item catalog, and the adapter resolves
displayed names such as `Wood` back to numeric TShock item codes for `/give`.

## Events

`player-connected` and `player-disconnected` are derived from repeated TShock
player snapshots.

`player-death` and `entity-killed` come from the plugin's `TAKARO_EVENT` markers,
which the bridge reads from the TShock log. Set `logFiles` in the bridge config
to the active TShock log for these to be delivered.

Chat, join, and leave lines are additionally parsed from the log on a
best-effort basis.

## Safety

Raw console execution is allowlisted by exact command and by prefix. Shutdown is
separately gated behind `enableShutdown=true` and is off by default.

## Version support

The plugin builds against the TShock release in `terraria/scripts/setup-environment.sh`.
TShock must match the Terraria server protocol version, and Terraria clients must
match the server. A Terraria client newer than the TShock build is rejected at
join time with `You are not using the same version as this server.`
