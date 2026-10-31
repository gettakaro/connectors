# Takaro Project Zomboid Connector

A Java agent (`-javaagent` jar) that runs inside a Project Zomboid Build 42 dedicated server on
Linux or Windows. Players do not install anything, and there is no Workshop item.

## Install

You need: a Project Zomboid **42.21.0** dedicated server whose JVM start you can change (an
environment variable or the start script), and a Takaro game server of type **Generic** with its
registration token.

### 1. Download

From https://takaro.io/connectors/zomboid, download
**`takaro-zomboid-agent-linux-<game build>-<version>.jar`** (for example
`takaro-zomboid-agent-linux-42.21.0-<version>.jar`; it also works on Windows). Do not use the
`zomboid-dev` or `pr-…` pre-releases.

### 2. Install

Stop the server and save the jar as `TakaroConnector.jar` in a `Takaro` folder in the server's
Zomboid data folder (where `db/` and the saves live, not the game install):

- Linux: `/home/steam/Zomboid/Takaro/TakaroConnector.jar`
- Windows: `C:\Users\<user>\Zomboid\Takaro\TakaroConnector.jar`

Then set this environment variable on the server process (Docker `environment:`, systemd
`Environment=`, or `export` in the start script):

```text
JAVA_TOOL_OPTIONS=-javaagent:/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

If you cannot set it, add the same `-javaagent:` argument to `vmArgs` in `ProjectZomboid64.json`
in the game install; re-apply it after every game update.

### 3. Configure

Start the server; it creates `Takaro/TakaroConfig.txt` next to the jar. Paste your token and save;
it connects within a few seconds, no restart needed:

```ini
wsUrl=wss://connect.takaro.io/
registrationToken=<your registration token>
identityToken=
```

Leave `identityToken` empty; the connector fills it in. Keep it once set.

| Optional setting | What it does |
|---|---|
| `serverChatName` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |
| `logEvents` | `true` sends server log lines to Takaro (restart to apply). |

### 4. Check that it works

`Takaro/takaro-agent.log` (also in the server console) shows
`Identified successfully, server ID: <id>` and the server shows as online in Takaro. A missing or
wrong token shows a block of `*` lines naming the file. `target-check` with `"result":"refuse"`
means the jar is for another game build.

**Upgrading:** stop the server and replace `Takaro/TakaroConnector.jar`. After a game update, use
the release built for the new game build.

## What works, what doesn't

Checked on Project Zomboid 42.21.0 (4a0e9546ec), 2026-10-01. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | Steam id, IP and ping. |
| Player location | ✅ | |
| Teleport | ⚠️ | The new position can take about 15 seconds to show in Takaro. |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ✅ | Zombies and vehicles. |
| Locations | ⚠️ | Only player-claimed safehouses. |
| World map | ⚠️ | Ground floor only. Not checked yet. |
| Send a message | ✅ | Broadcasts and whispers. |
| Console commands | ✅ | |
| Kick | ✅ | |
| Ban and unban | ⚠️ | Timed ban expiry not checked on this build. |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ⚠️ | Not checked. |
| Shut down the server | ⚠️ | Not checked on this build. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | |
| Entity killed | ⚠️ | Not checked on this build. |
| Server log lines | ⚠️ | Off by default (`logEvents=true`). |
| Discord chat bridge | ⚠️ | Not checked. |
| Modules | ⚠️ | Not checked. |
| Game name, version and icon | ⚠️ | Not checked yet. |
| Restarts and outages | ✅ | Reconnects after a server restart. |

## Known issues

- Each release is built for one game build; a jar for another build stays inactive.
- The map shows the vanilla Knox County picture; basements, upper floors and map-mod areas are
  missing.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
