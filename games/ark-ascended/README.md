# Takaro ARK: Survival Ascended Connector

Connects an ARK: Survival Ascended dedicated server on Windows to Takaro. It is one AsaApi plugin
that runs inside `ArkAscendedServer.exe`; there is no extra program to run. Players do not install
anything.

## Install

### 1. Before you start

- An ARK: Survival Ascended dedicated server on **Windows** (Steam app 2430930) that you can stop
  and start. Running it on Linux through Proton is not tested.
- File access to its `ShooterGame\Binaries\Win64` folder, and control over the command that starts
  the server. A locked-down rented host without file access cannot use this connector.
- A Takaro game server of type **Generic** and its **registration token** (Takaro shows it when you
  add the server).

### 2. Download

Download the latest release from https://takaro.io/connectors/ark-ascended (or the newest ARK:
Survival Ascended release on the [GitHub releases page](https://github.com/gettakaro/connectors/releases)).
You need one file:

- `takaro-ark-ascended-windows-<build>-<version>.zip`, where `<build>` is the server's Steam build.

The zip holds the connector and the AsaApi plugin loader in the exact version the connector needs.
Each release has a `SHA256SUMS` file to check the download with.

### 3. Install

1. Stop the server.
2. Unzip the file into `ShooterGame\Binaries\Win64`. You get:

```
ShooterGame\Binaries\Win64\
    ArkAscendedServer.exe
    AsaApiLoader.exe
    config.json
    ArkApi\
        AsaApi.dll
        Plugins\TakaroAsa\
            TakaroAsa.dll
            PluginInfo.json
            TakaroConfig.txt.example
```

3. From now on, start the server with `AsaApiLoader.exe` instead of `ArkAscendedServer.exe`, with
   the same arguments. The map must stay the first argument, as in `TheIsland_WP?listen`.

If you already run AsaApi, back up your `config.json` first and put it back after unzipping.

### 4. Configure

Copy `ArkApi\Plugins\TakaroAsa\TakaroConfig.txt.example` to `TakaroConfig.txt` in the same folder
and fill it in:

| Key | What to put there |
|---|---|
| `TAKARO_REGISTRATION_TOKEN` | Your Takaro registration token. Required. |
| `TAKARO_SERVER_NAME` | The name Takaro shows for this server. |
| `TAKARO_SERVER_CHAT_NAME` | Sender name on messages from Takaro. Empty = `Server`. |
| `TAKARO_WS_URL` | Leave `wss://connect.takaro.io/`. |
| `TAKARO_IDENTITY_TOKEN` | Leave empty. The connector makes one and keeps it in `state\identity-token`; keep that file when you move or reinstall the server. |
| `TAKARO_CA_FILE` | Leave empty to use the Windows certificate store. |
| `TAKARO_WIRE_DEBUG` | `1` logs all Takaro traffic for troubleshooting. Default `0`. |

**Console commands and shut down.** Set this in `ShooterGame\Saved\Config\WindowsServer\GameUserSettings.ini`:

```
[ServerSettings]
RCONEnabled=True
```

The RCON port does not need to be open. Everything else works without this setting.

Start the server. Changes to `TakaroConfig.txt` need a restart.

**Map picture (optional).** On The Island, Takaro's map shows your players on a grid out of the
box. For the in-game map as background, upload a picture of the whole map once in Takaro: open the
game server's map, pick **The Island** and upload the image. Leave the bounds empty; the connector
gives Takaro the exact ones. The connector ships no picture. Other maps have no map in Takaro.

### 5. Check that it works

The connector connects only after the world has finished loading, so the server shows as offline
in Takaro during load. After that, `ArkApi\Plugins\TakaroAsa\takaro.log` shows:

```
takaro: identified gameServerId=…
```

The game server now shows as online in Takaro.

A missing token logs `no TAKARO_REGISTRATION_TOKEN in TakaroConfig.txt, not connecting`. A wrong
token logs `takaro: identify rejected`. No `takaro.log` at all means AsaApi did not load the plugin:
check that you started `AsaApiLoader.exe`, and see Known issues for game updates.

### 6. Upgrade

Stop the server, unzip the new release over `ShooterGame\Binaries\Win64` and start it with
`AsaApiLoader.exe`. `TakaroConfig.txt` and the `state` folder in `ArkApi\Plugins\TakaroAsa` stay as
they are. The zip replaces `config.json`, so if you changed it, back it up first and put it back.
After an ARK update, read Known issues first.

## What works, what doesn't

Game actions and events checked on game 97.17 (Steam build 26515168), 2026-11-09; shop and modules on game 96.9. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Connects once the world has loaded and reconnects by itself after a restart or network drop. |
| Player list | ✅ | Name, ping and IP. The id is the Epic (EOS) id; no Steam id is available. |
| Player location | ✅ | Live position; offline players give the last position seen since the server started. |
| Teleport | ✅ | Online players. |
| Player inventory | ⚠️ | Carried and equipped items of an online player. Account cosmetics are left out. |
| Item list | ✅ | All items, with their in-game names. |
| Entity list | ✅ | Creatures, with their in-game names. |
| Locations | ✅ | The map's player spawn points, as "Spawn point 1" and so on. |
| World map | ⚠️ | Map bounds are built in for The Island only, and the picture is a one-time upload (step 4). |
| Send a message | ✅ | To everyone or to one online player. |
| Console commands | ⚠️ | Needs `RCONEnabled=True`. Output comes back for cheat-manager commands. |
| Kick | ✅ | The player sees the reason. |
| Ban and unban | ✅ | Also offline and timed; a banned player is refused on rejoin, also after a restart. |
| Ban list | ✅ | Also bans made in the game; for those the reason and end time are unknown. |
| Give an item | ✅ | To an online player, by item class path, with amount and quality. |
| Shop | ✅ | Buying, orders and claims work. Items land like Give an item. |
| Shut down the server | ⚠️ | Needs `RCONEnabled=True`. The game saves by itself on the way out. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Tribe chat arrives as "team", all other chat as "global". |
| Player death | ✅ | With position, and the killer when it was another player. |
| Entity killed | ⚠️ | Kills by a player directly, with creature and weapon names. A kill by a tamed creature is not reported. |
| Server log lines | ✅ | The game's own lines; lines with addresses or credentials are held back. |
| Discord chat bridge | ✅ | Both ways; use `chatBridgeNoEcho` to avoid echoes. |
| Modules | ✅ | |
| Game name, version and icon | ✅ | |

## Known issues

- **Every ARK server update stops the connector for a while.** AsaApi needs an offset cache for the
  exact `ArkAscendedServer.exe`, which it downloads by file hash from community-run servers. The
  server ships no debug files to make one from, so the connector stays down until the community
  has published the cache for the new build. Takaro cannot speed this up. If you want no gap, hold
  back server updates until the cache is out.
- A teleport far above the map can remove the character without a death, and Takaro still reports
  success. Keep teleport heights close to the ground.
- Only `-NoBattlEye` is known to work. BattlEye on is unchecked.
- Players are known by their Epic (EOS) id. There is no Steam id, so anything in Takaro keyed on
  Steam ids does not match ARK players.
- Without `RCONEnabled=True`, console commands and shut down answer with an error. Nothing else is
  affected.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
