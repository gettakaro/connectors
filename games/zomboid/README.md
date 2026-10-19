# Takaro Project Zomboid Connector

A server-side-only Java agent (version **1.0.0**) that connects a Project Zomboid Build 42
dedicated server to Takaro. Tested against a **42.20.4 b0bbce05d5** dedicated server; players
do not install anything and there is no Workshop item.

## Install

Download the latest release: https://takaro.io/connectors/zomboid

### 1. Before you start

You need:

- A **Project Zomboid Build 42 dedicated server** (Linux or Windows) that you can stop, start
  and copy files to.
- The ability to **change how the server JVM starts** — either an environment variable on the
  server process, or an edit to the server's start script. This connector is a Java agent, so
  there is no `Mods/` folder to drop it into.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

### 2. Download the connector

Download **`takaro-zomboid-agent-linux-42.21.0-<version>.jar`** from the latest
`zomboid-vX.Y.Z` release on the releases page:

> https://github.com/gettakaro/connectors/releases

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/zomboid-v<version>/takaro-zomboid-agent-linux-42.21.0-<version>.jar`

`TakaroConnector-<version>.jar` beside it is the same file under its old name. It is one
self-contained jar; there is nothing to unzip. Do not use the `zomboid-dev` pre-release; that is
an untested rolling build.

**Supported server build:** Project Zomboid 42.21.0, Steam build 25485538 (the current `public`
branch). The connector checks the server jar when it starts and refuses to hook another build
unless you set `TAKARO_TARGET_POLICY=warn` on the server process. Releases up to 1.1.0 were built
for 42.20.4 and refuse a 42.21.0 server; update the connector when you update the game.

### 3. Put the jar in place

Stop the server and save the jar as `TakaroConnector.jar` in a `Takaro` folder inside the
server's Zomboid data folder (where your saves, logs and `db/` live, not the game install
directory):

```
/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

On Windows that is `C:\Users\<user>\Zomboid\Takaro\TakaroConnector.jar`. A SteamCMD `validate`
never touches this folder.

### 4. Attach it to the server JVM

Set this environment variable on the server process:

```
JAVA_TOOL_OPTIONS=-javaagent:/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

That is the mechanism this connector is tested with. In Docker/compose it is an `environment:`
entry; on a systemd unit it is `Environment=`; in a shell start script, `export` it before
launching the server.

If you cannot set an environment variable, add the same `-javaagent:` argument to `vmArgs` in
`ProjectZomboid64.json` in the game install directory instead — that works too, but SteamCMD
`validate` reverts it, so you have to re-apply it after every game update.

### 5. Paste your registration token

The settings live in `TakaroConfig.txt` next to the jar:

```
/home/steam/Zomboid/Takaro/TakaroConfig.txt
```

Start the server. If the file is not there yet, the connector creates it with a new
`identityToken`, and the console says that the token is missing. Open the file, paste the token
after `registrationToken=` and save. Saving is enough: the connector connects within a few
seconds, no restart needed. The same goes for a corrected token later on.

You can also create the file yourself before the first start, with this content (leave
`identityToken` empty; the connector fills it in):

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

Every key can also be given as an environment variable, which wins over the file and is never
written into it: `TAKARO_WS_URL`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`,
`TAKARO_SERVER_CHAT_NAME`, `TAKARO_DEBUG`, `TAKARO_LOG_EVENTS`, `TAKARO_DEBUG_CATALOG`,
`TAKARO_PROTOCOL`.

The connector speaks Takaro's Generic Game Protocol version 1 when Takaro offers it (it falls
back to version 0 on an older Takaro). Version 1 gives the map below and resends events that
happened while the connection was down. `protocol=0` in `TakaroConfig.txt` (or
`TAKARO_PROTOCOL=0`) turns version 1 off, and with it the map; changing `protocol` reconnects
without a restart. `logEvents` and `debugCatalog` (extra diagnostics in the log) need a restart.

### 6. Check that it worked

The connector writes its own log to `Takaro/takaro-agent.log` and mirrors it to the server
console. In order, you should see:

```
premain: Takaro Project Zomboid connector (M2)
target-check: {"result":"ok", ...}
config: loaded /home/steam/Zomboid/Takaro/TakaroConfig.txt
premain: hooks installed
first tick reached — starting Takaro connector
Connecting to Takaro at wss://connect.takaro.io/
Identified successfully
```

In Takaro, the game server shows as **online**.

If the token is missing or Takaro rejects it, the console shows a block of `*` lines that names
the file to edit. Fix the token there and save; no restart needed. Emptying the token
disconnects the server from Takaro.

If the `target-check` line says `"result":"refuse"`, your server is a different Project Zomboid
build from the one this jar was built for and no hooks were installed. Use the release built for
your build, or set `TAKARO_TARGET_POLICY=warn` to run it anyway (the hooks may bind nothing).

### 7. Upgrading

**Stop the server first.** Replace only `Takaro/TakaroConnector.jar` with the new jar (renamed to
`TakaroConnector.jar`) and start the server again. Your `Takaro/TakaroConfig.txt` is never
touched. Never swap the jar under a running server.

## What works, what doesn't

Verified end to end on **2026-09-13/14** against a real dedicated server (game build
**42.20.4 b0bbce05d5**) with a real game client connected. Re-verified on **42.21.0 4a0e9546ec** on 2026-10-01
with a real game client: connection, restart/reconnect, player list and lookup, location, inventory,
both catalogues, chat, broadcast, whisper, give item, teleport, console commands, kick, permanent
ban, unban, ban list and the join, leave, chat and death events. Not yet re-run on 42.21.0:
timed-ban expiry, shutdown and the zombie-kill event.
✅ = works, ⚠️ = works with a caveat or is unverified, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The server reports itself reachable to Takaro while it is up, with a player connected. |
| Server restart / reconnect | ✅ | After a restart the connector comes back and re-identifies on its own, no manual step. |
| Token change without a restart | ✅ | A token pasted or corrected in `TakaroConfig.txt` while the server runs connects within a few seconds; a missing or rejected token is reported in the console. |
| Player list | ✅ | Name, Steam id, platform id, IP and ping. Empty list when nobody is online. |
| Single player lookup | ✅ | Same details for one player, looked up by name. |
| Player location | ✅ | Live X/Y/Z, and it follows teleports. |
| Player inventory | ✅ | Matches what the player is carrying, including item condition. |
| Item catalogue | ✅ | 5,092 items synced. |
| Entity catalogue | ✅ | 242 entities synced (zombies and vehicles), vehicles under their in-game names ("Dash Bulldriver", "Burnt Chevalier Cossette"). |
| Locations / points of interest | ⚠️ | Build 42 has no named-location registry, so only player-claimed safehouses can be listed — on a world with no safehouses the list is empty. |
| Chat messages from players | ✅ | Real player chat reaches Takaro with the player and channel attached. |
| Broadcast a message | ✅ | Shown to everyone in the server chat. |
| Whisper a player | ✅ | A message addressed to one player arrives in that player's chat. |
| Give an item | ✅ | The item appears in the player's inventory without a relog. |
| Teleport a player | ⚠️ | The player is moved to the requested position; it can take around 15 seconds before the new position is reported back. |
| Run a console command | ✅ | Output and success/failure are returned, including the error text for unknown commands. |
| Kick a player | ✅ | The player is dropped from the server and can rejoin afterwards. |
| Ban a player (timed and permanent) | ✅ | The ban lands on the game server with its expiry; timed bans are lifted automatically when they run out. |
| Unban a player | ✅ | Clears the ban everywhere it was written, and the player can rejoin. |
| Ban list | ✅ | Shows bans with their expiry, including bans made outside Takaro. |
| Shut the server down | ✅ | The server saves and quits on request. |
| Player joined event | ✅ | Arrives in Takaro on join. |
| Player left event | ✅ | Arrives in Takaro on leave, and on a kick. |
| Player chat event | ✅ | See "Chat messages from players". |
| Player death event | ✅ | Includes the position where the player died. |
| Entity kill event | ✅ | Proven with a real zombie kill; the weapon used is included. A bare-handed kill reports no weapon. Rate-limited to about 20 per second so a horde cannot flood Takaro. |
| Log events | ⚠️ | Server log lines are forwarded, but **off by default** — set `logEvents=true` (or `TAKARO_LOG_EVENTS=true`) to turn them on. |
| Map | ⚠️ | Needs Takaro protocol v1 (not yet on takaro.io). Takaro shows your server's own world map (the picture the in-game map draws, read from the game install) with player dots. Ground floor only; areas added by map mods show as grey. |
| Game name, icon and version | ✅ | Takaro shows the server as Project Zomboid with its icon, the running game build and the connector version (needs Takaro protocol v1). |
| Discord chat bridge | ⚠️ | Not verified in a live test. The underlying chat in both directions works, so it is expected to, but it was not proven. |
| Shop & economy | ⚠️ | Not verified in a live test. The pieces it needs — item catalogue, give an item, player inventory — are all proven, but the shop itself was not exercised. |

### Known issues

- **Messages sent from the Takaro dashboard show the server name as the sender.** Takaro does
  not attach your domain's **Server Chat Name** setting to admin messages, so they fall back to
  the Project Zomboid server name. Messages sent by modules and commands use the right name. You
  can force a name by setting `serverChatName=` in `TakaroConfig.txt`, but that duplicates the
  Takaro setting, so it is left unset by default.
- **No named locations.** Build 42 simply does not keep a registry of named places, so Takaro
  can only ever see safehouses players have claimed.
- **One map, one floor.** The map is the vanilla Knox County picture that ships with the game,
  built once when the server starts (about 20 seconds, cached in `Takaro/map/`). Basements and
  upper floors are not separate maps, and map mods that add new areas are not drawn.
- **A teleport takes a few seconds to show up.** The move happens immediately in game, but the
  position Takaro reads back can lag by roughly 15 seconds.
- **The connector must be attached before the server starts.** It hooks game classes as the JVM
  loads them, so it cannot be added to a server that is already running.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
