# Takaro StarRupture Connector

A plugin for the StarRupture dedicated server on Windows. It runs inside the server through ModLoader and connects to Takaro itself, so there is no extra program to run. Players do not install anything.

## Install

### 1. Before you start

- A StarRupture dedicated server on Windows (Steam app 3809400), at the build named in the release.
- File access to the server's `StarRupture\Binaries\Win64` folder.
- A Takaro game server of type Generic and its registration token.

### 2. Download

Download the latest release from https://takaro.io/connectors/starrupture. Pick the zip for your server build, `takaro-starrupture-<target>-<version>.zip`. `SHA256SUMS` lists the checksums.

### 3. Install

Stop the server and unzip into `StarRupture\Binaries\Win64`, next to `StarRuptureServerEOS-Win64-Shipping.exe`:

```
StarRupture\Binaries\Win64\
  dwmapi.dll
  ModLoader\
    StarRupture-ModLoader-Core.dll
    StarRupture-ModLoader-Core.pdb
    Plugins\
      TakaroStarRupture.dll
      config\TakaroStarRupture.example.ini
```

The zip includes ModLoader, which loads the plugin. If you already use ModLoader, keep it at the version in the release: it only runs on its own game build.

### 4. Configure

Copy `ModLoader\Plugins\config\TakaroStarRupture.example.ini` to `TakaroStarRupture.ini` in the same folder and fill it in:

```ini
[Takaro]
RegistrationToken=<your token>
ServerName=My StarRupture Server
```

Leave `IdentityToken` empty. The plugin fills it in on first start; keep it after that, or Takaro creates a new server.

Start the server. To reach a playable world without a client, set `LoadSavedGame` in `DSSettings.txt` next to the server exe.

### 5. Check that it works

`ModLoader\Logs\ModLoader.log` shows:

```
takaro: connected as game server <id>
```

The game server now shows as online in Takaro. A wrong token shows as `takaro: identify rejected: ...`. A missing token logs an error and the plugin stays offline.

### 6. Upgrade

Stop the server, unzip the new release over `StarRupture\Binaries\Win64` and start it. Keep `TakaroStarRupture.ini` and `TakaroStarRupture.bans`. After a StarRupture update, wait for a connector release that names the new server build: ModLoader refuses any other build.

## What works, what doesn't

Checked on StarRupture 0.3.5 (Steam build 25642172), 2026-11-06. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects by itself after a server restart or network drop. |
| Player list | ⚠️ | Name, ping, SteamID64 and Epic id. No IP address. |
| Player location | ✅ | Live position; the last position for a short while after leaving. |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Player levels and skills | ✅ | Movement, Combat and Survival, read from the server. The game has no overall player level. Skills belong to the player, so they stay the same when a player switches convict. |
| Item list | ✅ | The items in the game's item database, with their in-game names and descriptions. |
| Entity list | ⚠️ | Enemy types loaded in the world. The game has no display names for them, so names come from the internal ones (Melee Tier 1). |
| Locations | ✅ | Points of interest in the world. |
| World map | ❌ | The server has no map images. |
| Send a message | ⚠️ | The game has no private chat, so a message to one player is shown to everyone with `[to Name]`. Lines also show a small `<Player_4>:</>` prefix. |
| Console commands | ❌ | The server has no console Takaro can use. |
| Kick | ✅ | |
| Ban and unban | ✅ | Also for players who are offline. Bans are kept in `TakaroStarRupture.bans` and enforced by the plugin. |
| Ban list | ✅ | |
| Give an item | ✅ | Lands in the player's inventory. The game has no item quality, so Takaro's quality is ignored. |
| Shop | ✅ | Items land like Give an item. |
| Shut down the server | ✅ | The server saves first. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Player chat is reported as global. |
| Player death | ⚠️ | With position. The killer is not reported. |
| Entity killed | ⚠️ | Only with a weapon the plugin can name. |
| Server log lines | ❌ | Not sent. |
| Discord chat bridge | ✅ | Both ways; use `chatBridgeNoEcho` to avoid echoes. |
| Modules | ✅ | Chat commands, hooks and scheduled jobs work. |
| Game name, version and icon | ✅ | |
| Restarts and outages | ✅ | Takaro keeps the same server entry after a restart. |
| Game updates | ⚠️ | ModLoader only runs on its own game build, so after a StarRupture update wait for a connector release that names the new server build. |

## Known issues

- After a server is killed instead of stopped, clients can see "More than one server at this IP" for a while. Stopping the server normally avoids it.
- A player who stays idle in a world can starve and die; that is normal game behaviour and shows up as a death.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
