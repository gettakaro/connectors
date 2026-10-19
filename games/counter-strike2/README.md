# Takaro Counter-Strike 2 Connector

This is a server-side CounterStrikeSharp plugin. Players do not install anything. The plugin owns the outbound Takaro WebSocket; there is no sidecar or inbound connector port.

## Release status

**0.1.0 candidate, not approved for publication.** The current target is Linux x86-64 CS2 build 25966348 with Metamod:Source 2.0.0.1483 and CounterStrikeSharp 1.0.377. The plugin builds and connects to Takaro on this target. Real-client actions and events still require current-build verification. CounterStrikeSharp `Server.ExecuteCommand` returns no command output, so `executeConsoleCommand` supports read-only allowlisted ConVar queries and refuses other commands instead of inventing `rawResult`. Shop quantities above one are refused because CS2 weapons are individual equipped items.

The catalog requires gameplay verification. The release assembler refuses that level until the remaining real-client checks pass, the evidence is reviewed, and the target is explicitly approved for publication.

## Install

### Linux x86-64 dedicated server

1. Install the exact CS2 Steam build named by the [catalog target](../../catalog/counter-strike2/targets/). The dedicated server uses Steam app 730. Keep the server updated only after a new catalog target and live proof are available.
2. Install Metamod:Source **2.0.0.1483** and CounterStrikeSharp **1.0.377 with runtime** into `game/csgo/addons/`. This plugin was compiled against `CounterStrikeSharp.API` **1.0.377** and .NET **10**. The Linux host also needs ICU shared libraries (`libicu`). Follow the [CounterStrikeSharp installation guide](https://docs.cssharp.dev/docs/guides/getting-started.html), including the Metamod `gameinfo.gi` SearchPaths entry. These frameworks are server dependencies and are not bundled in the Takaro archive.
3. Extract the release ZIP so that `TakaroCs2.dll` and `Takaro.Cs2.Core.dll` are under `game/csgo/addons/counterstrikesharp/plugins/TakaroCs2/`.
4. Copy `TakaroCs2.example.json` to `game/csgo/addons/counterstrikesharp/configs/plugins/TakaroCs2/TakaroCs2.json`. Set a unique stable `IdentityToken` and the Takaro `RegistrationToken` for initial registration. Keep the config and token out of source control. The plugin creates its durable ban file under `game/csgo/addons/counterstrikesharp/data/TakaroCs2/bans.json`; preserve that directory on upgrades.
5. Restart the server. Confirm `meta list`, `css_plugins list`, and the plugin's identify response log. Set `+sv_hibernate_when_empty 0` while testing idle server requests. For the Linux runtime, put `game/csgo/bin/linuxsteamrt64` before `game/bin/linuxsteamrt64` in `LD_LIBRARY_PATH`.

`AllowShutdown` defaults to false. Enable it only for a disposable server where a Takaro shutdown action is intended to stop the process. Console queries use an exact/prefix allowlist. The input must be one ConVar name, and the returned `rawResult` is its current server value. Mutating commands and unavailable ConVars return `success:false` with `errorMessage`.

## What works, what doesn't

The target is CS2 build **25966348** on Linux x86-64. Current-build real-client acceptance is in progress. ✅ means verified on this target, ⚠️ means implemented but not yet live verified on this target, and ❌ means unavailable.

| Capability | Status | Current evidence |
| --- | --- | --- |
| Reachability | ✅ | Updated server identified through Takaro and reported de_dust2 |
| Steam identity, player lookup | ⚠️ | Requires a real-client join on this target |
| Chat, location, inventory, item delivery, teleport | ⚠️ | Requires current-build action and readback checks |
| Item catalog | ✅ | Current-build Takaro sync returned the curated ten-item catalog |
| Read-only allowlisted console ConVar query | ✅ | Takaro returned the server's `sv_cheats` value and refused an unlisted command |
| Kick, durable bans, unban | ⚠️ | Requires current-build client effects and restart persistence |
| Shutdown gate | ✅ | Takaro received `shutdown_disabled`; enabled path is untested |
| Connect, disconnect, chat, player death events | ⚠️ | Requires current-build Takaro event rows |
| Nonplayer kill | ⚠️ | Requires a current-build real-client kill and Takaro event row |
| Entity catalog | ✅ | Current-build Takaro sync returned the neutral Chicken entry |
| Log event | ⚠️ | Requires a current-build Takaro hook execution |
| Idle server tick timing | ⚠️ | Requires current-build engine timing captures |
| General console command output | ❌ | CounterStrikeSharp execution API does not expose output |
| Named location catalog, map tiles | ❌ | No map-wide location catalog or validated server map raster source |

## Capability accounting

[capabilities.json](capabilities.json) is the machine-readable record. The connector implements authenticated SteamID64 lookup, global and recipient chat, spawned-pawn location, equipped inventory, a 10-item catalog, one-item delivery, teleport, kick, durable SteamID64 bans with expiry, and a disabled-by-default shutdown gate. Takaro requests location before storing player events, so the plugin retains the last observed position for up to one day after a player leaves or dies; after expiry it returns `position_unavailable`.

`entity-killed` uses CounterStrikeSharp's `EventOtherDeath` hook. `listEntities` returns a curated Chicken entry. `listLocations` returns `unsupported_action`: CounterStrikeSharp exposes a pawn's last place name, but no validated map-wide catalog with coordinates. `getMapInfo` reports `enabled:false` through Takaro's map status contract; `getMapTile` returns `unsupported_action` until a server overview raster can be decoded and calibrated. General console command output remains unavailable through CounterStrikeSharp. Named-location and map-tile feasibility remain release decisions.

## Build and test

From the repository root:

```bash
docker run --rm -v "$PWD:/repo" -w /repo/games/counter-strike2/mod mcr.microsoft.com/dotnet/sdk:10.0 dotnet test Takaro.Cs2.sln
bash games/counter-strike2/scripts/build-release.sh 0.1.0 /tmp/cs2-release --target linux-25966348
python3 -m zipfile -l /tmp/cs2-release/takaro-counter-strike2-linux-25966348-0.1.0.zip
```

The catalog and release script pin the build toolchain and framework inputs. Keep `.runtime` server and proof data out of Git.
