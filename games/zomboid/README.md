# Takaro Project Zomboid Connector

A Java agent that runs inside a Project Zomboid Build 42 dedicated server and connects it to
Takaro. Each release is built for one game build, currently 42.21.0. Players do not install
anything, and there is no Workshop item.

## Install

### 1. Before you start

- A **Project Zomboid Build 42 dedicated server** (Linux or Windows) on build 42.21.0, and a way to
  set an environment variable on the server process (Docker, systemd or the start script).
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download

Download **`takaro-zomboid-agent-linux-42.21.0-<version>.jar`** from
<https://takaro.io/connectors/zomboid> or the latest `zomboid-vX.Y.Z` release on
<https://github.com/gettakaro/connectors/releases>. Do not use the `zomboid-dev` or `pr-…`
pre-releases. Releases up to 1.1.0 only work on 42.20.4.

### 3. Put the jar in place

Stop the server. Save the jar as `TakaroConnector.jar` in a `Takaro` folder inside the server's
Zomboid data folder (where saves and logs live, not the game install folder):

```
/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

On Windows: `C:\Users\<user>\Zomboid\Takaro\TakaroConnector.jar`.

### 4. Attach it to the server

Set this environment variable on the server process:

```
JAVA_TOOL_OPTIONS=-javaagent:/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

If you cannot set an environment variable, add the same `-javaagent:` argument to `vmArgs` in
`ProjectZomboid64.json` in the game install folder. A SteamCMD `validate` or game update undoes
that edit, so re-apply it afterwards.

### 5. Set the registration token

Start the server. The connector creates `Zomboid/Takaro/TakaroConfig.txt`. Paste your token after
`registrationToken=` and save: it connects within a few seconds, no restart needed. You can also
create the file yourself before the first start:

```
wsUrl=wss://connect.takaro.io/
registrationToken=<your registration token>
identityToken=
serverChatName=
protocol=1
debug=false
logEvents=false
debugCatalog=false
```

Leave `identityToken` empty; the connector fills it in. Set `logEvents=true` (restart needed) to
forward server log lines. Each key can also be set as an environment variable, which wins over the
file: `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL`,
`TAKARO_SERVER_CHAT_NAME`, `TAKARO_PROTOCOL`, `TAKARO_LOG_EVENTS`, `TAKARO_DEBUG`,
`TAKARO_DEBUG_CATALOG`.

### 6. Check that it worked

The connector logs to the server console and `Zomboid/Takaro/takaro-agent.log`. Look for:

```
[Takaro] Identified successfully, server ID: <id>, protocol 1
```

The server is then **online** in Takaro. A block of `*` lines means the token is missing or
rejected; fix it in `TakaroConfig.txt` and save. Early `target-check` lines saying `no-game-jar`
are normal. If the last `target-check` says `"result":"refuse"`, the jar was built for a different
game build: use the release for your build.

### 7. Upgrading

Stop the server, replace `Takaro/TakaroConnector.jar` with the new jar and start again.
`TakaroConfig.txt` is kept. When you update Project Zomboid, update the connector too.

## What works, what doesn't

Tested on Project Zomboid 42.21.0 on 2026-10-26.

| Feature | Status | Note |
|---|---|---|
| Connection, restart, reconnect | ✅ | Re-identifies on its own after a restart. |
| Token change without a restart | ✅ | |
| Game name, version and icon | ✅ | |
| Player list and lookup | ✅ | Name, Steam ID, IP and ping. |
| Player location | ✅ | |
| Player inventory | ✅ | |
| Item and entity catalogue | ✅ | |
| Locations | ⚠️ | Only player safehouses; Build 42 has no named places. |
| Broadcast and whisper | ✅ | Whisper not tested with two players. |
| Give an item | ✅ | |
| Teleport a player | ⚠️ | Takaro may see the new position about 15 seconds later. |
| Run a console command | ✅ | Unknown commands return an error. |
| Kick, ban, unban, ban list | ✅ | Timed bans are lifted when they expire. |
| Shut the server down | ✅ | Saves first. |
| Join, leave, chat, death events | ✅ | |
| Entity kill event | ⚠️ | Weapon is the item code, e.g. `Base.Axe`. |
| Log events | ⚠️ | Off by default; set `logEvents=true`. |
| World map | ⚠️ | Needs Takaro protocol 1. |
| Modules: commands, hooks, teleports | ✅ | |
| Shop and economy | ✅ | |
| Discord: game chat to Discord | ⚠️ | Takaro posts it; not checked in the Discord app. |
| Discord: Discord to game chat | ✅ | |

## Known issues

- Game name, version and icon need a release newer than 1.4.0.
- Dashboard messages show the server name as sender; set `serverChatName=` to override.
- The map is ground floor only; areas added by map mods show grey.
- The connector must be attached before the server starts; it cannot be added live.
