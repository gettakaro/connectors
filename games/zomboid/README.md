# Takaro Project Zomboid Connector

A server-side Java agent (`-javaagent` jar) that runs inside the JVM of a Project Zomboid Build 42
dedicated server, on Linux or Windows, and connects it to Takaro. Players do not install
anything, and there is no Workshop item.

## Install

### 1. Before you start

- A **Project Zomboid Build 42 dedicated server** (Linux or Windows) that you can stop, start
  and copy files to.
- The ability to **change how the server JVM starts**: an environment variable on the server
  process, or an edit to the server's start script. The connector is a Java agent, so there is
  no `Mods/` folder to drop it into.
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download

Download **`takaro-zomboid-agent-linux-<game build>-<version>.jar`** (for example
`takaro-zomboid-agent-linux-42.21.0-<version>.jar`) from the latest release at
https://takaro.io/connectors/zomboid (or from the latest `zomboid-vX.Y.Z` release at
<https://github.com/gettakaro/connectors/releases>; do not use the `zomboid-dev` or `pr-…`
pre-releases). `TakaroConnector-<version>.jar` beside it is the same file under its old name.
It is one self-contained jar; there is nothing to unzip. To check it, fetch `SHA256SUMS` from the
same release and run `sha256sum -c --ignore-missing SHA256SUMS` in the download folder.

Supported server build: Project Zomboid 42.21.0, Steam build 25485538 (the `public` branch). The
connector checks the server jar when it starts and refuses to hook another build unless you set
`TAKARO_TARGET_POLICY=warn` on the server process.

### 3. Install

Stop the server and save the jar as `TakaroConnector.jar` in a `Takaro` folder inside the
server's Zomboid data folder (where your saves, logs and `db/` live, not the game install
directory):

```
Zomboid/
    db/
    Takaro/
        TakaroConnector.jar
        TakaroConfig.txt    (created by the connector on first start)
```

- Linux: `/home/steam/Zomboid/Takaro/TakaroConnector.jar`
- Windows: `C:\Users\<user>\Zomboid\Takaro\TakaroConnector.jar`

A SteamCMD `validate` never touches this folder. Then attach the jar to the server JVM by setting
this environment variable on the server process:

```
JAVA_TOOL_OPTIONS=-javaagent:/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

In Docker/compose that is an `environment:` entry; in a systemd unit, `Environment=`; in a shell
start script, `export` it before launching the server.

If you cannot set an environment variable, add the same `-javaagent:` argument to `vmArgs` in
`ProjectZomboid64.json` in the game install directory instead. SteamCMD `validate` reverts that
file, so re-apply it after every game update.

### 4. Configure

The settings live in `Takaro/TakaroConfig.txt` next to the jar. Start the server: if the file is
not there yet, the connector creates it with a new `identityToken` and the console says the
token is missing. Paste the token after `registrationToken=` and save. No restart is needed: the
connector connects within a few seconds, also after you correct a token.

You can also create the file yourself before the first start (leave `identityToken` empty; the
connector fills it in):

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

- `protocol`: leave `1`. `0` turns off the world map and the resending of events missed while
  disconnected; a change reconnects without a restart.
- `serverChatName`: see below.
- `logEvents` (forward server log lines) and `debugCatalog` (extra diagnostics in the log) need
  a restart.

Every key can also be given as an environment variable, which wins over the file and is never
written into it: `TAKARO_WS_URL`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`,
`TAKARO_SERVER_CHAT_NAME`, `TAKARO_DEBUG`, `TAKARO_LOG_EVENTS`, `TAKARO_DEBUG_CATALOG`,
`TAKARO_PROTOCOL`.

**Server chat name (optional).** Messages from Takaro show as a General chat line `[Your Name]: message`, to everyone or, for a whisper, to that one player. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `serverChatName=` instead; the name set in Takaro wins over it. With neither (or with General chat turned off on the server), messages arrive as server chat with the server's name in front.

### 5. Check that it works

The connector writes its own log to `Takaro/takaro-agent.log` and mirrors it to the server
console. Every line starts with `[Takaro]`. In order, you should see:

```
premain: Takaro Project Zomboid connector (M2)
target-check: {"result":"ok", ...}
config: loaded /home/steam/Zomboid/Takaro/TakaroConfig.txt
premain: hooks installed
first tick reached — starting Takaro connector
Connecting to Takaro at wss://connect.takaro.io/
Identified successfully, server ID: <id>, protocol 1
```

The game server then shows as **online** in Takaro. (`protocol 0` instead of `protocol 1` is
fine on an older Takaro.)

- The server launcher starts the JVM more than once, so the first one or two `premain` blocks can
  show `target-check: {"result":"no-game-jar", ...}`. That is expected; the block that counts is
  the last one, just before `first tick reached`.
- On the very first start, `config: loaded` reads `config: … not present yet`, and after
  `first tick reached` the connector logs `config: created … with a new identityToken`.
- A missing or rejected token shows a block of `*` lines naming the file to edit
  (`registrationToken not set, …` or
  `Takaro rejected identify: invalid_args Invalid registrationToken provided.`). Fix it and save;
  no restart needed. Emptying the token disconnects the server from Takaro.
- `target-check` with `"result":"refuse"` means your server is a different build from the one
  this jar was built for, and no hooks were installed. Use the release for your build, or set
  `TAKARO_TARGET_POLICY=warn` to run it anyway (the hooks may bind nothing).

### 6. Upgrade

Stop the server first; never swap the jar under a running server. Replace only
`Takaro/TakaroConnector.jar` with the new jar (renamed to `TakaroConnector.jar`) and start the
server. `Takaro/TakaroConfig.txt` is never touched.

When you update Project Zomboid itself, update the connector too: each release is built for one
game build (it is in the file name), and a jar for another build logs `"result":"refuse"` and
stays inactive.

## What works, what doesn't

Checked on Project Zomboid 42.21.0 (4a0e9546ec), 2026-10-01. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | The server shows as reachable while it is up. |
| Player list | ✅ | Name, Steam id, platform id, IP and ping; one player can also be looked up by name. |
| Player location | ✅ | Live X/Y/Z, following teleports. |
| Teleport | ⚠️ | The move is immediate, but the new position can take about 15 seconds to show in Takaro. |
| Player inventory | ✅ | Matches what the player carries, including item condition. |
| Item list | ✅ | |
| Entity list | ✅ | Zombies and vehicles, vehicles under their in-game names. |
| Locations | ⚠️ | Build 42 has no named places, so only player-claimed safehouses are listed. |
| World map | ⚠️ | The game's own world map with player dots; ground floor only, areas added by map mods show grey. Not checked on takaro.io yet. |
| Send a message | ✅ | Broadcasts reach everyone, whispers reach one player, with the server chat name as sender. |
| Console commands | ✅ | Output and success or failure are returned, including errors for unknown commands. |
| Kick | ✅ | The player can rejoin afterwards. |
| Ban and unban | ⚠️ | Permanent ban and unban work; automatic expiry of timed bans is not checked on this build. |
| Ban list | ✅ | With expiry, including bans made outside Takaro. |
| Give an item | ✅ | Appears in the inventory without a relog. |
| Shop | ⚠️ | Not checked. |
| Shut down the server | ⚠️ | Saves and quits on request; not checked on this build. |
| Player joined / left | ✅ | Left also arrives on a kick. |
| Chat messages | ✅ | With the player and channel. |
| Player death | ✅ | With the position. |
| Entity killed | ⚠️ | Zombie kills with the weapon (none for bare hands), at most about 20 per second; not checked on this build. |
| Server log lines | ⚠️ | Off by default; set `logEvents=true` (or `TAKARO_LOG_EVENTS=true`). |
| Discord chat bridge | ⚠️ | Not checked. |
| Modules | ⚠️ | Not checked. |
| Game name, version and icon | ⚠️ | Not checked on takaro.io yet. |
| Restarts and outages | ✅ | Re-identifies on its own after a server restart. |

## Known issues

- The map is the vanilla Knox County picture from the game install, built once at server start
  (about 20 seconds, cached in `Takaro/map/`). Basements and upper floors are not separate maps.
- The connector must be attached before the server starts; it cannot be added to a running
  server.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
