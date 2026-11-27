# DayZ connector: development

The connector has two parts that ship together:

- **the mod** (`mod/`): Enforce Script, packed into `@TakaroIntegration/Addons/TakaroIntegration.pbo`
  and loaded by the dedicated server with `-serverMod=`. It reads game state and runs actions.
- **the sidecar** (`sidecar/`): a Node.js program on the same machine. It holds the Takaro
  WebSocket and talks to the mod over loopback HTTP on `127.0.0.1:8088`.

## Why there is a sidecar

The repository prefers native connectors, where the plugin inside the game server holds the Takaro
WebSocket itself (`docs/adr/0002-prefer-native-connectors.md`). DayZ cannot do that:

- **Enforce Script has no WebSocket and no sockets.** The only network API a server mod gets is
  `RestApi`/`RestContext`, a one-shot HTTP client. Takaro's game server protocol is WebSocket only,
  so a mod alone cannot connect.
- **There is no supported way into native code.** `callExtension`, the usual escape hatch to a native
  library, exists only in the DayZDiag diagnostic build, not in the retail server.
- **The native alternative considered** is injecting a library into the server process: the upstream
  project has a Windows `winmm.dll` proxy for that (not imported), and Linux would need `LD_PRELOAD`.
  Both hook a closed engine binary that also hosts BattlEye, break with engine updates, exist for one
  OS each, and need control of the server's launch environment that many hosts do not give.

So the mod polls the sidecar over loopback HTTP (events out, requests in, results back) and the
sidecar speaks the Takaro protocol. An outbound WebSocket needs no inbound port; the mod-facing
HTTP server binds `127.0.0.1` only. If Enfusion ever gains a WebSocket client in script, the sidecar
can go.

## Origin and licence

Written by [mad-001](https://github.com/mad-001) as
[DayZ-Takaro-Integration](https://github.com/mad-001/DayZ-Takaro-Integration), MIT licensed
([LICENSE](LICENSE), copyright line kept as published). Imported unchanged from commit `d959819`
(v0.1.14); every change since is a separate commit here. Not imported: the Windows `winmm.dll` proxy
transport, the unrelated HypeTrainSoundFix and IronZoneQuestFix mods and their packed PBOs, the
project website, and the Windows install and deploy scripts. No Bohemia Interactive game files are
committed or shipped; the PBO holds only this mod's scripts and `config.cpp`.

## Layout

| Path | What it is |
|---|---|
| `mod/takaro_integration/config.cpp` | `CfgPatches`/`CfgMods`; the version is stamped by the build |
| `mod/takaro_integration/scripts/3_Game/TakaroConfig.c` | `profiles/TakaroIntegration/config.json`, logging, the name cache |
| `mod/takaro_integration/scripts/4_World/TakaroBridge.c` | registration with the sidecar, event flush and request poll timers |
| `mod/takaro_integration/scripts/4_World/TakaroCommandDispatcher.c` | every Takaro action |
| `mod/takaro_integration/scripts/4_World/TakaroEventQueue.c` | the bounded event queue and per-event JSON |
| `mod/takaro_integration/scripts/4_World/{PlayerBase,ZombieBase,AnimalBase}Takaro.c` | deaths and kills |
| `mod/takaro_integration/scripts/4_World/TakaroMapSurvey.c` | the world survey the map tiles are drawn from |
| `mod/takaro_integration/scripts/5_Mission/MissionServerTakaro.c` | connect, disconnect, chat and the tick |
| `mod/tools/pack_pbo.py` | reproducible pure-Python PBO packer (`product=dayz ugc`, prefix `TakaroIntegration`) |
| `sidecar/src/ws/` | the Takaro protocol: identify, Capability Manifest, event outbox, v1 errors |
| `sidecar/src/local/server.ts` | the mod-facing HTTP API and the per-action shape fixes |
| `sidecar/src/catalogue.ts` | human item and entity names, and the names in kill and death events |
| `sidecar/src/gameProfile.ts` | game and connector slugs, `game.profile`, the icon |
| `sidecar/src/map/` | map tiles rendered from the survey |
| `sidecar/src/local/rptTail.ts` | server log lines as `log` events |
| `assets/icon-256.png` | the game icon Takaro shows (DayZ's Steam client icon) |
| `scripts/build-release.sh`, `scripts/test.sh` | release build and tests, in the pinned Node image |

## Protocol

`identify` sends `game: "dayz"` and `connector: {name: "takaro-dayz", version}`, offers protocol
versions 0 and 1, and on version 1 carries the Capability Manifest (`sidecar/src/ws/capabilities.ts`):
`game.profile` (name, the server build the mod reports with each poll, the icon), every action and
event the mod answers, `players.characters` (each DayZ life, ended by death) and the world map as one
tiled Dimension once the survey exists. Version 1 events are numbered, kept until Takaro acknowledges
them and resent after a reconnect; events produced before identify answers wait for it.

The sidecar connects to Takaro only after the mod's first poll, which starts once the world has loaded. Takaro fills a new server's item
and entity catalogue right after its first identify (then hourly), and a DayZ server needs minutes
to load its world; connecting earlier made that first sync find no mod, and showed a loading server
as online.

Identity: `gameId` and `steamId` are the SteamID64 and `platformId` is `steam:<SteamID64>` on every
path; DayZ's own player UID is never sent.

## Game thread

Everything the mod does runs on the server's game thread, which Enforce Script cannot leave. The mod
keeps that work small: it batches events and polls on timers, and walks the game config for the item
and entity catalogues once per server start (the result is cached). Shaping answers, naming,
rendering map tiles and the Takaro connection all happen in the sidecar.

## Build and test

```bash
games/dayz/scripts/test.sh                       # sidecar unit tests + PBO packer tests
maintenance/bin/takaro-maint build --game dayz --target linux-26220720 --version 0.1.0 --out dist --toolchain container
```

The release build is reproducible: two builds of one commit give identical bytes. CI runs at
contract level: the DayZ server depots need a Steam account that owns DayZ, so no hosted runner can
boot a server. Enforce Script is compiled only by the server at start; after a mod change, check one
server start for `[Takaro] Bridge v<version> starting` and no `Can't compile` in the script log.
