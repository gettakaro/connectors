# Takaro Team Fortress 2 Connector

A SourceMod plugin with two small extensions that connects a Team Fortress 2 dedicated server
(Windows or Linux) to Takaro. Players do not install anything.

## Install

You need: a TF2 dedicated server with
[MetaMod:Source 1.12](https://www.sourcemm.net/downloads.php?branch=stable) and
[SourceMod 1.13](https://www.sourcemod.net/downloads.php?branch=dev) (SourceMod 1.12 crashes
current TF2), and a Takaro game server of type **Generic** with its registration token.

### 1. Download

From https://takaro.io/connectors/tf2, download the zip for your server's system:

- Windows: `takaro-tf2-windows-<build>-<version>.zip`
- Linux: `takaro-tf2-linux-<build>-<version>.zip`

### 2. Install

Stop the server and unzip into the server's `tf/` folder. It adds:

```text
tf/addons/sourcemod/plugins/takaro_tf2.smx
tf/addons/sourcemod/extensions/   (websocket and json extensions)
tf/cfg/sourcemod/takaro_tf2.cfg
```

### 3. Configure

Edit `tf/cfg/sourcemod/takaro_tf2.cfg` and start the server:

```text
takaro_tf2_registration_token "your-registration-token-here"
takaro_tf2_identity_token "my-tf2-1"
takaro_tf2_server_name "My TF2 server"
```

The identity token is any unique name for this server; keep it unchanged afterwards. Do not start
the server with `-insecure`: players then have no SteamID and cannot be banned.

| Optional setting | What it does |
|---|---|
| `takaro_tf2_command_allowlist_exact` | Console commands Takaro may run, separated by `;` (default `status`). |
| `takaro_tf2_command_allowlist_prefixes` | Command prefixes Takaro may run, for example `sm_slay `. |
| `takaro_tf2_include_bots` | `1` lists bots as players. |
| `takaro_tf2_shutdown_enabled` | `1` lets Takaro stop the server. |
| `takaro_tf2_server_chat_name` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |

### 4. Check that it works

The server console shows `[TakaroTF2] [info] Takaro identity accepted` and the server shows as
online in Takaro.

**Upgrading:** unzip the new release over `tf/` but keep your own `cfg/sourcemod/takaro_tf2.cfg`.

## What works, what doesn't

Checked on TF2 server version 11138802 (Steam build 25995664), 2026-10-20. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | SteamID64, IP and ping. |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ⚠️ | Equipped weapons only. |
| Item list | ✅ | |
| Entity list | ✅ | Buildings, the sapper and Halloween bosses. |
| Locations | ✅ | Intelligence, team spawns and control points. |
| World map | ❌ | TF2 has no map tiles. |
| Send a message | ✅ | To everyone or one player. |
| Console commands | ✅ | Allowlisted commands only. |
| Kick | ✅ | |
| Ban and unban | ✅ | Needs real SteamIDs (no `-insecure`). |
| Ban list | ✅ | |
| Give an item | ❌ | A TF2 server cannot grant Steam items. |
| Shop | ❌ | No give item. |
| Shut down the server | ⚠️ | Needs `takaro_tf2_shutdown_enabled 1`. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | |
| Entity killed | ✅ | Building kills. |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ✅ | Both ways. |
| Modules | ✅ | |
| Timed bans | ⚠️ | Takaro shows a synced ban without its end date. |

## Known issues

- On Linux only connection, player list, chat, teleport and locations are checked.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
