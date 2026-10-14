# SCUM connector: development

The connector is the UE4SS mod `TakaroSCUM`, running inside `SCUMServer.exe`:

```
mod/TakaroSCUM/Scripts/        Lua: Takaro protocol, action router, events, and the game adapter
  takaro/game.lua              the only module that touches the game (UE4SS reflection)
  takaro/coverage.lua          one record per Takaro action and event; capabilities.json is generated from it
native/src/                    main.dll, a UE4SS C++ mod: the Takaro WebSocket (WinHTTP, TLS) and a few Lua functions
native/ue4ss/                  UE4SS.def (the UE4SS.dll exports main.dll links against) and the required hook settings
native/check-imports.py        every UE4SS import of main.dll must be exported by the pinned UE4SS.dll
native/build.bat               local MSVC v143 build (main.dll + takaro-smoke.exe, a WebSocket smoke test)
tests/                         Lua tests against a fake UE4SS and a fake Takaro
scripts/build-release.sh       the release build, driven by catalog/scum/targets/<target>.json
```

## Tests

```
LUA=lua5.4 tests/run.sh
```

The tests cover the wire protocol (identify, request/response, reconnect, event resend), every action
with a stub game, the log parsers, config handling and `capabilities.json` drift. After changing
`coverage.lua`, regenerate the registry:

```
LUA_PATH="mod/TakaroSCUM/Scripts/?.lua;;" lua5.4 scripts/gen-capabilities.lua > capabilities.json
```

## Release build

```
maintenance/bin/takaro-maint build --game scum --target windows-25803875 --version 0.1.0-dev --out dist --toolchain container
```

`build-release.sh` runs the Lua tests and compiles `main.dll` with clang-cl and lld-link in the pinned
Debian image, against the MSVC CRT and Windows SDK that the pinned `xwin` downloads (checked against
the catalog's tree digest). It then checks the UE4SS imports and packages the zip: the pinned UE4SS
build with the required `[Hooks]` settings, `mods.txt` with only TakaroSCUM enabled, and the mod.
Two builds of the same commit produce identical bytes.

## Why only two UE4SS hooks

With UE4SS's stock hook set, SCUMServer 1.3.4.2 crashes as soon as a player joins. The connector needs
only `HookEngineTick` (its game-thread loop) and `HookUObjectProcessEvent` (its function hooks on
SCUM's chat and attack RPCs), so the release turns every other hook off
(`native/ue4ss/UE4SS-settings.required.txt`). The UE4SS build is pinned and bundled because its
`experimental-latest` release is replaced in place.

## Protocol 1 and the map

The connector offers Takaro protocol versions `[0, 1]` (`takaro/protocol.lua`, `takaro/v1.lua`). On version 1 it
declares a Capability Manifest built from the live-supported coverage entries plus the `map` capability, answers
errors as `{ code, message }`, adds `dimension` to positions, and sends events with `streamId` and `seq` until
Takaro's cumulative `eventAck` confirms them. `TAKARO_PROTOCOL=0` in `TakaroConfig.txt` keeps version 0.

The map is one Dimension, `the_island` (`takaro/map.lua`), with `source: none`: the server ships no map art, so the
operator uploads the in-game map texture (`/Game/ConZ_Files/UI/HUD/IslandMap`, 2048 x 2048, stored upside down) in
Takaro. The bounds are the server's `AConZWorldSettings.WorldBounds` (read at runtime; -954800..669200 cm on
1.3.4.2) inset by 50,000 cm on every side, which is the 5 x 5 sector grid at 304,800 cm per sector that the map
image frames. World X grows to the left of the map and Y grows up. A live check on 1.3.4.2 put the player dot
within about 2 m of the in-game marker at four spots spread over the island; `tests/test_v1.lua` keeps those pairs.
After a SCUM update, compare `GET /gameserver/{id}/map` with the in-game map at two spots.

## A new SCUM build

The Lua side finds classes and functions by name, so a new build usually keeps working. Before a new
target is added: install the build, run the connector with a real client, and recheck at least join,
chat, getPlayers, teleport, giveItem, kick and ban. Then add `catalog/scum/targets/windows-<buildid>.json`
with the new depot manifests and file hashes.
