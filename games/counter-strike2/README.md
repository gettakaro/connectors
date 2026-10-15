# Takaro Counter-Strike 2 Connector

This is a server-side CounterStrikeSharp plugin. Players do not install anything. The plugin owns the outbound Takaro WebSocket; there is no sidecar or inbound connector port.

## Release status

**0.1.0 candidate, not approved for publication.** The imported prototype and the first native expansion build, but the current Linux dedicated server has not passed a real-client acceptance run. CounterStrikeSharp `Server.ExecuteCommand` returns no command output, so `executeConsoleCommand` supports read-only allowlisted ConVar queries through the server API and refuses other commands instead of inventing `rawResult`. Shop quantities above one are refused because CS2 weapons are individual equipped items. Do not infer live support from this source or the old July smoke run.

## Install

### Linux x86-64 dedicated server

1. Install the exact CS2 Steam build named by the [catalog target](../../catalog/counter-strike2/targets/). The dedicated server uses Steam app 730. Keep the server updated only after a new catalog target and live proof are available.
2. Install Metamod:Source **2.0.0.1481** and CounterStrikeSharp **1.0.376 with runtime** into `game/csgo/addons/`. This plugin was compiled against `CounterStrikeSharp.API` **1.0.376** and .NET **10**. Follow the [CounterStrikeSharp installation guide](https://docs.cssharp.dev/docs/guides/getting-started.html), including the Metamod `gameinfo.gi` SearchPaths entry. These frameworks are server dependencies and are not bundled in the Takaro archive.
3. Extract the release ZIP so that `TakaroCs2.dll` and `Takaro.Cs2.Core.dll` are under `game/csgo/addons/counterstrikesharp/plugins/TakaroCs2/`.
4. Copy `TakaroCs2.example.json` to `game/csgo/addons/counterstrikesharp/configs/plugins/TakaroCs2/TakaroCs2.json`. Set a unique stable `IdentityToken` and the Takaro `RegistrationToken` for initial registration. Keep the config and token out of source control. The plugin creates its durable ban file under `game/csgo/addons/counterstrikesharp/data/TakaroCs2/bans.json`; preserve that directory on upgrades.
5. Restart the server. Confirm `meta list`, `css_plugins list`, and the plugin's identify response log. Set `+sv_hibernate_when_empty 0` while testing idle server requests. For the Linux runtime, put `game/csgo/bin/linuxsteamrt64` before `game/bin/linuxsteamrt64` in `LD_LIBRARY_PATH`.

`AllowShutdown` defaults to false. Enable it only for a disposable server where a Takaro shutdown action is intended to stop the process. Console queries use an exact/prefix allowlist. The input must be one ConVar name, and the returned `rawResult` is its current server value. Mutating commands and unavailable ConVars return `success:false` with `errorMessage`.

## What works, what doesn't

The target is CS2 build **25947352** on Linux x86-64. As of **2026-10-15**, no behavior in this candidate has passed the current live server and real-client acceptance run. ✅ means live verified, ⚠️ means implemented but not yet live verified, and ❌ means unavailable.

| Capability | Status | Current evidence |
| --- | --- | --- |
| Reachability, Steam identity, player lookup | ⚠️ | Native implementation and protocol tests; real-client proof pending |
| Chat, location, inventory, item catalog, item delivery, teleport | ⚠️ | Native implementation; Takaro and client side effects pending |
| Read-only allowlisted console ConVar query | ⚠️ | Native server API implementation; real server result pending |
| Kick, durable bans, unban, shutdown gate | ⚠️ | Native implementation and persistence tests; real-client proof pending |
| Connect, disconnect, chat, death, nonplayer kill, log events | ⚠️ | Hook implementation; Takaro event rows pending |
| General console command output | ❌ | CounterStrikeSharp execution API does not expose output |
| Entity and named location catalog, map tiles | ❌ | Integration feasibility not yet established |

## Capability accounting

[capabilities.json](capabilities.json) is the machine-readable record. An implementation without a real client and Takaro-side effect remains unverified. The native paths implemented here are: authenticated SteamID64 player enumeration and lookup, global and recipient chat, spawned-pawn location, equipped weapon inventory, a curated weapon catalog, one-item delivery, teleport, kick, durable SteamID64 bans with expiry, and a gated shutdown. Hook paths emit connect, disconnect, chat, death, and log events. All need a current real-client run before being claimed as live supported.

`entity-killed` uses CounterStrikeSharp's `EventOtherDeath` hook and needs real nonplayer-kill proof. `listEntities`, `listLocations`, `getMapInfo`, and `getMapTile` still require a game API and live feasibility decision. Those actions return a definite `unsupported_action` today. General console command output remains unavailable through CounterStrikeSharp. A full acceptance run must also cover a client death, reconnect and restart, network outage recovery, modules, Discord bridge, shop and economy, and an A/B server tick measurement.

## Build and test

From the repository root:

```bash
docker run --rm -v "$PWD:/repo" -w /repo/games/counter-strike2/mod mcr.microsoft.com/dotnet/sdk:10.0 dotnet test Takaro.Cs2.sln
bash games/counter-strike2/scripts/build-release.sh 0.1.0 /tmp/cs2-release --target linux-25947352
python3 -m zipfile -l /tmp/cs2-release/takaro-counter-strike2-linux-25947352-0.1.0.zip
```

The catalog and release script pin the build toolchain and framework inputs. Keep `.runtime` server and proof data out of Git.
