# Takaro Project Zomboid Connector

A server-side-only Java agent (version **1.0.0**) that connects a Project Zomboid Build 42
dedicated server to Takaro. Tested against a **42.20.4 b0bbce05d5** dedicated server; players
do not install anything and there is no Workshop item.

## Install

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

Download **`TakaroConnector-1.0.0.jar`** from the latest `zomboid-vX.Y.Z` release on the
releases page:

> https://github.com/gettakaro/connectors/releases

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/zomboid-v<version>/TakaroConnector-<version>.jar`

Use `zomboid-v1.0.0` or newer. The results in the table below were proven on the code that
shipped in 1.0.0. Do not use the `zomboid-dev` pre-release; that is an untested rolling build.

The download is a **single self-contained jar** — everything it needs (ByteBuddy, the WebSocket
client, Gson) is already inside it. There is nothing to unzip.

### 3. Copy it into place

Stop the server, then put the jar in the Takaro folder inside the server's Zomboid data
directory, **renamed to `TakaroConnector.jar`**:

```
/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

On Windows that folder is `C:\Users\<user>\Zomboid\Takaro\`. Create the `Takaro` folder if it
does not exist. This is the *data* directory (where your saves, logs and `db/` live), not the
game install directory — so a SteamCMD `validate` never touches it.

Now attach it to the server JVM. Set this environment variable on the server process:

```
JAVA_TOOL_OPTIONS=-javaagent:/home/steam/Zomboid/Takaro/TakaroConnector.jar
```

That is the mechanism this connector is tested with. In Docker/compose it is an `environment:`
entry; on a systemd unit it is `Environment=`; in a shell start script, `export` it before
launching the server.

If you cannot set an environment variable, add the same `-javaagent:` argument to `vmArgs` in
`ProjectZomboid64.json` in the game install directory instead — that works too, but SteamCMD
`validate` reverts it, so you have to re-apply it after every game update.

If you keep the versioned file name, point the `-javaagent:` path at that exact file name
instead of `TakaroConnector.jar`.

### 4. Configure

Create the config file next to the jar:

```
/home/steam/Zomboid/Takaro/TakaroConfig.txt
```

with your Takaro registration token:

```
wsUrl=wss://connect.takaro.io/
registrationToken=your-registration-token-here
identityToken=
debug=false
logEvents=false
```

`registrationToken` is the key that matters — the server cannot identify to Takaro without it.
Leave `wsUrl` as it is, and leave `identityToken` alone; the connector fills it in by itself.

Every key can also be given as an environment variable, which wins over the file:
`TAKARO_WS_URL`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_DEBUG`,
`TAKARO_LOG_EVENTS`. That is handy in Docker, where you may not want a config file at all.

Save the file and start the server.

### 5. Check that it worked

The connector writes its own log to `/home/steam/Zomboid/Takaro/takaro-agent.log` and mirrors
it to the server console. In order, you should see:

```
premain: Takaro Project Zomboid connector (M2)
config: loaded /home/steam/Zomboid/Takaro/TakaroConfig.txt
premain: hooks installed
first tick reached — starting Takaro connector
WebSocket connected, sending identify...
```

If you see `config: /home/steam/Zomboid/Takaro/TakaroConfig.txt not present, using env only`,
the connector did not find your config file — check the path and the file name.

And in Takaro, the game server shows as **online**. If it stays offline, the registration token
is the first thing to re-check.

### 6. Upgrading

**Stop the server first.** Replace `/home/steam/Zomboid/Takaro/TakaroConnector.jar` with the new
one and start the server again. Leave `TakaroConfig.txt` alone — your token and identity survive
the upgrade. Never swap the jar under a running server; the agent is loaded into the live JVM.

## What works, what doesn't

Verified end to end on **2026-09-13/14** against a real dedicated server (game build
**42.20.4 b0bbce05d5**) with a real game client connected.
✅ = works, ⚠️ = works with a caveat or is unverified, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The server reports itself reachable to Takaro while it is up, with a player connected. |
| Server restart / reconnect | ✅ | After a restart the connector comes back and re-identifies on its own, no manual step. |
| Player list | ✅ | Name, Steam id, platform id, IP and ping. Empty list when nobody is online. |
| Single player lookup | ✅ | Same details for one player, looked up by name. |
| Player location | ✅ | Live X/Y/Z, and it follows teleports. |
| Player inventory | ✅ | Matches what the player is carrying, including item condition. |
| Item catalogue | ✅ | 5,092 items synced. |
| Entity catalogue | ✅ | 242 entities synced (zombies and vehicles). |
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
| Map info | ❌ | Not implemented by this connector. |
| Map tiles | ❌ | Not supported by Takaro for this connector type. |
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
- **No map.** Takaro's API does not support map tiles for Generic-connector servers; nothing on
  the game server side changes that.
- **A teleport takes a few seconds to show up.** The move happens immediately in game, but the
  position Takaro reads back can lag by roughly 15 seconds.
- **The connector must be attached before the server starts.** It hooks game classes as the JVM
  loads them, so it cannot be added to a server that is already running.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
