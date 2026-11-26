# Takaro Hytale Connector

A Java plugin for Hytale dedicated servers (Windows or Linux) that connects the server to Takaro
by itself, with no extra program to run. Players do not install anything. It was written by
[mad-001](https://github.com/mad-001/Hytale-Takaro-Integration) and is maintained here by the
Takaro team (MIT licence).

## Install

You need: a Hytale dedicated server on the version named in the file name, with file access to
its `mods` folder, and a Takaro game server of type **Generic** with its registration token.

### 1. Download

From https://takaro.io/connectors/hytale, download `takaro-hytale-server-0.7.7-<version>.jar`. The
`0.7.7` is the Hytale server version it is built for. The
[GitHub releases page](https://github.com/gettakaro/connectors/releases) has the same files.

### 2. Install

Stop the server and copy the jar into its `mods` folder:

```
<server>/
    HytaleServer.jar
    Assets.zip
    mods/
        takaro-hytale-server-0.7.7-<version>.jar
```

Start the server once. The plugin creates `mods/HytaleTakaroMod/TakaroConfig.properties`.

### 3. Configure

Edit `mods/HytaleTakaroMod/TakaroConfig.properties`:

```properties
IDENTITY_TOKEN=MyHytaleServer
REGISTRATION_TOKEN=<your registration token>
```

Restart the server. `IDENTITY_TOKEN` names the server in Takaro: keep it the same once the server
is added, or Takaro creates a new one.

| Setting | Default | What it does |
|---|---|---|
| `COMMAND_PREFIX` | `!` | Chat prefix for Takaro commands. Do not use `/`, Hytale uses it. |
| `MAP_ENABLED` | `true` | Draws the world map for Takaro from the explored parts of the world. |
| `LOG_FORWARD_LEVEL` | `INFO` | Lowest server log level sent to Takaro; `OFF` sends none. |
| `TAKARO_WS_URL` | `wss://connect.takaro.io/` | Leave as it is unless you run your own Takaro. |

### 4. Check that it works

The server log shows `[Takaro] Successfully identified (protocol v1)` and the server shows as online in
Takaro. A wrong registration token shows as an identify error in the log, and the plugin keeps
retrying; fix the token in the file, no restart needed.

**Upgrading:** replace the jar in `mods` and restart; `TakaroConfig.properties` is kept.

## What works, what doesn't

Checked on Hytale server 0.7.7, 2026-11-26. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects by itself; events made while offline are sent later |
| Player list | ✅ | |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ✅ | Display names; test and template items left out |
| Entity list | ✅ | Display names |
| Locations | ✅ | The server's warps |
| World map | ⚠️ | Only areas players have explored |
| Send a message | ✅ | To everyone or one player |
| Console commands | ✅ | Returns the command's own output |
| Kick | ✅ | |
| Ban and unban | ✅ | Uses the server's own ban list |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ✅ | |
| Shut down the server | ✅ | |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ⚠️ | No weapon: Hytale does not report it |
| Entity killed | ✅ | With the weapon used |
| Server log lines | ✅ | Rate limited, see `LOG_FORWARD_MAX_PER_MIN` |
| Discord chat bridge | ✅ | Use the `chatBridgeNoEcho` module |
| Modules | ✅ | |
| Game name, version and icon | ✅ | |
| Restarts and outages | ✅ | |
| Game updates | ⚠️ | Each Hytale update needs a matching plugin release |

## Known issues

- Hytale is in early access and its server API changes between updates; use the jar built for
  your server version.
- Players and levels: Hytale has no level or skill system for Takaro to read.
- Kick and ban reasons: the Hytale 0.7.7 client sometimes shows "Lost connection to the server."
  instead of the reason. The game's own `/kick` does the same.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
