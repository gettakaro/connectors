# ARK: Survival Ascended connector: development

The connector is the C++ AsaApi plugin `TakaroAsa`, loaded by AsaApi inside `ArkAscendedServer.exe` from
`ShooterGame/Binaries/Win64/ArkApi/Plugins/TakaroAsa/`. It holds the Takaro WebSocket itself:

```
native/src/plugin.cpp          the game half: Plugin_Init, hooks, one function per Takaro action, the tick callback
native/src/takaro_client.*     the Takaro connection (protocol v0 and v1), free of any game or AsaApi type
native/src/logic.*             rules and durable state that need no game object (console refusals, bans, known players)
native/src/transport*          the WebSocket over WinHTTP, shared design with the other native connectors
native/src/tj_json.hpp         strict JSON DOM for the protocol
native/build.bat               MSVC build of TakaroAsa.dll
native/PROVENANCE.md           where third-party and ported code comes from
tests/                         game-free tests, plus the capabilities.json drift check
scripts/fetch-deps.sh          fetches the pinned AsaApi headers, AsaApi.lib and fmt
scripts/build-release.sh       packages the release zip, driven by catalog/ark-ascended/targets/<target>.json
```

## Build and test

```
tests/run.sh                         # client and logic tests with ASan and UBSan; needs g++ with C++20
python3 tests/check_capabilities.py  # capabilities.json against plugin.cpp
```

`client_test.cpp` drives the real `takaro::Client` against a fake transport; `logic_test.cpp` covers `logic.cpp`.
`plugin.cpp` needs the game and has no unit tests.

The DLL builds only with MSVC, because AsaApi's headers and `AsaApi.lib` target it. On Windows, with Visual Studio
2022 and the MSVC 14.44 toolset (bash from Git for Windows, plus `jq` and Python):

```
scripts/fetch-deps.sh --target windows-26334129 --dest C:/asa-deps
set ASA_DEPS=C:\asa-deps
native\build.bat
```

The build uses AsaApi's headers at a pinned commit with `AsaApi.lib` from the pinned release: the release's own
headers are older than the current game. Both pins and the fmt version are in the catalog target.

## Release build

`.github/workflows/ark-ascended.yml` runs the tests, compiles `TakaroAsa.dll` on `windows-2022` with `build.bat`
(version stamped through `TAKARO_VERSION`) and hands it to the shared `connector-release.yml` through
`prebuilt_artifact`, which puts it in `_data/prebuilt/<target>/`. `build-release.sh` then runs on Linux. It compiles
nothing: it checks that the DLL is x64, exports `Plugin_Init` and carries this version, checks the AsaApi release zip
against the catalog and writes `takaro-ark-ascended-<target>-<version>.zip` plus its `.meta.json`. Two packaging runs
over the same DLL give identical bytes. The MSVC build itself is not claimed to be reproducible.

```
TAKARO_ASA_DLL=<path>/TakaroAsa.dll maintenance/bin/takaro-maint build --game ark-ascended --target windows-26334129 --version 0.1.0-dev --out dist --toolchain container
```

The zip mirrors `ShooterGame/Binaries/Win64`: the AsaApi release without `Lib/` and the Permissions developer folder,
its licence as `ArkApi/LICENSE-AsaApi.txt`, and `ArkApi/Plugins/TakaroAsa/`.

## Architecture notes

- **The game thread.** Everything that touches a game object runs on the game thread, from the AsaApi tick callback or
  a hook, with a 2 ms budget per tick for queued Takaro actions. Network and JSON work stays on the client's worker
  threads. `TAKARO_WIRE_DEBUG=1` logs what the connector costs the game thread once a minute.
- **Connecting late.** The connector calls `Client::Start` only when AsaApi reports the server as Ready, because
  Takaro reads the item and entity catalogues in the first seconds after a server registers. Items, entities and
  locations are snapshotted once at that point, before anyone can be on the server.
- **Offsets.** AsaApi resolves game functions and fields from an offset cache keyed on the sha256 of
  `ArkAscendedServer.exe`, downloaded from community CDNs. The server ships no PDB, so there is no local fallback. A
  field or function missing from the cache is a hard crash, so check `ArkApi/Cache/generations/*/cached_offsets.txt`
  before using any header name.
- **Console.** `UEngine::Exec` crashes a dedicated server. Console commands go through the RCON cheat manager, which
  exists only with `RCONEnabled=True`; the RCON port is never used. Output comes from the static
  `URCONServer::SetClientMessage(FString)` and `UShooterCheatManager::PrintMessageOut`, hooked around the call.
- **Shutdown.** `DoExit` saves by itself; a second save just before it can corrupt the world file. The game runs no
  `Logout` for players online at exit, so the connector reports them as left first.
- **FString by value.** `BanPlayer` and `UnbanPlayer` take FString by value, which on x64 is a pointer the callee
  destroys. Pass a heap copy (`CalleeOwned`) and never destroy it.
- **Bans.** The game holds every ban as permanent; `state/bans.json` keeps the reason and end time, and the connector
  lifts a timed ban itself, so one clock decides. A banned player reaches `HandleNewPlayer` before the game refuses
  them, so the join hook checks the bans before it reports a join.
- **Player identity.** `gameId` and `epicOnlineServicesId` are the EOS id. The game exposes no Steam id. `platformId`
  and `steamId` are not sent.
- **Map.** On `TheIsland_WP` the manifest declares one Dimension with `source none`, so the operator uploads the
  picture. Bounds are +-342900: world 0 is 50 on the in-game map and world 100000 is 64.58. No other map is calibrated.

## A new ARK build

Before a new target is added: wait for the offset cache of the new exe hash, install the build, run the connector with
a real client, and recheck at least join, chat, getPlayers, teleport, giveItem, kick, ban and a creature kill. AsaApi's
headers may also need a newer commit. Then add `catalog/ark-ascended/targets/windows-<buildid>.json` with the new depot
manifests and the exe hash (`takaro-maint steam pin --game ark-ascended --record-files
ShooterGame/Binaries/Win64/ArkAscendedServer.exe`).
