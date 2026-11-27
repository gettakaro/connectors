# Hytale connector: development

The connector is one Java plugin (`games/hytale/mod`, package `dev.takaro.hytale`) that the Hytale
dedicated server loads from its `mods/` folder. It opens the Takaro WebSocket itself; there is no
sidecar.

## Origin and licence

The plugin was written by [mad-001](https://github.com/mad-001) as
[Hytale-Takaro-Integration](https://github.com/mad-001/Hytale-Takaro-Integration) and is MIT
licensed ([LICENSE](LICENSE), copyright line kept as published). It was imported unchanged from
commit `ba872f1` of that repository; every change since is a separate commit here. Not imported:
the template example plugin, the separate HytaleCharts plugin, the legacy root plugin and a bundled
`ue4ss.zip`. The HytaleCharts heartbeat and promo messages were removed from the plugin: it sends
player data to Takaro only. Bundled libraries and their licences are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Layout

| Path | What it is |
|---|---|
| `mod/pom.xml` | Maven build: Java 25, `com.hypixel.hytale:Server` (scope `provided`, never shipped), Java-WebSocket and Gson shaded |
| `mod/src/main/java/dev/takaro/hytale/TakaroPlugin.java` | plugin entry point: config, listeners, ECS systems, request pool, map service |
| `mod/src/main/java/dev/takaro/hytale/websocket/` | the Takaro connection: identify, requests, events, reconnect |
| `mod/src/main/java/dev/takaro/hytale/handlers/TakaroRequestHandler.java` | every Takaro action |
| `mod/src/main/java/dev/takaro/hytale/events/` | chat, join/leave, death, entity-killed, log forwarding |
| `mod/src/main/java/dev/takaro/hytale/map/` | world map tiles rendered from the server's own map generator |
| `mod/src/main/java/dev/takaro/hytale/{state,util,config}/` | pure helpers (bans, known players, arguments, catalogues, config); unit tested |
| `mod/src/main/resources/manifest.json` | the Hytale plugin manifest; `Version` is stamped by the build |
| `assets/icon-256.png` | the game icon Takaro shows, embedded in the jar |
| `scripts/build-release.sh`, `scripts/test.sh` | release build and unit tests, both in the pinned Maven image |

## Protocol

`identify` offers `protocolVersions: [0, 1]` with a Capability Manifest
(`protocol/Capabilities.java`) and always sends `game: "hytale"` and
`connector: {name: "takaro-hytale", version}`. The version Takaro picks in `identifyResponse`
holds for that connection; a missing value is 0, and protocol 0 answers are unchanged.

- **Manifest:** players.list/get/location/inventory/teleport, items.list/give, entities.list,
  locations.list, chat.send, console.execute, moderation.kick/ban/unban/listBans,
  server.shutdown, the six `events.*`, `game.profile`, and `map` once the map is ready. Hytale has
  no level or skill API, so `players.progression` is not declared.
- **game.profile:** name `Hytale`, the running server version, and the icon
  (`assets/icon-256.png`, embedded; revision = first 12 hex of its sha256). `getGameIcon` answers
  with the whole file in one response.
- **Errors (v1):** `{error: {code, message}}` (`protocol/V1Results.java`); void actions and an
  unknown player's `getPlayer` / `getPlayerLocation` answer `payload: null`.
- **Events:** `protocol/EventOutbox.java`, one `streamId` per process, `seq` from 1,
  `EVENT_QUEUE_SIZE` events, drop-oldest. Events stay until an `eventAck` covers them and are resent
  after every identify, so nothing produced during an outage is lost.
- **Map:** the default world is one `tiled` Dimension (256 px tiles, zoom 0 = whole world, x right,
  z down like the in-game map), drawn from the server's own world-map generator off the world
  thread and only from chunks that already exist (`map/`). When the map becomes ready or grows
  after identify, a full `capabilities` message replaces the manifest. Protocol 0 keeps
  `getMapInfo` / `getMapTile` in the older tile layout.

## Build and test

Everything runs in the Maven image pinned by the catalog target
([`catalog/hytale/targets/server-0.7.7.json`](../../catalog/hytale/targets/server-0.7.7.json)), so
no local Java or Maven is needed:

```bash
games/hytale/scripts/test.sh                          # unit tests
games/hytale/scripts/build-release.sh 0.1.0 dist      # dist/takaro-hytale-server-0.7.7-0.1.0.jar
maintenance/bin/takaro-maint build --game hytale --target server-0.7.7 --version 0.1.0 --out dist --toolchain container
```

The Hytale server API comes from Hypixel's Maven repository (`https://maven.hytale.com/release`).
The build checks that the server jar Maven resolved has the SHA-256 pinned in the catalog target,
repacks the plugin jar with fixed timestamps (two builds of one commit give the same bytes) and
writes the target identity into it (`META-INF/takaro-target.json` and `Takaro-*` manifest
attributes). It refuses to package any `com/hypixel/` class.

For a quick local build with your own Maven and JDK 25: `cd games/hytale/mod && mvn package`
(version `0.0.0-dev`).

## A new Hytale version

Hytale's server API still changes between updates. The maintenance scan watches
`com.hypixel.hytale:Server` in Hypixel's release repository. For a new version: add a catalog
target with the new jar's SHA-256, set `hytale.server.version` in `mod/pom.xml`, fix what no longer
compiles, run the tests, then retest live with a real client before declaring the target. A Hytale
server cannot boot on a hosted runner (it needs `Assets.zip` and an account login), so CI verifies
at contract level only.

## Running a test server

Install the server with Hypixel's downloader (it needs a Hytale account), copy the jar into
`mods/`, start it once to create `mods/HytaleTakaroMod/TakaroConfig.properties`, set
`REGISTRATION_TOKEN` and `TAKARO_WS_URL` (for a local or staging Takaro), and restart. The server
itself needs `/auth login device` once before release clients can join; that login is stored
encrypted next to the server and refreshes while the server runs.

## Configuration keys

`mods/HytaleTakaroMod/TakaroConfig.properties`:

| Key | Default | Meaning |
|---|---|---|
| `IDENTITY_TOKEN` | `MyHytaleServer` | name that identifies this server in Takaro |
| `REGISTRATION_TOKEN` | empty | Takaro registration token; re-read without a restart |
| `TAKARO_WS_URL` | `wss://connect.takaro.io/` | the Takaro to connect to |
| `COMMAND_PREFIX` | `!` | chat prefix for Takaro commands |
| `COMMAND_RESPONSE` | `[cyan]Command[-] ...` | private reply when a command is received |
| `MAP_ENABLED` | `true` | render world map tiles for Takaro |
| `LOG_FORWARD_LEVEL`, `LOG_FORWARD_MAX_PER_MIN` | `INFO`, `120` | server log forwarding |
| `EVENT_QUEUE_SIZE` | `1000` | events kept while Takaro is unreachable |
| `CATALOG_INCLUDE_DEBUG` | `false` | include debug/test/template assets in item and entity lists |
| `COMMAND_SETTLE_MS` | `0` | extra time to collect console command output |
| `TAKARO_DEBUG` | `false` | trace every WebSocket frame |

`DEV_ENABLED` with `DEV_WS_URL`, `DEV_IDENTITY_TOKEN` and `DEV_REGISTRATION_TOKEN` is the older
way to point at another Takaro and is still read when `TAKARO_WS_URL` is not set.
