# Takaro Counter-Strike 2 Connector

This is a server-side CounterStrikeSharp plugin. Players do not install anything. The plugin owns the outbound Takaro WebSocket; there is no sidecar or inbound connector port.

## Release status

**0.1.0 candidate, not approved for publication.** The current target is Linux x86-64 CS2 build 26087496 with Metamod:Source 2.0.0.1483 and CounterStrikeSharp 1.0.377. The previous build 25966348 passed core actions, moderation persistence, lifecycle events, economy, Discord, shop delivery, network recovery and idle tick timing. Map support is implemented from server-owned assets and awaits current-build live proof. CounterStrikeSharp `Server.ExecuteCommand` returns no command output, so `executeConsoleCommand` supports read-only allowlisted ConVar queries and refuses other commands instead of inventing `rawResult`. Shop quantities above one are refused because CS2 weapons are individual equipped items.

The catalog requires gameplay verification. The release assembler refuses that level until the remaining real-client checks pass, the evidence is reviewed, and the target is explicitly approved for publication.

## Install

### Linux x86-64 dedicated server

1. Install the exact CS2 Steam build named by the [catalog target](../../catalog/counter-strike2/targets/). The dedicated server uses Steam app 730. Keep the server updated only after a new catalog target and live proof are available.
2. Install Metamod:Source **2.0.0.1483** and CounterStrikeSharp **1.0.377 with runtime** into `game/csgo/addons/`. This plugin was compiled against `CounterStrikeSharp.API` **1.0.377** and .NET **10**. The Linux host also needs ICU shared libraries (`libicu`). Follow the [CounterStrikeSharp installation guide](https://docs.cssharp.dev/docs/guides/getting-started.html), including the Metamod `gameinfo.gi` SearchPaths entry. These frameworks are server dependencies and are not bundled in the Takaro archive.
3. Extract the release ZIP so that `TakaroCs2.dll`, `Takaro.Cs2.Core.dll`, `StbImageSharp.dll`, and `StbImageWriteSharp.dll` are under `game/csgo/addons/counterstrikesharp/plugins/TakaroCs2/`.
4. Copy `TakaroCs2.example.json` to `game/csgo/addons/counterstrikesharp/configs/plugins/TakaroCs2/TakaroCs2.json`. Set a unique stable `IdentityToken` and the Takaro `RegistrationToken` for initial registration. Keep the config and token out of source control. The plugin creates its durable ban file under `game/csgo/addons/counterstrikesharp/data/TakaroCs2/bans.json`; preserve that directory on upgrades.
5. Prepare map assets on the server with the bundled `prepare-maps.sh` and Source 2 Viewer CLI 20.1. The script extracts radar PNGs, overview calibration, and spawn entities from the installed CS2 VPKs into `addons/counterstrikesharp/data/TakaroCs2/maps/`; Valve assets are never included in the release ZIP. Re-run it after CS2 map updates. For example:

   ```bash
   curl -fL -o cli-linux-x64.zip https://github.com/ValveResourceFormat/ValveResourceFormat/releases/download/20.1/cli-linux-x64.zip
   echo "29d25a6d6b62377fc5386c3e5145b0a2180a401a50041afe9494b3f503deaef5  cli-linux-x64.zip" | sha256sum -c -
   unzip cli-linux-x64.zip -d source2viewer-cli
   chmod +x source2viewer-cli/Source2Viewer-CLI
   bash game/csgo/addons/counterstrikesharp/plugins/TakaroCs2/prepare-maps.sh "$CS2_SERVER_ROOT" "$PWD/source2viewer-cli/Source2Viewer-CLI" de_dust2
   ```

   Set `CS2_SERVER_ROOT` to the dedicated server install root before running the command. A map without prepared assets returns a definite unavailable result.
6. Restart the server. Confirm `meta list`, `css_plugins list`, and the plugin's identify response log. Set `+sv_hibernate_when_empty 0` while testing idle server requests. For the Linux runtime, put `game/csgo/bin/linuxsteamrt64` before `game/bin/linuxsteamrt64` in `LD_LIBRARY_PATH`.

`AllowShutdown` defaults to false. Enable it only for a disposable server where a Takaro shutdown action is intended to stop the process. Console queries use an exact/prefix allowlist. The input must be one ConVar name, and the returned `rawResult` is its current server value. Mutating commands and unavailable ConVars return `success:false` with `errorMessage`.

## What works, what doesn't

The target is CS2 build **26087496** on Linux x86-64. The rows below summarize proven behavior on build 25966348. ⚠️ means implemented but not yet live verified on build 26087496; ❌ means unavailable.

| Capability | Status | Current evidence |
| --- | --- | --- |
| Reachability | ⚠️ | Updated server identified through Takaro and reported de_dust2 |
| Steam identity, player lookup | ⚠️ | Takaro returned the authenticated real client as SteamID64 |
| Chat, location, inventory, item delivery, teleport | ⚠️ | Takaro results, inventory and position readback, and real-client view |
| Item catalog | ⚠️ | Current-build Takaro sync returned the curated ten-item catalog |
| Read-only allowlisted console ConVar query | ⚠️ | Takaro returned the server's `sv_cheats` value and refused an unlisted command |
| Kick, durable bans, unban | ⚠️ | Client disconnect, persisted ban after restart, blocked rejoin, then unban and rejoin |
| Shutdown gate | ⚠️ | Takaro received `shutdown_disabled`; enabled path is untested |
| Connect, disconnect, chat, player death events | ⚠️ | Real-client event rows and one connect/disconnect per completed session |
| Nonplayer kill | ⚠️ | Real client killed a spawned Chicken; Takaro recorded the attacker and weapon |
| Entity catalog | ⚠️ | Current-build Takaro sync returned the neutral Chicken entry |
| Log event | ⚠️ | Takaro executed an installed hook on the current plugin startup event |
| Economy, Discord bridge, shop delivery | ⚠️ | Module and hook executions, completed shop order, inventory history and client item view |
| Network recovery | ⚠️ | Scoped WebSocket outage, automatic reconnect, real-client message after recovery |
| Idle server tick timing | ⚠️ | Two loaded and two unloaded VProf captures on the same empty server; 0.0075 ms observed difference per tick |
| General console command output | ❌ | CounterStrikeSharp execution API does not expose output |
| Named spawn locations, map tiles | ⚠️ | Game-owned spawn entities, radar image and overview calibration are prepared on the server; current-build Takaro/client proof is pending |

## Capability accounting

[capabilities.json](capabilities.json) is the machine-readable record. The connector implements authenticated SteamID64 lookup, global and recipient chat, spawned-pawn location, equipped inventory, a 10-item catalog, one-item delivery, teleport, kick, durable SteamID64 bans with expiry, and a disabled-by-default shutdown gate. Takaro requests location before storing player events, so the plugin retains the last observed position for up to one day after a player leaves or dies; after expiry it returns `position_unavailable`.

`entity-killed` uses CounterStrikeSharp's `EventOtherDeath` hook. `listEntities` returns a curated Chicken entry. `listLocations` returns the representative T and CT spawn points from the current map's server-owned entity data. `getMapInfo` enables the current map only when its radar PNG, overview coordinates and entity data have been prepared. `getMapTile` returns calibrated 128-pixel PNG tiles as base64; missing assets and out-of-range tiles return explicit errors. General console command output remains unavailable through CounterStrikeSharp. Map claims remain pending until current-build Takaro and real-client checks pass.

## Build and test

From the repository root:

```bash
docker run --rm -v "$PWD:/repo" -w /repo/games/counter-strike2/mod mcr.microsoft.com/dotnet/sdk:10.0 dotnet test Takaro.Cs2.sln
bash games/counter-strike2/scripts/build-release.sh 0.1.0 /tmp/cs2-release --target linux-26087496
python3 -m zipfile -l /tmp/cs2-release/takaro-counter-strike2-linux-26087496-0.1.0.zip
```

The catalog and release script pin the build toolchain and framework inputs. Keep `.runtime` server and proof data out of Git.
