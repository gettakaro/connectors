# Takaro Team Fortress 2 Connector

Connects a Team Fortress 2 dedicated server (Windows or Linux) to Takaro. It is a SourceMod plugin
with two small SourceMod extensions that run inside the server; there is no extra program to run.
Players do not install anything.

## Install

### 1. Before you start

- A Team Fortress 2 dedicated server (Steam app `232250`) on Windows or Linux that you can stop,
  start and upload files into its `tf/` folder.
- [MetaMod:Source 1.12](https://www.sourcemm.net/downloads.php?branch=stable) and
  [SourceMod 1.13](https://www.sourcemod.net/downloads.php?branch=dev) on that server. SourceMod
  1.12 crashes current TF2 builds.
- A Takaro game server of type **Generic** and its **registration token** (Takaro shows it when
  you add the server).

### 2. Download

Download the latest release from https://takaro.io/connectors/tf2 (or the newest TF2 release on
the [GitHub releases page](https://github.com/gettakaro/connectors/releases)). Pick the zip for
your server's system:

- Windows: `takaro-tf2-windows-<build>-<version>.zip`
- Linux: `takaro-tf2-linux-<build>-<version>.zip`

`<build>` is the TF2 server Steam build the release was checked on. Each release has a
`SHA256SUMS` file to check the download with.

### 3. Install

Stop the server and unzip the file into the server's `tf/` folder. It adds:

```
tf/
    addons/sourcemod/plugins/takaro_tf2.smx
    addons/sourcemod/extensions/   websocket.ext and json.ext (32- and 64-bit)
    cfg/sourcemod/takaro_tf2.cfg
```

### 4. Configure

Edit `tf/cfg/sourcemod/takaro_tf2.cfg`:

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
| `takaro_tf2_server_chat_name` | Optional sender name on Takaro messages, for example `MyServer` (see below). |

Start the server. Do not start it with `-insecure`: without Steam authentication players have no
SteamID, and Takaro cannot ban them.

**Server chat name (optional).** Messages from Takaro show as `[<sender>] <message>`, the sender in green, to everyone and to one player. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `takaro_tf2_server_chat_name` instead (applies after `exec sourcemod/takaro_tf2` or a restart); the name set in Takaro wins over it. With neither, the sender is `Takaro`.

### 5. Check that it works

- The server console shows `[TakaroTF2] [info] Takaro identity accepted`.
- `sm plugins list` shows `Takaro TF2 Connector` and `sm exts list` shows the websocket and json
  extensions.
- The game server shows as online in Takaro.

### 6. Upgrade

Stop the server and unzip the new release over `tf/`, but keep your own
`cfg/sourcemod/takaro_tf2.cfg` (do not let the unzip replace it; it holds your token). Bans made
through Takaro are kept in `addons/sourcemod/data/takaro_tf2_bans.txt`, which the zip never
contains. After a TF2 update the plugin usually keeps working; each connector release names the
build it was checked on.

## What works, what doesn't

Checked on TF2 server version 11138802 (Steam build 25995664), 2026-10-20. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | Name, ping and IP; the id is the SteamID64. Bots only with `takaro_tf2_include_bots 1`. |
| Player location | ✅ | |
| Teleport | ✅ | Also through the teleports module (`@settp`, `@tp`). |
| Player inventory | ⚠️ | The equipped weapons with in-game names; not the Steam backpack or cosmetics. |
| Item list | ✅ | TF2 weapons with their in-game names. |
| Entity list | ✅ | Engineer buildings, the sapper and Halloween bosses. |
| Locations | ✅ | Read from the running map: intelligence, team spawns and control points. |
| World map | ❌ | TF2 has no map tiles. |
| Send a message | ✅ | To everyone or to one player, as `[<sender>] <message>`. |
| Console commands | ✅ | Only commands you allow in the config; others are refused. |
| Kick | ✅ | |
| Ban and unban | ✅ | The player is kicked and cannot rejoin; bans survive a restart. Needs real SteamIDs (no `-insecure`). |
| Ban list | ✅ | Takaro's bans plus permanent bans made in game (`banned_user.cfg`). A ban made in game can only be lifted in game. |
| Give an item | ❌ | A TF2 server cannot grant Steam items. |
| Shop | ❌ | Items cannot be delivered (no give item). |
| Shut down the server | ⚠️ | Off by default; turn it on with `takaro_tf2_shutdown_enabled 1`. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | All chat as "global", team chat as "team". Chat that TF2 or a plugin blocks (spam limit, gag) is not reported. |
| Player death | ✅ | With the killer and the position. |
| Entity killed | ✅ | Sentry gun, dispenser and teleporter kills, with the weapon. |
| Server log lines | ⚠️ | Sent; Takaro does not store them, but module hooks can react to them. |
| Discord chat bridge | ✅ | Both ways, plus join notices. |
| Modules | ✅ | Chat commands such as `@ping` work. |
| Timed bans | ⚠️ | The server lifts the ban on time; Takaro shows a synced ban without its end date. |

## Known issues

- On Linux only connection, player list, chat, teleport and locations have been checked; the other
  rows were checked on Windows.
- Takaro keeps entity entries from earlier connector versions; they are harmless.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
