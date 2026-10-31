# Takaro SCUM Connector

Connects a SCUM dedicated server on Windows to Takaro. It is one UE4SS mod that runs inside
`SCUMServer.exe`; there is no extra program to run. Players do not install anything.

## Install

### 1. Before you start

- A SCUM dedicated server on **Windows** (Steam app 3792580) that you can stop and start.
- File access to its `SCUM\Binaries\Win64` folder (the one with `SCUMServer.exe`). On a rented
  host without file access, ask support to place the files.
- A Takaro game server of type **Generic** and its **registration token** (Takaro shows it when
  you add the server).

### 2. Download

Download the latest release from https://takaro.io/connectors/scum (or the newest SCUM release on
the [GitHub releases page](https://github.com/gettakaro/connectors/releases)). You need one file:

- `takaro-scum-windows-<build>-<version>.zip`, where `<build>` is the SCUM server's Steam build.

The zip holds the connector and the UE4SS mod loader in the exact version the connector needs,
already set up. Each release has a `SHA256SUMS` file to check the download with.

### 3. Install

1. Stop the server.
2. If you already use UE4SS, rename your `SCUM\Binaries\Win64\ue4ss` folder and `dwmapi.dll` first.
3. Unzip the file into `SCUM\Binaries\Win64`. You get:

```
SCUM\Binaries\Win64\
    SCUMServer.exe
    dwmapi.dll
    ue4ss\
        UE4SS-settings.ini
        Mods\TakaroSCUM\
            TakaroConfig.txt.example
```

Do not change the `[Hooks]` values in `ue4ss\UE4SS-settings.ini`: with UE4SS's default hooks the
server crashes when a player joins.

### 4. Configure

Copy `ue4ss\Mods\TakaroSCUM\TakaroConfig.txt.example` to `TakaroConfig.txt` in the same folder and
fill it in:

| Key | What to put there |
|---|---|
| `REGISTRATION_TOKEN` | Your Takaro registration token. Required. |
| `SERVER_NAME` | The name Takaro shows for this server. |
| `IDENTITY_TOKEN` | Leave empty. The connector makes one and keeps it in `TakaroIdentity.txt`; keep that file when you move or reinstall the server. |
| `TAKARO_WS_URL` | Leave `wss://connect.takaro.io/`. |
| `COMMAND_ALLOWLIST` | Commands Takaro may run, comma separated, without `#`, e.g. `announce,listplayers`. Empty = none. |
| `ENABLE_DESTRUCTIVE_COMMANDS` | `true` lets Takaro shut the server down. Default `false`. |
| `LOG_FRAMES` | `true` logs all Takaro traffic for troubleshooting (tokens are hidden). Default `false`. |
| `SERVER_CHAT_NAME` | Optional sender name on messages from Takaro (see below). Empty = no name. |
| `TAKARO_PROTOCOL` | Leave `1`. Set `0` only if Takaro refuses the connection with `no_shared_protocol_version`. |

Start the server. For console commands other than `announce` and `listplayers`, an admin from
`AdminUsers.ini` must be online.

**Map picture (optional).** Takaro's map shows your players on a grid out of the box. For the
in-game map as background, upload the picture once in Takaro: open the game server's map, pick
**The Island** and upload the image. Leave the bounds empty; the connector gives Takaro the exact
ones. The server files contain no map picture and the connector ships none. Export it from a SCUM
**client** install with an Unreal asset viewer such as FModel (the game's paks are encrypted; the
viewer needs SCUM's AES key):

| Texture (client paks) | Size |
|---|---|
| `SCUM/Content/ConZ_Files/UI/HUD/IslandMap` (in `pakchunk14-WindowsNoEditor.pak`) | 2048 × 2048 |

The texture is stored upside down. Turn it by 180° before the upload (any image editor, or
`magick IslandMap.png -rotate 180 island.png`), so sector D4 is top left and Z0 bottom right. Do
not crop it or add borders; a smaller copy of the same full image works too.

**Server chat name (optional).** SCUM server lines have no sender field, so a sender shows as `Name: message`. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `SERVER_CHAT_NAME` instead; the name set in Takaro wins over it.

### 5. Check that it works

After the world has loaded (one to two minutes), `ue4ss\Mods\TakaroSCUM\takaro.log` shows:

```
identified with Takaro as game server …
```

The game server now shows as online in Takaro. If it does not, `takaro.log` and
`ue4ss\UE4SS.log` say why.

### 6. Upgrade

Stop the server, unzip the new release over `SCUM\Binaries\Win64` (it replaces `Scripts\` and
`dlls\main.dll`) and start it. Keep `TakaroConfig.txt` and `TakaroIdentity.txt`. After a SCUM
update, wait for a connector release that names the new game build.

## What works, what doesn't

Checked on SCUM 1.3.5.1.172191 (Steam build 26280049), 2026-10-29. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects by itself after a server restart or network drop; events from the outage are sent afterwards. |
| Player list | ✅ | Name, ping and IP; the id is the SteamID64. Offline players answer with their last known data. |
| Player location | ✅ | Live position; offline players give their last position. |
| Teleport | ✅ | The player is moved at once. |
| Player inventory | ✅ | Clothes, item in hands, holster and backpack/pocket contents, with stack sizes and durability. Rounds loaded in a weapon or magazine are not listed. |
| Item list | ✅ | All items, with their in-game names. |
| Entity list | ⚠️ | Animals have in-game names; zombie and NPC names are built from codes (e.g. "Zombie Civilian Skinny Male"). |
| Locations | ❌ | SCUM has no list of named places. |
| World map | ✅ | Player dots line up with the in-game map. The map picture is a one-time upload (step 4). |
| Send a message | ✅ | To everyone (an orange server line in chat) or to one player, as `Name: message` when a server chat name is set. |
| Console commands | ⚠️ | Only commands you allow. Admin commands need an admin from `AdminUsers.ini` online, and their output shows in that admin's chat, not in Takaro. `announce` and `listplayers` work with nobody online. |
| Kick | ✅ | The player sees the reason. |
| Ban and unban | ✅ | Also offline and timed; a banned player is refused on rejoin. Unban also lifts bans that were in `BannedUsers.ini` at server start. |
| Ban list | ✅ | The server's bans with reasons and end times. |
| Give an item | ⚠️ | Usually lands in pockets or backpack and stacks merge. When SCUM refuses the pickup (e.g. hands busy), the items drop at the player's feet. |
| Shop | ✅ | Buying, orders and claims work, also for orders bought while offline. Items land like Give an item. |
| Shut down the server | ✅ | Off by default; set `ENABLE_DESTRUCTIVE_COMMANDS=true`. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Global, local (as "friends") and squad (as "team") chat; `#` admin commands are not sent. |
| Player death | ⚠️ | With position. Deaths by zombies, animals, falls or bleeding have no killer. The killer's name for player kills is not checked. |
| Entity killed | ✅ | Player kills of zombies, animals and NPCs, with creature and weapon names. |
| Server log lines | ⚠️ | Admin log lines are sent; Takaro does not store them, but module hooks can react to them. |
| Discord chat bridge | ✅ | Both ways; use `chatBridgeNoEcho` to avoid echoes. |
| Modules | ⚠️ | Chat commands, join and scheduled messages, hooks and `@settp` work; `@tp` is not checked. |
| BattlEye | ✅ | Works with BattlEye on. |

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
