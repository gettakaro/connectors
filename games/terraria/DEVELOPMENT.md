# Terraria connector — development

Developer, architecture and build notes for the Terraria connector. Operator install steps live
in [README.md](README.md).

## Architecture

Terraria support is made of two parts that run on the Terraria dedicated server host:

- `mod/TakaroTerrariaEvents` — a server-side TShock plugin that emits structured `TAKARO_EVENT`
  markers for events TShock REST does not expose, and registers admin-only helper commands for
  coordinate-based location, inventory, give, teleport, ban and unban.
- `bridge/` — a TypeScript sidecar that connects outbound to Takaro over WebSocket and maps
  Takaro actions onto the TShock REST API.

TShock runs inside the Terraria dedicated server and exposes a REST API. The bridge stays outside
the game process, calls that REST API, and connects outbound to Takaro, so no inbound port has to
be opened for Takaro.

```text
Takaro  <--websocket--  bridge/  --REST-->  TShock  --hooks-->  plugin
                             ^                                     |
                             +--------- tails TShock log ----------+
```

## Plugin

### Build

Install the TShock reference DLLs once:

```bash
games/terraria/scripts/setup-environment.sh
```

This extracts `TShockAPI.dll`, `TerrariaServer.dll` and `OTAPI.dll` from
`ghcr.io/pryaxis/tshock:stable` (override with `TSHOCK_IMAGE`) into `_data/refs`.

Build the plugin:

```bash
games/terraria/scripts/build-mod.sh
```

The build output is written to:

```text
games/terraria/_data/build/TakaroTerrariaEvents/TakaroTerrariaEvents.dll
```

### Package

```bash
games/terraria/scripts/build-release.sh 0.2.0 dist
```

This creates `dist/takaro-terraria-plugin.zip`, containing `TakaroTerrariaEvents/` with the DLL
and a generated `README.txt`.

### Runtime commands

```text
/takaropos <player>
/takarotp <player> <x> <y>
/takaroinv <player>
/takarogive <player> <item> <amount>
/takaroban <player> <reason>
/takarounban <player>
```

These are server-side TShock commands intended for connector automation, and all of them require
the `takaro.admin` TShock permission.

A REST user in the `superadmin` group already satisfies this through TShock's wildcard permission,
which is the common setup and needs no extra configuration. For a more narrowly scoped user, grant
`takaro.admin` explicitly: without it `teleportPlayer` fails, `getPlayerLocation` reports `0,0,0`,
and `getPlayerInventory` returns an empty list, all rather than failing loudly.

## Bridge

### Build

```bash
cd games/terraria/bridge
npm ci
npm test
npm run build
```

### Package

```bash
games/terraria/scripts/build-bridge-release.sh 0.2.0 dist
```

This creates `dist/takaro-terraria-bridge.zip`, containing `TakaroTerrariaBridge/` with `dist/`
(tests stripped), `TakaroConfig.example.txt`, `package.json`, `package-lock.json`, the connector
README and a generated `README.release.txt`.

### Local endpoints

```text
GET /health     bridge, Takaro identification, and TShock reachability
GET /coverage   per-action and per-event support status
```

`/health` returns `{ ok, takaroIdentified, gameServerId, tshockReachable, lastPollAt }`.

### CI

`.github/workflows/terraria.yml` runs `npm ci && npm test && npm run build` for the bridge on
every PR, and a `package` job that sets up .NET 9 and Node 22, runs `setup-environment.sh`, builds
both zips and publishes them: stable assets on a `terraria-v*` tag, a rolling `terraria-dev`
pre-release on pushes to main, and a disposable `pr-<number>-terraria` pre-release per PR.

## Coverage

Every Takaro action has one explicit outcome, registered in `bridge/src/takaro/coverage.ts`.
Status meanings:

- **Supported** — reachable through TShock REST, a raw command, polling or log tailing.
- **Not applicable** — the concept does not exist in Terraria, or TShock exposes no way to reach
  it. A Takaro-valid empty or disabled response is returned so callers do not break.
- **Not built yet** — technically reachable, but not implemented here. These are the real roadmap
  items.

These are implementation statuses, not proof that an action works on a live server; see the table
in [README.md](README.md) for what has actually been verified.

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
| `banPlayer` | Supported | Plugin command `/takaroban`, banning UUID and IP. |
| `unbanPlayer` | Supported | Plugin command `/takarounban`, clearing every identifier. |
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
| `player-death` | Supported | Plugin `TAKARO_EVENT` marker, including the resolved `attacker`. |
| `entity-killed` | Supported | Plugin `TAKARO_EVENT` marker. |
| `chat-message` | Supported | Parsed from the TShock log (best effort). |
| `log` | Supported | Tailed from configured TShock log files. |

Set `logFiles` in the bridge config to the TShock log directory, otherwise the log-derived events
above are not delivered. TShock opens a new log file on every restart; pointing at the directory
lets the bridge follow that rotation, whereas a single file path goes silent after the first
restart.

The tailer never ships lines matching `logExcludePatterns` (default
`takaro-rest executed:,RestManager:`) as `log` events. TShock logs every REST call the bridge
itself makes, so without this the bridge feeds its own traffic back to Takaro and trips the rate
limiter, which silently drops real gameplay events. The filter applies only to plain `log`
passthrough, so a chat line or a death whose text happens to match is still delivered. Set an
empty value to disable filtering, or a comma-separated list to replace the defaults.

`entity-killed` reports a `weapon`, taken from the killer's held item at the moment the kill
fires. Terraria records no damage source on NPC death, so this is a proxy rather than exact
attribution: a projectile fired earlier can land after the player swaps weapons, and minion,
sentry, or damage-over-time kills may credit an item that dealt none of the damage. It reports
`unknown` when no killer or held item resolves.

## Items and inventory

Terraria has items, and both directions work. The bridge ships a static catalog of 6147 items
extracted from the server assemblies and resolves a display name such as `Wood` to the numeric
code TShock's `/give` expects.

Inventory reading is plugin-backed. `/takaroinv` reports every container a player owns, skips
empty slots, and aggregates duplicate item types across all of them into a single entry by summing
stacks:

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

Loadouts do not double-count. `EquipmentLoadout.Swap` exchanges items with `player.armor` and
`player.dye` rather than copying them, so the active loadout's own arrays hold only empty items
while it is equipped and the empty-slot check drops them. Only the inactive loadouts contribute
entries.

Excluded are Terraria's transient engine arrays and its cached accessory-effect items, such as
`starCloakItem`, which are internal state rather than possessions and would otherwise be reported
as phantom items.

Takaro does not call `listItems` on demand. It runs a `syncItems` job when a game server is
registered, hourly thereafter, and on manual trigger, and that job checks reachability first. If
the connector is not attached at registration time the initial sync is skipped and Takaro's item
table for that server stays empty until the next successful sync. Attach the bridge before
registering the server, or trigger the job manually afterwards.

## Bans

Bans go through the plugin's `/takaroban`, not TShock REST. TShock matches an untyped name ban
only against players who authenticated under that name, so on a server where players join
unauthenticated a REST name ban records cleanly and the player reconnects straight through it.

The plugin bans `uuid:<TSPlayer.UUID>` plus `ip:<address>`, which TShock matches on every join,
and disconnects the player. The IP is banned alongside the UUID because a reinstalled client
produces a fresh UUID.

Because a banned player is offline — so their UUID can no longer be read from a live `TSPlayer` —
each ban is tagged `[takaro:<name>]` in its reason, and `/takarounban` searches the ban list by
that tag. Without that tag an unban can only ever succeed for a player who is not actually banned.

Without the plugin loaded, both actions fall back to the old REST name ban, so an operator running
the bridge alone keeps prior behaviour rather than losing bans outright.

## Safety

Raw console execution is allowlisted by exact command and by prefix. Shutdown is separately gated
behind `enableShutdown=true` and is off by default. Application REST tokens and Takaro
registration tokens are operational secrets: keep them in `TakaroConfig.txt` or the environment
(`TAKARO_REGISTRATION_TOKEN`, `TSHOCK_TOKEN`, `TSHOCK_USERNAME`, `TSHOCK_PASSWORD`), never in
version control.

## Version support

The plugin builds against the TShock release in `games/terraria/scripts/setup-environment.sh`.
TShock must match the Terraria server protocol version, and Terraria clients must match the
server. A Terraria client newer than the TShock build is rejected at join time with
`You are not using the same version as this server.`
