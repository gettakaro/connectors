# Takaro Counter-Strike 2 Connector

Connects a Counter-Strike 2 dedicated server on Linux to Takaro. It is a CounterStrikeSharp plugin that runs inside the server; there is no extra program to run. Players do not install anything.

## Install

### 1. Before you start

- A Linux x86-64 Counter-Strike 2 dedicated server (Steam app `730`) that you can stop, start and copy files into. Use the exact build named on the download page, and update only when a new connector release supports the new build.
- Metamod:Source and CounterStrikeSharp with runtime, in the versions named on the download page. Follow the [CounterStrikeSharp guide](https://docs.cssharp.dev/docs/guides/getting-started.html), including the Metamod `gameinfo.gi` entry. The Linux host also needs the ICU libraries (`libicu`).
- A Takaro game server of type Generic and its registration token.

### 2. Download

Download the latest release from https://takaro.io/connectors/counter-strike2. Pick the file for your server build: `takaro-counter-strike2-<target>-<version>.zip`. Compare it with `SHA256SUMS` if you want to check the download.

### 3. Install

Extract the ZIP so the plugin lands here:

```text
game/csgo/addons/counterstrikesharp/plugins/TakaroCs2/
  TakaroCs2.dll
  Takaro.Cs2.Core.dll
  StbImageSharp.dll
  StbImageWriteSharp.dll
```

For the world map and spawn locations, prepare the map files once with the bundled script and Source 2 Viewer CLI 20.1. They come from your own server install; Valve's files are never in the ZIP. Run it again after a CS2 map update:

```bash
curl -fL -o cli-linux-x64.zip https://github.com/ValveResourceFormat/ValveResourceFormat/releases/download/20.1/cli-linux-x64.zip
echo "29d25a6d6b62377fc5386c3e5145b0a2180a401a50041afe9494b3f503deaef5  cli-linux-x64.zip" | sha256sum -c -
unzip cli-linux-x64.zip -d source2viewer-cli
chmod +x source2viewer-cli/Source2Viewer-CLI
bash game/csgo/addons/counterstrikesharp/plugins/TakaroCs2/prepare-maps.sh "$CS2_SERVER_ROOT" "$PWD/source2viewer-cli/Source2Viewer-CLI" de_dust2
```

Set `CS2_SERVER_ROOT` to the server install folder first. On Linux, put `game/csgo/bin/linuxsteamrt64` before `game/bin/linuxsteamrt64` in `LD_LIBRARY_PATH`.

### 4. Configure

Copy `TakaroCs2.example.json` to `game/csgo/addons/counterstrikesharp/configs/plugins/TakaroCs2/TakaroCs2.json`. Set `IdentityToken` to a unique name for this server and `RegistrationToken` to your Takaro registration token. Keep this file private.

`AllowShutdown` is off by default. Console commands only work for the names in `CommandAllowlistExact` and `CommandAllowlistPrefixes`. Start the server with `+sv_hibernate_when_empty 0` so Takaro can reach it while nobody is playing.

### 5. Check that it works

Restart the server. The console shows:

```text
[Takaro CS2] Takaro CS2 identified as gameServerId=<id>.
```

The game server then shows as online in Takaro. With a wrong or missing token the console shows `Takaro CS2 identification failed` and the connector retries.

### 6. Upgrade

Stop the server, replace the files in `plugins/TakaroCs2/`, and start it again. Keep `configs/plugins/TakaroCs2/TakaroCs2.json` and the `data/TakaroCs2/` folder, which holds bans and map files.

## What works, what doesn't

Checked on CS2 build 26445667, 2026-11-06. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | Name, ping and IP; the id is the SteamID64. |
| Player location | ✅ | While the player is alive; afterwards the last known position for one day. |
| Teleport | ✅ | |
| Player inventory | ⚠️ | The equipped weapons; not the Steam inventory or skins. |
| Player levels and skills | ⚠️ | The CS2 profile rank, reported by the player's game when they join, so it can be spoofed and only changes on rejoin. Rank is earned in official matches. Experience is not visible to the server. |
| Item list | ✅ | Ten weapons and grenades. |
| Entity list | ✅ | The chicken. |
| Locations | ✅ | T and CT spawn, for maps with prepared files. |
| World map | ✅ | The radar image, for maps with prepared files. |
| Send a message | ✅ | To everyone or to one player. |
| Console commands | ⚠️ | Reads the value of a ConVar you allow; other commands are refused. |
| Kick | ✅ | |
| Ban and unban | ✅ | By SteamID64; bans survive a restart. |
| Ban list | ✅ | |
| Give an item | ⚠️ | One at a time; a weapon is lost when the round resets. |
| Shop | ⚠️ | Currency and orders work. A listing with a quantity above one fails delivery, so sell single items. |
| Shut down the server | ⚠️ | Off by default; turn it on with `AllowShutdown`. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Player chat as "global" or "team", and chat typed on the server console. |
| Player death | ⚠️ | With the position. The killer is reported for real players only, not bots. |
| Entity killed | ✅ | Chicken kills, with the weapon. |
| Server log lines | ⚠️ | Connector messages only, not the full server log. |
| Discord chat bridge | ⚠️ | Game chat reaches Discord. Discord to game has not been checked. |
| Modules | ✅ | Chat commands such as `@ping` work. |
| Game name, version and icon | ⚠️ | Name and version; no icon. |
| Restarts and outages | ✅ | Reconnects by itself; events from an outage arrive afterwards. |

## Known issues

- A new CS2 build can stop CounterStrikeSharp until it is updated, so keep the server on a supported build.
- Events sent after an outage show the time they reached Takaro, not the time they happened.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
