# Takaro Team Fortress 2 Connector

A SourceMod plugin that connects a Team Fortress 2 dedicated server (Windows or Linux) to Takaro by
itself; there is no extra program to run. Players do not install anything.

## Install

### 1. Before you start

- A TF2 dedicated server (Steam app `232250`) where you can stop, start and upload files into `tf/`.
- [MetaMod:Source 1.12](https://www.sourcemm.net/downloads.php?branch=stable) and
  [SourceMod 1.13](https://www.sourcemod.net/downloads.php?branch=dev). SourceMod 1.12 crashes current TF2 builds.
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download and unzip

From https://takaro.io/connectors/tf2 (or the newest `tf2-v<version>` release on
[GitHub](https://github.com/gettakaro/connectors/releases)), download
`takaro-tf2-windows-<build>-<version>.zip` or `takaro-tf2-linux-<build>-<version>.zip`. Stop the
server and unzip it into the server's `tf/` folder. It adds the plugin, the websocket and json
extensions it needs, and `cfg/sourcemod/takaro_tf2.cfg`.

### 3. Configure

Edit `tf/cfg/sourcemod/takaro_tf2.cfg`:

| Setting | Value |
|---|---|
| `takaro_tf2_registration_token` | Your Takaro registration token. Required. |
| `takaro_tf2_identity_token` | Any unique name for this server, for example `my-tf2-1`. Never change it afterwards. |
| `takaro_tf2_server_name` | The name Takaro shows. |
| `takaro_tf2_command_allowlist_exact` | Console commands Takaro may run, separated by `;`. Default `status`. |
| `takaro_tf2_command_allowlist_prefixes` | Command prefixes Takaro may run, for example `sm_slay `. |
| `takaro_tf2_include_bots` | `1` lists bots as players. Default `0`. |
| `takaro_tf2_shutdown_enabled` | `1` lets Takaro stop the server. Default `0`. |

Start the server, without `-insecure` (without Steam authentication players have no SteamID and
cannot be banned).

### 4. Check that it worked

The console shows `[TakaroTF2] [info] Takaro identity accepted`, and Takaro lists the server as reachable.

### Updating

Stop the server and unzip the new release over `tf/`, but keep your own
`cfg/sourcemod/takaro_tf2.cfg` (it holds your token). Takaro bans in
`addons/sourcemod/data/takaro_tf2_bans.txt` are kept.

## What works, what doesn't

Tested on TF2 server version 11138802 (Steam build 25995664) from 2026-10-18 to 2026-10-20, on Windows; the Linux
package was checked for connection, players, chat, teleport and locations.

| Feature | Status | Note |
|---|---|---|
| Connection | ✅ | |
| Player list and lookup | ✅ | Name, SteamID64, ping and IP. |
| Player location and teleport | ✅ | Also through `@settp` and `@tp`. |
| Player inventory | ⚠️ | Equipped weapons only; no Steam backpack or cosmetics. |
| Item and entity list | ✅ | TF2 weapons, buildings and Halloween bosses, with in-game names. |
| Locations | ✅ | Intelligence, team spawns and control points of the running map. |
| Send a message | ✅ | To everyone or to one player. |
| Run a console command | ✅ | Only commands allowed in the config. |
| Kick a player | ✅ | |
| Ban, unban, ban list | ✅ | Bans survive a restart; in-game bans can only be lifted in game. |
| Timed bans | ⚠️ | Lifted on time; Takaro shows a synced ban without its end date. |
| Give an item / shop delivery | ❌ | A TF2 server cannot grant Steam items. |
| Shut the server down | ⚠️ | Off by default; set `takaro_tf2_shutdown_enabled 1`. |
| World map | ❌ | TF2 has no map tiles. |
| Player joined / left | ✅ | |
| Chat | ✅ | Team chat is reported as team. |
| Player death | ✅ | With the killer and the position. |
| Building destroyed | ✅ | Sentry, dispenser and teleporter, with the weapon. |
| Log events | ⚠️ | Sent; Takaro does not store them, but hooks can react. |
| Modules | ✅ | Commands, hooks and cronjobs. |
| Discord chat bridge | ✅ | Both ways, plus join notices. |

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
