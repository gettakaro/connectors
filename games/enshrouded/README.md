# Takaro Enshrouded Connector

A server-side-only connector (plugin **0.4.2** + sidecar) that connects an Enshrouded dedicated
server to Takaro. Tested against game build **1024233** (Steam build 23178631) running in the
`mornedhels/enshrouded-server` Docker image; players do not install anything.

## Install

### 1. Before you start

You need:

- An **Enshrouded dedicated server** you can stop, start and copy files to. The tested setup is the
  **Linux Docker image `mornedhels/enshrouded-server`**, where `enshrouded_server.exe` runs under
  Wine/Proton. The plugin is a **Windows DLL**, so a native Windows dedicated server should work the
  same way, but that has never been tried — only the Linux/Wine container is verified.
- **Docker** with the Compose plugin on the same host (the sidecar runs as a container that shares
  the game container's network).
- **zig 0.13** to build the plugin (see step 2 — there is no download).
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

### 2. Get the plugin and the sidecar

**There is no tagged release yet** — nothing is published on the releases page, and the repository
contains no prebuilt `dbghelp.dll` and no published Docker image. Both parts are built from this
repository:

```bash
git clone https://github.com/gettakaro/connectors.git
cd connectors/games/enshrouded
./mod/build.sh            # -> mod/build/dbghelp.dll  (needs zig 0.13; set ZIG=/path/to/zig)
```

The sidecar is not built by hand: `docker-compose.example.yml` builds it from `sidecar/` with
`docker compose ... up -d --build` in step 5.

### 3. Copy it into place

The plugin is the file `mod/build/dbghelp.dll`. It is loaded by the game server as a **dbghelp
proxy**, so it has to sit next to `enshrouded_server.exe` and the server has to be told to prefer
it over the system copy.

With the example compose file, put it in the plugin mount directory:

```
games/enshrouded/
    docker-compose.example.yml
    .env
    data/
        enshrouded/            (game server data, created by the container)
        enshrouded-plugin/
            dbghelp.dll        <- the file you just built
        enshrouded-sidecar/    (sidecar cursor/online state, created by the container)
```

```bash
mkdir -p data/enshrouded-plugin data/enshrouded-sidecar
cp mod/build/dbghelp.dll data/enshrouded-plugin/
```

The compose file bind-mounts it **read-only** to `/opt/enshrouded/server/dbghelp.dll`, so SteamCMD
updates of the game cannot overwrite or delete it, and sets
`WINEDLLOVERRIDES: "dbghelp=n,b"` on the game container so the server loads this DLL instead of the
system one. If you use your own compose file or a native Windows server, you must reproduce both of
those yourself.

### 4. Configure

Copy the example environment file and fill it in:

```bash
cp .env.example .env
```

`games/enshrouded/.env` keys:

| Key | What to put there |
|---|---|
| `TAKARO_REGISTRATION_TOKEN` | Your Takaro registration token. Required. |
| `TAKARO_IDENTITY_ENSHROUDED` | A name that identifies this server to Takaro, e.g. `my-enshrouded-server`. |
| `TAKARO_ENSHROUDED_PLUGIN_TOKEN` | A long random shared secret, e.g. `openssl rand -hex 32`. The same value is passed to the game container (as `TAKARO_PLUGIN_TOKEN`) and to the sidecar; without it the plugin rejects every request with 401. Required. |
| `ENSHROUDED_ADMIN_PASSWORD` | Password for the server's Admins role. Required. |
| `ENSHROUDED_PLAYER_PASSWORD` | Password for the Friends role. Required. |
| `ENSHROUDED_GUEST_PASSWORD` | Password for the Guests role. Required. |

Everything else already has a working default in `docker-compose.example.yml`
(`TAKARO_WS_URL` = `wss://connect.takaro.io/`, `TAKARO_PLUGIN_URL` = `http://127.0.0.1:18890`,
log tailing, cursor file, health port 18891). Change `SERVER_NAME`, `SERVER_SLOT_COUNT`,
`TAKARO_SERVER_NAME` and `TZ` in the compose file to taste.

UDP port **15637** is published for game clients. The plugin's HTTP API stays on loopback inside the
container and is never exposed to the host.

### 5. Check that it worked

Start everything:

```bash
docker compose -f docker-compose.example.yml --env-file .env up -d --build
docker compose -f docker-compose.example.yml logs -f
```

In the plugin's own log, `data/enshrouded/server/takaro/plugin.log`:

```
takaro enshrouded plugin 0.4.2 starting (pid ...)
http: token from env TAKARO_PLUGIN_TOKEN
http: listening on 127.0.0.1:18890
game build: 1024233
capabilities: {...}
```

If you see `http: WARNING no token configured; all requests will be rejected with 401`, the shared
secret did not reach the game container — re-check `TAKARO_ENSHROUDED_PLUGIN_TOKEN` in `.env`.

In the sidecar log (`docker logs enshrouded-takaro-sidecar`):

```
Connecting to Takaro at wss://connect.takaro.io/
Takaro WebSocket open, sending identify
Takaro confirmed WebSocket connection
Identified with Takaro (gameServerId=...)
Sidecar health on http://127.0.0.1:18891/health; plugin http://127.0.0.1:18890
```

And in Takaro, the game server shows as **online**. If it stays offline, the registration token in
`.env` is the first thing to re-check. From inside the container,
`curl http://127.0.0.1:18891/health` shows per-capability status; anything reported as `degraded`
means a game update moved code the plugin hooks (see Known issues).

### 6. Upgrading

**Stop the game container first** — the running server holds `dbghelp.dll` open.

```bash
./mod/build.sh
docker compose -f docker-compose.example.yml stop enshrouded
cp mod/build/dbghelp.dll data/enshrouded-plugin/dbghelp.dll
docker compose -f docker-compose.example.yml up -d --build
```

Replacing the DLL in place keeps the bind mount valid. Your `.env`, the world in `data/enshrouded/`
and the sidecar state in `data/enshrouded-sidecar/` (event cursor, online players) survive the
upgrade — keep the cursor file so events are not replayed. Rebuilding with `--build` also picks up
a newer sidecar.

## What works, what doesn't

Verified end to end on **2026-09-13/14** against a real dedicated server (game build **1024233**,
plugin **0.4.2**) with a real game client connected.
✅ = works, ⚠️ = works with a caveat or was not verified live, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The sidecar keeps an outbound WebSocket to Takaro and reconnects by itself. |
| Server restart / reconnect | ✅ | Comes back on its own after a game-container restart; takes up to ~3 minutes. Players still online when the server died are reconciled and reported as disconnected. |
| Player list | ✅ | Names are the Steam persona names, not the in-game character names — the server never exposes those. `gameId` is the SteamID64. |
| Single player lookup | ✅ | Same data as the player list. |
| Player location | ✅ | Matches the in-game position within 0.2 m and follows teleports. |
| Player inventory | ✅ | Matches the in-game backpack. Takaro's inventory history only records changes, so unchanged starting equipment never shows up there. |
| Item catalogue | ✅ | 3,609 items synced. |
| Entity catalogue | ✅ | 979 creature/NPC templates (Takaro stored 977 — two share a code). This is the fixed template list, not live creatures. |
| Locations / points of interest | ⚠️ | The plugin has 1,031 map locations and the sidecar serves them, but Takaro never asks for them, so this cannot be checked end to end. |
| Chat messages from players | ✅ | Real player chat reaches Takaro with the player attached. |
| Broadcast a message | ✅ | Shown to everyone in the server chat, but under the character name of an online player — Enshrouded has no "server" sender. |
| Whisper a player | ⚠️ | The message reaches the intended player. Only one player account was available, so "nobody else sees it" was never confirmed. |
| Give an item | ✅ | Works for normal players and for Admins-group players. The game ignores the count, so the plugin splits the request into stacks of at most 64. |
| Teleport a player | ✅ | Takaro rounds the coordinates to whole numbers and the game nudges the player to the nearest free spot. |
| Run a console command | ✅ | Enshrouded has no console; the plugin provides its own set: `help`, `players`, `say`, `whisper`, `location`, `teleport`/`tp`, `inventory`, `give`, `item`, `kick`, `save-and-shutdown`. There is no time-of-day command. |
| Kick | ✅ | The player must be online. Also works on Admins-group players: the game normally refuses that, and plugin 0.4.2 lifts the check for the one call. Plugin 0.4.1 and older silently did nothing for those players. |
| Ban (timed and permanent) | ⚠️ | Bans work on **online** players only, and are always permanent until unbanned — the game has no ban reason and no expiry, so a timed ban from Takaro becomes a permanent one. |
| Unban | ✅ | The player must have been seen online at least once while the connector was running (the game identifies accounts by a hash, not the SteamID). |
| Ban list | ✅ | Matches the server's own banned-accounts list. |
| Shut the server down | ✅ | Saves first, then quits; the container restart policy brings it back. |
| Player joined event | ✅ | From real joins; hooks fire from it. |
| Player left event | ✅ | From real leaves, and from sidecar reconciliation after the server was killed with a player online. |
| Player chat event | ✅ | See "Chat messages from players". |
| Player death event | ⚠️ | Falls and deaths by a creature both reach Takaro. The creature name is now included in the message text; that change was only unit-tested, never re-checked live. |
| Entity kill event | ✅ | Real kills reach Takaro. The weapon field stays empty, and destroyed props/voxels are filtered out. |
| Log events | ⚠️ | The sidecar sends them, but Takaro does not store server log lines as events, so they cannot be searched or used in modules. |
| Map info | ⚠️ | Not verified in a live test; nothing in the evidence records a map-info call for this connector. |
| Map tiles | ❌ | The Takaro API does not support map tiles for Generic-connector servers. |
| Discord chat bridge | ⚠️ | Game → Discord, Discord → game (from a real human account) and join/leave notices were all seen. The built-in `chatBridge` also posts Takaro's own server messages back to Discord (a Takaro-core echo affecting every game); use the `chatBridgeNoEcho` fork, which skips player-less chat and was proven to relay player chat without the echo. |
| Shop & economy | ⚠️ | Buying in game, ordering through Takaro and claiming later, refusal when the balance is too low, and balance/top-list commands were all exercised in the 2026-09-14 hard test, but there is no evidence file for that run in the workspace. Currency transfer was only tried player-to-self (one account), and in-game `@claim` needs the player's account linked to Takaro. A failed purchase shows the generic in-game message "Oops, something went wrong". |

### Known issues

- **Bans cannot expire and need the player online.** Enshrouded has no ban reason or end date, so
  every ban is permanent until you unban; unban needs the player to have joined once while the
  connector was running.
- **Whispers are not proven private.** They reach the right player, but with a single test account
  it was never confirmed that others do not see them.
- **Discord bridge echoes with the stock module.** Takaro stores its own outgoing server messages as
  chat and `chatBridge` relays them to Discord. Use `chatBridgeNoEcho`.
- **A game update can switch a feature off.** The plugin finds game code by shape and self-checks at
  load; after a game update a mismatching feature reports `degraded` in `/health` and in Takaro's
  reachability reason, while the server and everything else keep running. It then needs a new
  plugin build with re-derived signatures.
- **Player names are Steam persona names**, not the in-game character names.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
