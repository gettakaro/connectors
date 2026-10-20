# Takaro Team Fortress 2 Connector

Connects a Team Fortress 2 dedicated server (Windows or Linux) to Takaro. It is a SourceMod plugin with two
small SourceMod extensions that run inside the server. There is no extra program to run, and players install
nothing.

## What works, what doesn't

Tested on 2026-10-18 to 2026-10-20 with a real Steam client on TF2 server version 11138802 (Steam build
25995664): every row below on a Windows dedicated server, and the Linux package on a Linux dedicated server
(connection, player list, chat, teleport, locations). ✅ = works, ⚠️ = works with a limit, ❌ = not supported.

| What | | Notes |
|---|---|---|
| Connection to Takaro | ✅ | Connects by itself and shows the server as reachable. |
| Player list | ✅ | Name, ping and IP; the id is the SteamID64. Bots only with `takaro_tf2_include_bots 1`. |
| Single player lookup | ✅ | |
| Player location and teleport | ✅ | Teleports work through the API and the teleports module (`@settp`, `@tp`). |
| Player inventory | ⚠️ | The weapons the player has equipped, with in-game names; not the Steam backpack or cosmetics. |
| Item list | ✅ | 318 TF2 weapons with their in-game names. |
| Entity list | ✅ | Engineer buildings, the sapper and Halloween bosses. |
| Locations | ✅ | Read from the running map: intelligence, team spawns and control points. |
| Send a message | ✅ | To everyone or to one player, with Takaro's sender name. |
| Run a console command | ✅ | Only commands you allow in the config; others are refused. |
| Kick a player | ✅ | |
| Ban and unban | ✅ | The player is kicked and cannot rejoin; bans survive a server restart. Needs real SteamIDs (no `-insecure`). |
| Timed bans | ⚠️ | The server lifts the ban on time; Takaro shows a synced ban without its end date. |
| Ban list | ✅ | Takaro's bans plus permanent bans made in game (`banned_user.cfg`). A ban made in game can only be lifted in game. |
| Give an item | ❌ | A TF2 server cannot grant Steam items. |
| Shop item delivery | ❌ | Needs give item. |
| Shut down the server | ⚠️ | Off by default; turn it on with `takaro_tf2_shutdown_enabled 1`. |
| World map | ❌ | TF2 has no map tiles. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | All chat as "global", team chat as "team". |
| Player death | ✅ | With the killer and the position. |
| Building destroyed | ✅ | Sentry gun, dispenser and teleporter kills, with the weapon. |
| Server log lines | ⚠️ | Sent; Takaro does not store them, but module hooks can react to them. |
| Modules: commands, hooks, cronjobs | ✅ | Chat commands such as `@ping`, `@settp` and `@tp` work. |
| Discord chat bridge (both ways) | ✅ | Game chat goes to Discord and Discord messages show in game. |
| Discord join notices | ✅ | |

## Install

### Before you start

- A Team Fortress 2 dedicated server (Steam app `232250`) on Windows or Linux where you can stop, start and
  upload files into its `tf/` folder.
- [MetaMod:Source 1.12](https://www.sourcemm.net/downloads.php?branch=stable) and
  [SourceMod 1.13](https://www.sourcemod.net/downloads.php?branch=dev) installed on that server.
  SourceMod 1.12 crashes current TF2 builds.
- A Takaro game server of type **Generic** and its **registration token** (Takaro shows it when you add the
  server).

### 1. Install

1. Download the zip for your server's system from the newest TF2 release on the
   [Takaro connectors releases page](https://github.com/gettakaro/connectors/releases):
   `takaro-tf2-windows-<build>-<version>.zip` or `takaro-tf2-linux-<build>-<version>.zip` (`<build>` is the
   TF2 server's Steam build it was tested on).
2. Stop the server and unzip the file into the server's `tf/` folder. It adds
   `addons/sourcemod/plugins/takaro_tf2.smx`, the two extensions it needs (`websocket.ext`, `json.ext`, 32- and
   64-bit) and `cfg/sourcemod/takaro_tf2.cfg`.
3. Edit `tf/cfg/sourcemod/takaro_tf2.cfg`:

   | Setting | What to put there |
   |---|---|
   | `takaro_tf2_registration_token` | Your Takaro registration token. Required. |
   | `takaro_tf2_identity_token` | Any unique name for this server, for example `my-tf2-1`. Keep it unchanged afterwards. |
   | `takaro_tf2_server_name` | The name Takaro shows for this server. |
   | `takaro_tf2_ws_url` | Leave `wss://connect.takaro.io`. |
   | `takaro_tf2_command_allowlist_exact` | Console commands Takaro may run, separated by `;`. Default: `status`. |
   | `takaro_tf2_command_allowlist_prefixes` | Command prefixes Takaro may run, for example `sm_slay `. Default: none. |
   | `takaro_tf2_include_bots` | `1` lists bots as players. Default: `0`. |
   | `takaro_tf2_shutdown_enabled` | `1` lets Takaro stop the server. Default: `0`. |

4. Start the server. Do not start it with `-insecure`: without Steam authentication players have no SteamID,
   and Takaro cannot ban them.

### 2. Check it works

- The server console shows `[TakaroTF2] [info] Takaro identity accepted`.
- `sm plugins list` shows `Takaro TF2 Connector` and `sm exts list` shows the websocket and json extensions.
- Takaro lists the server as reachable.

### 3. Update

Stop the server and unzip the new release over `tf/`, but keep your own `cfg/sourcemod/takaro_tf2.cfg` (do not
let the unzip replace it; it holds your token). Bans made through Takaro are kept in
`addons/sourcemod/data/takaro_tf2_bans.txt`, which the zip never contains. After a TF2 update the plugin
usually keeps working; a connector release names each tested build.

## Known issues

- Takaro keeps entity entries from earlier connector versions; they are harmless.

See [DEVELOPMENT.md](DEVELOPMENT.md) for building from source.
