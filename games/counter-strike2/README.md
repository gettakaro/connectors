# Takaro Counter-Strike 2 Connector

This is a server-side CounterStrikeSharp plugin. Players do not install anything. The plugin owns the outbound Takaro WebSocket; there is no sidecar or inbound connector port.

## Release status

**0.1.0 candidate, not approved for publication.** A real CS2 client and production Takaro confirmed the core actions, server restart, scoped network recovery, module commands, and both Discord chat directions on 2026-10-15. The lifecycle retest on 2026-10-16 recorded one connect and one disconnect event after moving connect emission to spawn and retaining the last observed player position for Takaro's event processing. A real client killed a chicken and Takaro stored `entity-killed` with the authenticated attacker and USP-S. A Takaro hook executed on the plugin startup log. Shop delivery also appeared in Takaro inventory history. Engine frame-time measurement remains open. CounterStrikeSharp `Server.ExecuteCommand` returns no command output, so `executeConsoleCommand` supports read-only allowlisted ConVar queries through the server API and refuses other commands instead of inventing `rawResult`. Shop quantities above one are refused because CS2 weapons are individual equipped items. See the private proof page linked from the CS2 control panel entry for current check status.

The catalog requires gameplay verification. The release assembler refuses that level until the remaining real-client checks pass, the evidence is reviewed, and the target is explicitly approved for publication.

## Install

### Linux x86-64 dedicated server

1. Install the exact CS2 Steam build named by the [catalog target](../../catalog/counter-strike2/targets/). The dedicated server uses Steam app 730. Keep the server updated only after a new catalog target and live proof are available.
2. Install Metamod:Source **2.0.0.1481** and CounterStrikeSharp **1.0.376 with runtime** into `game/csgo/addons/`. This plugin was compiled against `CounterStrikeSharp.API` **1.0.376** and .NET **10**. The Linux host also needs ICU shared libraries (`libicu`); the test rig used ICU 74 from the pinned .NET 10 SDK image. Follow the [CounterStrikeSharp installation guide](https://docs.cssharp.dev/docs/guides/getting-started.html), including the Metamod `gameinfo.gi` SearchPaths entry. These frameworks are server dependencies and are not bundled in the Takaro archive.
3. Extract the release ZIP so that `TakaroCs2.dll` and `Takaro.Cs2.Core.dll` are under `game/csgo/addons/counterstrikesharp/plugins/TakaroCs2/`.
4. Copy `TakaroCs2.example.json` to `game/csgo/addons/counterstrikesharp/configs/plugins/TakaroCs2/TakaroCs2.json`. Set a unique stable `IdentityToken` and the Takaro `RegistrationToken` for initial registration. Keep the config and token out of source control. The plugin creates its durable ban file under `game/csgo/addons/counterstrikesharp/data/TakaroCs2/bans.json`; preserve that directory on upgrades.
5. Restart the server. Confirm `meta list`, `css_plugins list`, and the plugin's identify response log. Set `+sv_hibernate_when_empty 0` while testing idle server requests. For the Linux runtime, put `game/csgo/bin/linuxsteamrt64` before `game/bin/linuxsteamrt64` in `LD_LIBRARY_PATH`.

`AllowShutdown` defaults to false. Enable it only for a disposable server where a Takaro shutdown action is intended to stop the process. Console queries use an exact/prefix allowlist. The input must be one ConVar name, and the returned `rawResult` is its current server value. Mutating commands and unavailable ConVars return `success:false` with `errorMessage`.

## What works, what doesn't

The target is CS2 build **25947352** on Linux x86-64. As of **2026-10-16**, core actions and both lifecycle events have real-client proof, while release acceptance remains open. ✅ means live verified, ⚠️ means implemented but not yet live verified, and ❌ means unavailable.

| Capability | Status | Current evidence |
| --- | --- | --- |
| Reachability, Steam identity, player lookup | ✅ | Production Takaro MCP and authenticated SteamID64 client |
| Chat, location, inventory, item catalog, item delivery, teleport | ✅ | Takaro JSON, movement/inventory readback, and game window |
| Read-only allowlisted console ConVar query | ✅ | Real server value and rejected command in Takaro JSON |
| Kick, durable bans, unban | ✅ | Client disconnect, persisted SteamID64 ban across restart, blocked reconnect, then unban |
| Shutdown gate | ✅ | Default gate refused shutdown and server stayed available; enabled path is untested |
| Connect, disconnect, chat, player death events | ✅ | Current Takaro event rows from real client; one connect and disconnect row in the lifecycle retest |
| Nonplayer kill | ✅ | Real client killed a chicken; Takaro stored attacker, entity, and weapon |
| Log event | ✅ | A Takaro hook executed on the plugin startup log and recorded the message |
| General console command output | ❌ | CounterStrikeSharp execution API does not expose output |
| Entity and named location catalog, map tiles | ❌ | Integration feasibility not yet established |

## Capability accounting

[capabilities.json](capabilities.json) is the machine-readable record. Current live proof covers authenticated SteamID64 lookup, global and recipient chat, spawned-pawn location, equipped inventory, the 10-item catalog, one-item delivery, teleport, kick, durable SteamID64 bans with expiry, and the disabled-by-default shutdown gate. Connect, disconnect, chat, player death, and nonplayer kill events have Takaro rows. A startup log drove a Takaro hook. Takaro requests location before storing player events, so the plugin retains the last observed position for up to one day after a player leaves or dies; after expiry it returns `position_unavailable`.

`entity-killed` uses CounterStrikeSharp's `EventOtherDeath` hook; its chicken kill was verified through Takaro. `listEntities` and `listLocations` return empty arrays to satisfy Takaro's list response schema; they remain unsupported catalogs. `getMapInfo` and `getMapTile` still require a game API and live feasibility decision and return `unsupported_action`. General console command output remains unavailable through CounterStrikeSharp. A full acceptance run must also cover fresh event rows after restart and outage recovery, plus an A/B engine frame-time measurement.

## Build and test

From the repository root:

```bash
docker run --rm -v "$PWD:/repo" -w /repo/games/counter-strike2/mod mcr.microsoft.com/dotnet/sdk:10.0 dotnet test Takaro.Cs2.sln
bash games/counter-strike2/scripts/build-release.sh 0.1.0 /tmp/cs2-release --target linux-25947352
python3 -m zipfile -l /tmp/cs2-release/takaro-counter-strike2-linux-25947352-0.1.0.zip
```

The catalog and release script pin the build toolchain and framework inputs. Keep `.runtime` server and proof data out of Git.
