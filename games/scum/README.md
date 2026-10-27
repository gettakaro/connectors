# Takaro SCUM Connector

Connects a SCUM dedicated server on Windows to Takaro. It is one UE4SS mod that runs inside `SCUMServer.exe` and
connects to Takaro by itself; there is no extra program to run. Players do not install anything.

## Install

### 1. Before you start

- A SCUM dedicated server on **Windows** (Steam app 3792580) with file access to `SCUM\Binaries\Win64`. On a
  rented host without file access, ask support to place the files.
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download and unzip

1. Download `takaro-scum-windows-<build>-<version>.zip` from the newest SCUM release on
   <https://takaro.io/connectors/scum> or <https://github.com/gettakaro/connectors/releases>. `<build>` is the
   SCUM server's Steam build. The zip includes the exact UE4SS version the connector needs.
2. Stop the server. If you already use UE4SS, rename your `ue4ss` folder and `dwmapi.dll` first.
3. Unzip into `SCUM\Binaries\Win64` (the folder with `SCUMServer.exe`). You get `dwmapi.dll`, `ue4ss\` and
   `ue4ss\Mods\TakaroSCUM\`.

Do not change the `[Hooks]` values in `ue4ss\UE4SS-settings.ini`: with UE4SS's default hooks the server crashes
when a player joins.

### 3. Set the registration token

Copy `ue4ss\Mods\TakaroSCUM\TakaroConfig.txt.example` to `TakaroConfig.txt` in the same folder and set
`REGISTRATION_TOKEN`.

| Key | What it does |
|---|---|
| `REGISTRATION_TOKEN` | Your Takaro registration token. Required. |
| `SERVER_NAME` | The name Takaro shows for this server. |
| `COMMAND_ALLOWLIST` | Console commands Takaro may run, comma separated, without `#`, e.g. `announce,listplayers`. Empty = none. |
| `ENABLE_DESTRUCTIVE_COMMANDS` | `true` lets Takaro shut the server down. Default `false`. |
| `LOG_FRAMES` | `true` logs all Takaro traffic to `takaro.log` (tokens hidden). |

Leave `IDENTITY_TOKEN` empty: the connector creates `TakaroIdentity.txt`. Keep that file when you move or
reinstall the server.

### 4. Start and check

Start the server. After the world has loaded (one to two minutes), `ue4ss\Mods\TakaroSCUM\takaro.log` shows
`identified with Takaro as game server …` and Takaro shows the server as online. If not, `takaro.log` and
`ue4ss\UE4SS.log` say why.

### 5. Map picture (optional)

Takaro's map shows player positions without a picture. For the in-game map as background, open the game server's
map in Takaro, pick **The Island** and upload the image once; leave the bounds empty. The image is the client
texture `SCUM/Content/ConZ_Files/UI/HUD/IslandMap` (2048 × 2048, in `pakchunk14-WindowsNoEditor.pak`), exported
to PNG with an Unreal asset viewer such as FModel. Rotate it by 180° first (sector D4 top left); do not crop it.

### Updating

Stop the server, unzip the new release over `SCUM\Binaries\Win64` and start it; `TakaroConfig.txt` and
`TakaroIdentity.txt` are kept. After a SCUM update, wait for a connector release that names the new build.

## What works, what doesn't

Tested on SCUM 1.3.4.2.161892 (Steam build 25803875) on 2026-10-26, BattlEye off and on.

| Feature | Status | Note |
|---|---|---|
| Connect, reachability, reconnect after restart | ✅ | |
| BattlEye | ✅ | Works with BattlEye on |
| Game profile (name, version, icon) | ✅ | |
| Player list and lookup | ✅ | SteamID64, IP and ping; offline players give last data |
| Player location, teleport | ✅ | |
| Player inventory | ✅ | Ammo loaded in weapons or magazines is not listed |
| Give item | ⚠️ | Sometimes drops at the player's feet instead |
| Item list | ✅ | In-game names |
| Creature list | ⚠️ | Zombie and NPC names are derived from game codes |
| Locations | ❌ | SCUM has no list of named places |
| World map with player positions | ✅ | Map picture needs a one-time upload |
| Send message, whisper | ✅ | |
| Console commands | ⚠️ | Allowlisted only; most need an admin online; output not returned |
| Kick, ban, unban, ban list | ✅ | Banned players are refused before their character loads |
| Shutdown | ✅ | Off by default |
| Join, leave, chat events | ✅ | Local chat arrives as "friends", squad chat as "team" |
| Player death event | ⚠️ | No killer for non-player deaths; PvP killer not tested |
| Creature killed event | ✅ | With creature and weapon names |
| Log events | ⚠️ | Admin log lines; Takaro does not store them |
| Modules, shop, economy | ✅ | |
| Discord chat bridge | ✅ | Both ways |
| Whisper to only one of two players | ⚠️ | Not tested with two players |
