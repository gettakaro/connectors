# Empyrion connector: development

The connector is the C# mod `TakaroEmpyrion`, loaded by Empyrion's own mod loader from
`Content/Mods/TakaroEmpyrion/` (`ModTargets: Dedi, PfServer` in `TakaroEmpyrion_Info.yaml`):

```
mod/src/TakaroMod.cs           entry point: IMod (Mod API v2) and ModInterface (v1) in one class
mod/src/Protocol/              Takaro WebSocket client (protocol v0 and v1), Capability Manifest, args
mod/src/Game/GameBridge.cs     the only path into the game: queued work run in Game_Update under a frame budget
mod/src/Game/Actions.cs        one handler per Takaro action
mod/src/Game/Events.cs         game events -> Takaro events
mod/src/Game/PlayfieldAgent.cs runs in every playfield server and forwards kill events to the dedicated server
mod/src/Game/Catalog.cs        items, blocks and creatures from the game's .ecf configs and Localization.csv
mod/src/Game/PlayerRegistry.cs known players (players.json) and their Takaro identity fields
mod/src/Game/TelnetConsole.cs  console commands with real output, through the dedi's loopback telnet console
mod/src/Map/                   protocol v1 planet maps rendered from the server's terrain cache
mod/src/Core/Coverage.cs       one record per Takaro action and event; capabilities.json must match it
tests/                         contract tests: a fake Takaro and a fake Empyrion drive the real code
scripts/build-release.sh       the release build, driven by catalog/empyrion/targets/<target>.json
```

## Build and test

The mod compiles with Mono's `csc -langversion:7.3` against the server's own `Mif.dll`, `ModApi.dll` and
`Newtonsoft.Json.dll` (from `DedicatedServer/EmpyrionDedicated_Data/Managed`) plus Mono's `netstandard`
facade, `System.Net.Http` and `System.IO.Compression`. Nothing from the game is committed: the scripts
fetch exactly those three files from the target's pinned depot manifest with
`takaro-maint steam references` into `_data/refs/<fingerprint>/` and check their sha256.

```
scripts/test.sh                 # contract tests in the pinned mono image
scripts/build.sh                # bin/TakaroEmpyrion/, the folder that goes into Content/Mods
```

Both take `--target <id>` and run in the target's pinned `mono` image. `EMPYRION_MANAGED=<server>/DedicatedServer/EmpyrionDedicated_Data/Managed`
compiles against a server's own folder instead, and `BUILDER_IMAGE=<image>` uses another image.
`EMPYRION_SAVES=<server>/Saves` adds the map checks against a real save and writes a reference
render of every cached planet to `bin/tests/map-<planet>.png`; it is optional and not used in CI.

The tests cover the wire protocol (v0 and v1 identify, request/response, error frames, reconnect,
event resend with `seq`), every action and event against the fake game, the catalogue filters, the
map renderer and `capabilities.json` drift against `Coverage.cs`. A status in `Coverage.cs` moves to
`live-supported` only after a live run against a real server.

## Release build

```
maintenance/bin/takaro-maint build --game empyrion --target windows-25490402 --version 0.1.0-dev --out dist --toolchain container
```

`build-release.sh` fetches and checks the references, stamps the version into a staged copy of
`TakaroMod.cs` (`Version`) and `TakaroEmpyrion_Info.yaml`, checks the image's `netstandard.dll` against
the catalog hash, compiles with `-deterministic`, runs the contract tests on the same sources and packages
`takaro-empyrion-<target>-<version>.zip` (`TakaroEmpyrion/` with the DLL, the info yaml, the config template,
`takaro-target.json` and `PROVENANCE.txt`) plus its `.meta.json`. Two builds of the same commit produce
identical bytes. `EMPYRION_REFS_DIR=<dir>` builds from already fetched references (still hash-checked).

## Dev rig

`dev-servers/compose/empyrion.yml` runs the Windows dedicated server under Wine (`dev-servers/images/empyrion`),
with the server install and the Wine prefix bind-mounted (`EMPYRION_SERVER_DIR`, `EMPYRION_WINE_DIR`). The
entrypoint runs the dedi from `DedicatedServer/` (it resolves `../` paths from there) and stops it through
the telnet console (`saveandexit`), because the dedi ignores signals under Wine.

Deploy a build: stop the container, copy `bin/TakaroEmpyrion/` over `<server>/Content/Mods/TakaroEmpyrion/`
(keep `TakaroConfig.txt`, `players.json`, `bans.json` and `Logs/`), start the container.
`dev-servers/scripts/deploy-connector.sh empyrion` does exactly that. Never replace the DLL while the
server runs: Mono loads code lazily, so the next method it loads from a swapped file fails with
`BadImageFormatException`.

The mod writes `Logs/takaro.log` (and `Logs/playfield-<pid>.log` per playfield server) inside its folder;
`DEBUG=true` in `TakaroConfig.txt` adds every Takaro frame, tokens redacted.

## Architecture notes

- **Two processes, one connection.** The dedicated server holds the Takaro WebSocket and answers every
  action. Kill game events (`Event_GameEvent` `KilledEntity`) are raised only inside playfield server
  processes, so a `PlayfieldAgent` in each one forwards them to the dedi over the game's mod network
  (`INetwork.SendToDedicatedServer`, channel `TakaroEmpyrion`). The agent holds no Takaro connection and
  no secrets.
- **The game thread.** All game API calls go through `GameBridge`, which runs queued work in `Game_Update`
  under a per-frame budget; legacy requests resolve on the matching `Game_Event`. Map tiles, the catalogue
  and the telnet console run on worker threads.
- **Config inheritance.** `.ecf` blocks inherit every property they do not set from their `Ref:` parent,
  also across files (for example a baby creature takes its `Faction` from its parent). The catalogue walks
  that chain (bounded, cycle-safe) when it decides what is a creature, and filters ships, structures and
  test rigs found anywhere on it.
- **Item ids.** The runtime item id is `8192 + ItemsConfig Id`; `ModConsts.ItemBaseId` (4096) is not the
  runtime base and giving such an id crashes the client. The catalogue checks against the runtime
  mapping once the dedi has one.
- **Player identity.** `PlayerInfo.steamId` is the Takaro `gameId`. For Steam players it is a SteamID64 and
  is sent as `steamId` plus `platformId steam:<id>`. Epic Games Store players carry a numeric id the game
  makes itself (outside the SteamID64 range, not an EOS or Epic account id); it is sent only as
  `platformId epic:<id>`, never as `steamId` or `epicOnlineServicesId`. Kick, ban and unban take either form.
- **Game Profile.** Identify carries `game` and `connector` (`takaro-empyrion` with the release version); the
  v1 manifest adds `game.profile`. Its `version` comes from `BuildNumber.txt` in the server root, for example
  `1.19.6 b5188`, and is left out when that file cannot be read. `icon` is `assets/icon-256.png` (Steam library
  art of app 383120, 256 x 256 PNG, at most 128 KiB), embedded in the DLL; `getGameIcon` serves it, under a
  revision taken from its sha256.
- **Maps.** On protocol v1 the mod declares one Dimension per planet whose terrain the server has cached
  (`terrain.raw`, 16-bit little endian, plus `biomes.dat`) and renders tiles from those files. Takaro takes
  at most 16 Dimensions.

## A new Empyrion build

The mod uses the public Mod API, so a new build usually keeps working. Before a new target is added:
install the build, run the connector with a real client, and recheck at least join, chat, getPlayers,
teleport, giveItem, kick, ban and a creature kill. Then add `catalog/empyrion/targets/windows-<buildid>.json`
with the new depot manifests (`takaro-maint steam pin --game empyrion`) and the reference hashes.
