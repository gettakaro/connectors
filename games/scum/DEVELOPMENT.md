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

## A new SCUM build

The Lua side finds classes and functions by name, so a new build usually keeps working. Before a new
target is added: install the build, run the connector with a real client, and recheck at least join,
chat, getPlayers, teleport, giveItem, kick and ban. Then add `catalog/scum/targets/windows-<buildid>.json`
with the new depot manifests and file hashes.
