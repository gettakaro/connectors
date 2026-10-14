# Takaro SCUM Connector

Connects a SCUM dedicated server to Takaro. It is one mod folder that runs inside `SCUMServer.exe`. There is no extra program to run, and players install nothing.


## What works, what doesn't

Tested on 2026-10-13/14 with a real game client on SCUM 1.3.4.2.161892 (Steam build 25803875, Windows dedicated server), with BattlEye off and on. ✅ = works, ⚠️ = works with a limit, ❌ = not supported, ❔ = not tested yet.

| What | | Notes |
|---|---|---|
| Connection to Takaro | ✅ | Connects by itself and shows the server as reachable. |
| Reconnect after a server restart | ✅ | Connects again after every restart. |
| Reconnect after a network drop | ✅ | Reconnects on its own; game events from the outage are sent once it is back. |
| BattlEye | ✅ | Works on a server with BattlEye on and players using BattlEye. |
| Player list | ✅ | Name, ping and IP; the id is the SteamID64. |
| Single player lookup | ✅ | Offline players answer with their last known data. |
| Player location | ✅ | Live position; offline players give their last position. |
| Player inventory | ⚠️ | Carried items with their names; a stack (e.g. 10 rounds) counts as 1; no quality. |
| Give an item | ⚠️ | Usually goes into the player's pockets or backpack like a pickup, and stacks merge. Sometimes SCUM refuses the pickup (for example when the hands are busy); then the items drop at the player's feet and the first one goes into empty hands. |
| Item list | ✅ | 2854 items with their in-game names. |
| Creature list | ⚠️ | Animals use in-game names; zombie and NPC names come from codes (e.g. "Zombie Civilian Skinny Male"). |
| Locations / points of interest | ❌ | SCUM has no list of named places. |
| Map | ❌ | No map tiles. |
| Send a message to everyone | ✅ | Shows in chat as an orange server line. |
| Send a message to one player | ✅ | Arrives in that player's chat only. |
| Run a console command | ⚠️ | Only listed commands; most need an admin online; output stays in game. |
| Teleport a player | ✅ | The player is moved at once. |
| Kick a player | ✅ | The player sees the reason in chat, then "You have been kicked off the server!". |
| Ban a player | ✅ | Also offline and timed. The player sees the reason, is kicked, and is refused within a second when rejoining, before the character loads. |
| Unban a player | ✅ | Works at once, also for bans that were in `BannedUsers.ini` when the server started. |
| Ban list | ✅ | Shows the server's bans with reasons and end times. |
| Shut down the server | ✅ | Off by default; turn it on in the config. |
| Player joined / left | ✅ | Arrives about one second after the join or leave. |
| Chat messages | ✅ | Global chat as "global", local (nearby) chat as "friends", squad chat as "team"; `#` admin commands are not sent as chat. |
| Player death | ⚠️ | Detected within a second, with position. The killer's name for player kills is not tested yet (needs two players). |
| Creature killed | ⚠️ | Player kills of zombies, animals and NPCs, with the creature and weapon names. Tested with a melee weapon only. |
| Server log lines | ⚠️ | Admin log lines are sent; Takaro does not store them, but module hooks can react to them. |
| Modules: commands, hooks, cronjobs | ✅ | `@ping`, `@balance`, `@shop`, `@topcurrency`, join messages, scheduled messages and hooks work. |
| Modules: teleports (`@settp`, `@tp`) | ⚠️ | `@settp` works. `@tp` could not be tested (the Takaro test platform never ran it); a module teleport through the same path works. |
| Discord chat bridge (both ways) | ✅ | Game chat goes to Discord and Discord messages show in game; no echo with `chatBridgeNoEcho`. |
| Shop and economy | ✅ | Balance, shop list, buying in chat (`@shop 1 2 buy`), orders and claims work, also for orders bought while offline. Items land like Give an item (see above). |

## Install

### Before you start

- A SCUM dedicated server on **Windows** (Steam app 3792580) where you can stop, start and upload files into `SCUM\Binaries\Win64`. Rented hosts: you need file access to that folder; if you don't have it, ask support to place the files.
- A Takaro game server of type **Generic** and its **registration token** (Takaro shows it when you add the server).

### 1. Install

1. Download `takaro-scum-windows-<build>-<version>.zip` from the newest SCUM release on the [Takaro connectors releases page](https://github.com/gettakaro/connectors/releases) (`<build>` is the SCUM server's Steam build). It contains the UE4SS mod loader in the exact version the connector needs, already set up, plus the connector.
2. Stop the server. If you already use UE4SS, rename your `SCUM\Binaries\Win64\ue4ss` folder and `dwmapi.dll` first.
3. Unzip the file into `SCUM\Binaries\Win64` (the folder with `SCUMServer.exe`). You get `dwmapi.dll`, a `ue4ss` folder and `ue4ss\Mods\TakaroSCUM`.
4. Copy `ue4ss\Mods\TakaroSCUM\TakaroConfig.txt.example` to `TakaroConfig.txt` in the same folder and fill it in:

| Key | What to put there |
|---|---|
| `REGISTRATION_TOKEN` | Your Takaro registration token. Required. |
| `SERVER_NAME` | The name Takaro shows for this server. |
| `IDENTITY_TOKEN` | Leave empty. The connector makes one and keeps it in `TakaroIdentity.txt`; keep that file when you move or reinstall the server. |
| `TAKARO_WS_URL` | Leave `wss://connect.takaro.io/`. |
| `COMMAND_ALLOWLIST` | Commands Takaro may run, comma separated, without `#`, e.g. `announce,listplayers`. Empty = none. |
| `ENABLE_DESTRUCTIVE_COMMANDS` | `true` lets Takaro shut the server down. Default `false`. |
| `LOG_FRAMES` | `true` logs all Takaro traffic for troubleshooting (tokens are hidden). Default `false`. |

5. Start the server. For console commands other than `announce` and `listplayers`, an admin from `AdminUsers.ini` must be online.

Do not change the `[Hooks]` values in `ue4ss\UE4SS-settings.ini`: with UE4SS's default hooks the server crashes when a player joins.

### 2. Check it works

After the world has loaded (one to two minutes), `ue4ss\Mods\TakaroSCUM\takaro.log` shows `identified with Takaro as game server …` and Takaro lists the server as reachable. If not, `takaro.log` and `ue4ss\UE4SS.log` say why.

### 3. Update

Stop the server, unzip the new release over `SCUM\Binaries\Win64` (it replaces `Scripts\` and `dlls\main.dll`), start it. Keep `TakaroConfig.txt` and `TakaroIdentity.txt`. After a SCUM update, wait for a connector release that names the new game version.
