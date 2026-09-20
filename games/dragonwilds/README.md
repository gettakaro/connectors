# Takaro RuneScape: Dragonwilds Connector

A server-side-only connector (plugin + sidecar) that connects a RuneScape: Dragonwilds **Linux
dedicated server** to Takaro. Tested against Steam build **25110402** (UE 5.6.1
`++dominion+staging-CL-240163`) with a real game client (client build **25111146**) connected.
Players install nothing.

The plugin alone cannot talk to Takaro, and the sidecar alone cannot read players, positions,
inventories, items or entities. Install both.

## Install

Download the latest release: https://takaro.io/connectors/dragonwilds

### 1. Before you start

You need:

- A **Dragonwilds Linux dedicated server** (Steam app **4019830**) you can stop, start and copy
  files to, and the ability to change how the server binary is launched (this connector is loaded
  with `LD_PRELOAD`; there is no mod folder).
- The server's own **`RSDragonwildsServer-Linux-Shipping.sym`** file, which SteamCMD downloads next
  to the server binary. The plugin reads it to find the game's functions. If you deleted it to save
  space, re-run `app_update 4019830 validate` to get it back.
- **Docker** with the Compose plugin on the same host (the sidecar runs as a container that shares
  the game container's network), or **Node.js 22** on the game server host.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

Nothing has to be compiled: both parts are published as release assets.

### 2. Download

From the latest `dragonwilds-vX.Y.Z` release on the releases page:

> https://github.com/gettakaro/connectors/releases

Download both files:

- **`takaro-dragonwilds-plugin.tar.gz`** — the game-server plugin (`libtakaro-dragonwilds.so`)
- **`takaro-dragonwilds-sidecar.tar.gz`** — the sidecar that talks to Takaro

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/dragonwilds-v<version>/takaro-dragonwilds-plugin.tar.gz`

The two **"Source code (zip/tar.gz)"** links GitHub adds to every release are an archive of this
whole repository, not the connector — do not download those. Do not use the `dragonwilds-dev`
pre-release or a `pr-<number>-dragonwilds` build either; those are untested rolling builds.

### 3. Copy it into place

Stop the game server first.

**Plugin.** `takaro-dragonwilds-plugin.tar.gz` contains one folder:

```
TakaroDragonwilds/
    libtakaro-dragonwilds.so
    README.txt
```

Put the `.so` **outside the Steam/game tree** — a SteamCMD `app_update … validate` deletes files it
does not know about, and that includes this one. With `docker-compose.example.yml` that is
`data/dragonwilds-plugin/libtakaro-dragonwilds.so` on the host, bind-mounted **read-only** to
`/opt/takaro/libtakaro-dragonwilds.so` in the game container. On a plain (non-Docker) server, use a
directory next to the Steam tree, e.g. `<server>/takaro/libtakaro-dragonwilds.so`.

**Sidecar.** `takaro-dragonwilds-sidecar.tar.gz` contains one folder,
`TakaroDragonwildsSidecar/`, with `dist/`, `package.json`, `package-lock.json`, `Dockerfile`,
`.dockerignore` and `.env.example`. `docker-compose.example.yml` builds the sidecar image from
`./sidecar`, so unpack it next to the compose file and **rename the folder to `sidecar`**:

```
<your compose dir>/
    docker-compose.example.yml
    .env
    sidecar/                       <- TakaroDragonwildsSidecar renamed
    data/
        dragonwilds/               (game server data, created by the container)
        dragonwilds-plugin/
            libtakaro-dragonwilds.so
        dragonwilds-sidecar/       (event cursor and online state, created by the container)
```

```bash
mkdir -p data/dragonwilds-plugin data/dragonwilds-sidecar
tar -xzf takaro-dragonwilds-plugin.tar.gz
cp TakaroDragonwilds/libtakaro-dragonwilds.so data/dragonwilds-plugin/
tar -xzf takaro-dragonwilds-sidecar.tar.gz && mv TakaroDragonwildsSidecar ./sidecar
```

### 4. Load the plugin into the server

The plugin is loaded with `LD_PRELOAD`, and **only onto the game binary**. SteamCMD is 32-bit and
fails outright if it sees a 64-bit preload, so never set `LD_PRELOAD` globally for the container or
the user account — set it on the launch line of the server binary itself.

Plain server (shell script or systemd unit):

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so \
TAKARO_PLUGIN_TOKEN=<your shared secret> \
  ./RSDragonwildsServer.sh -log
```

Pterodactyl / Pelican — put the same two variables in front of the server binary in the egg's
startup command, for example:

```
LD_PRELOAD=/home/container/takaro/libtakaro-dragonwilds.so TAKARO_PLUGIN_TOKEN={{TAKARO_PLUGIN_TOKEN}} ./RSDragonwildsServer.sh -log
```

(add `TAKARO_PLUGIN_TOKEN` as an egg variable; leave the SteamCMD install script untouched).

Docker — `docker-compose.example.yml` shows the shape: the image's entrypoint must apply
`LD_PRELOAD="${TAKARO_PLUGIN_SO}"` to the server launch line only.

To confirm the plugin is really loaded: `grep libtakaro /proc/<server pid>/maps`.

### 5. Configure

Copy the example environment file and fill it in:

```bash
cp .env.example .env
```

| Key | Where | What to put there |
|---|---|---|
| `TAKARO_PLUGIN_TOKEN` | game server **and** sidecar | A long random shared secret, e.g. `openssl rand -hex 32`. Without it the plugin rejects every request with 401. Required. |
| `TAKARO_PLUGIN_PORT` | game server | Plugin loopback HTTP port, default `18890`. |
| `TAKARO_REGISTRATION_TOKEN` | sidecar | Your Takaro registration token. Required. |
| `TAKARO_IDENTITY_TOKEN` | sidecar | A name that identifies this server to Takaro, e.g. `my-dragonwilds-server`. Required. |
| `TAKARO_PLUGIN_URL` | sidecar | Where the plugin is, default `http://127.0.0.1:18890`. |
| `DRAGONWILDS_LOG_FILE` | sidecar | The server's log, e.g. `/game/RSDragonwilds/Saved/Logs/RSDragonwilds.log`. Used for join/leave lines and log events. |
| `TAKARO_CURSOR_FILE` | sidecar | Persisted event cursor, so a sidecar restart replays nothing. |

Instead of environment variables the plugin also reads `<serverdir>/takaro/plugin.json`
(`{"token": "...", "port": 18890}`) — handy on hosts where you cannot set variables on the process.

**The sidecar must reach the plugin on loopback.** The plugin's HTTP API binds `127.0.0.1` only and
is never exposed to the network, so the sidecar has to share the game server's network namespace
(compose: `network_mode: "service:dragonwilds"`) or run directly on the game server host.

### 6. Check that it worked

Start everything:

```bash
docker compose -f docker-compose.example.yml --env-file .env up -d --build
```

In the plugin's own log, `<serverdir>/takaro/plugin.log`:

```
takaro dragonwilds plugin <version> starting (pid ..., bootId ...)
http: listening on 127.0.0.1:18890
```

If you see a warning that no token is configured, `TAKARO_PLUGIN_TOKEN` did not reach the game
process. From the sidecar (or the game host):

```bash
curl -H "Authorization: Bearer $TAKARO_PLUGIN_TOKEN" http://127.0.0.1:18890/health
```

must answer `"status": "ok"`. The sidecar's own health endpoint,
`curl http://127.0.0.1:18891/health`, answers `"status": "ok"` too and lists each capability; a
capability reported as `degraded` means a game update moved code the plugin uses (see Known issues)
— everything else keeps working.

In the sidecar log (`docker logs dragonwilds-takaro`):

```
Takaro WebSocket open, sending identify
Identified with Takaro (gameServerId=...)
```

And in **Takaro the game server shows as online**. If it stays offline, the registration token is
the first thing to re-check.

### 7. Upgrading

**After a game update** you normally do nothing: the plugin resolves the game's functions from the
`.sym` file that ships with the update, notices the new build id, throws away its cache and
re-resolves on the next start. Check `/health` afterwards — if a capability reports `degraded`, the
game renamed something and that one feature needs a new plugin build.

**To upgrade the connector**, stop the game server (it holds the `.so` open), replace
`data/dragonwilds-plugin/libtakaro-dragonwilds.so`, replace the `sidecar/` folder with the new one,
and start again with `--build`. Take both files from the same release. Your `.env`, the world and
the sidecar state in `data/dragonwilds-sidecar/` (event cursor, online players) survive the upgrade
— keep the cursor file so events are not replayed. Confirm the new version in the
`takaro dragonwilds plugin <version> starting` log line.

## What works, what doesn't

Verified end to end on **2026-09-16** against a real dedicated server (game build **25110402**,
plugin **0.1.0**) with a real game client connected: every row was driven from Takaro and checked
again on the game side.
✅ = works, ⚠️ = works with a caveat or was not verified live, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The sidecar keeps an outbound WebSocket to Takaro and the server shows as reachable while it is up. |
| Player list | ✅ | Character name, ping and position state. `gameId` is the player's EOS Product User Id (the 32-character id at the bottom of the in-game Settings screen); `platformId` is `epic:<that id>`. |
| Single player lookup | ✅ | Same data as the player list, for one player. |
| Player location | ✅ | Matches the in-game position to the metre and follows walking and teleports. |
| Player inventory | ✅ | Matches what the player is carrying, item by item. |
| Give an item | ✅ | The item appears in the player's inventory without a relog. |
| Item catalogue | ✅ | 1,536 items synced — the server's full item list. |
| Entity catalogue | ⚠️ | Only creatures that have been loaded in the world so far. Dragonwilds streams creatures in on demand, so this is never the complete bestiary and it grows as the world is played. |
| Locations / points of interest | ⚠️ | The connector serves the world's lodestones, but Takaro does not use this, so it cannot be checked end to end. |
| Run a console command | ✅ | Dragonwilds has no operator console; the connector provides its own set (`help`, `players`, `say`, `whisper`, `give`, `tp`, `kick`, `ban`, `save`, `shutdown`, …). An unknown command comes back as a failure with the reason. |
| Broadcast a message | ⚠️ | Everyone sees the message, but it appears under the receiving player's own name with a `[sender]` prefix — the game has no server sender. |
| Whisper a player | ⚠️ | The message reaches the intended player (stored by Takaro as a whisper). With a single test account it could not be confirmed that nobody else sees it. |
| Teleport a player | ✅ | The player is moved to the requested position; the game snaps them to the ground. |
| Kick a player | ✅ | The player is dropped from the server and can rejoin afterwards. |
| Ban a player (timed and permanent) | ⚠️ | The player is disconnected immediately and refused on rejoin — the game's own login gate only reads its ban list at start-up, so the plugin enforces the ban at login and drops the player again within two seconds. Timed bans are lifted by the connector when they expire; the game has no expiry of its own. |
| Unban a player | ✅ | Clears the ban in the server config and in the plugin, and the player can rejoin. |
| Ban list | ✅ | Shows the server's own bans. Reason and expiry are kept by the connector, because the game stores neither. |
| Shut the server down | ✅ | Saves the world first, then quits; your restart policy brings it back. |
| Player joined event | ✅ | Arrives in Takaro on every join. |
| Player left event | ✅ | Arrives on a clean quit, on a kick, and after a server crash by reconciliation. |
| Player chat event | ✅ | Real player chat reaches Takaro; messages the connector itself sent are not echoed back. |
| Player death event | ✅ | Reaches Takaro with the death message. |
| Entity kill event | ✅ | Proven with real sword kills: the creature's readable name (e.g. Magpie, Giant Rat), the killing player and the weapon held (e.g. Adamant Sword) arrive in Takaro. With several players nearby, the killer is the one holding the weapon that dealt the blow; if that cannot be read, the nearest player is reported. |
| Log events | ⚠️ | The connector forwards server log lines (with passwords redacted), but Takaro does not store log lines as events, so they cannot be searched or used in modules. |
| Map info | ❌ | Not implemented by this connector. |
| Map tiles | ❌ | Not supported by Takaro for this connector type. |
| Modules: chat commands | ✅ | In-game chat commands with the domain's prefix reach the module and answer in chat. |
| Modules: hooks | ✅ | Chat and join hooks fire and run their module code. |
| Modules: cronjobs | ✅ | Scheduled module runs fire and can message the server. |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ | The teleports module's in-game commands move the player. |
| Modules: server messages / onboarding | ✅ | Timed server messages and the welcome message on join are delivered in game. |
| Shop: buy in game | ✅ | Buying from the shop with the in-game chat command. |
| Shop: order in Takaro and claim in game | ✅ | An order placed in Takaro delivers the items to the player. |
| Shop: bundle of several items | ✅ | One claim delivers every item in the listing. |
| Shop: order while offline, claim later | ✅ | The claim is refused while the player is offline and succeeds after rejoining. |
| Shop: not enough currency | ✅ | The purchase is refused and the balance is unchanged. |
| Economy: currency | ✅ | Balances are set, read and debited by Takaro. |
| Economy: balance / top list in game | ✅ | The in-game economy commands answer in chat. |
| Discord: game chat → Discord | ✅ | In-game chat is relayed to the linked Discord channel. |
| Discord: Discord → game chat | ✅ | A message posted in the linked Discord channel appears in the game chat as `[D] <name>: <text>`. |
| Discord: module hook / cronjob posts | ✅ | Module hooks and cronjobs can post to Discord and edit their own messages. |
| Discord: join/leave notices | ✅ | Join and leave notices posted to Discord by the chat-bridge module. |
| Discord: no echo of server messages | ✅ | The stock `chatBridge` module re-posts Takaro's own server messages to Discord (a Takaro-core echo affecting every game); the `chatBridgeNoEcho` fork does not. |
| Events while the Takaro connection is down | ✅ | Events that happen while Takaro is unreachable are kept and delivered in order once the connection is back; the connector notices a dead socket within about 30 seconds. |
| Reconnects after a server or container restart | ✅ | The connector comes back and re-identifies on its own, and players who were online are reported as disconnected. If the sidecar shares the game container's network (the docker-compose example), restart the sidecar together with the game container; on its own it recovers within about three minutes. |
| No duplicate events after a connector restart | ✅ | The event cursor is persisted, so a sidecar restart replays nothing. |
| Survives a network drop to Takaro | ✅ | The WebSocket reconnects by itself with a backoff of 2 to 60 seconds and re-identifies as the same server. |
| Timed bans expire on their own | ✅ | The connector lifts a timed ban when it runs out, including when it was restarted in between. |
| Keeps running after a game update breaks a feature | ⚠️ | The plugin self-checks at load and a feature it can no longer find reports `degraded` in `/health` and in Takaro's reachability reason while the server and everything else keep running. Exercised in development, not in a live game update. |

### Known issues

- **The server needs an owner id before it starts a world.** Dragonwilds' dedicated server idles on
  its start-up map until `OwnerId` is set to the owner's EOS Product User Id (the 32-character id at
  the bottom of the in-game Settings screen). That is a game requirement, not a connector one, but
  the connector reports the server as degraded until a world exists.
- **Client and server are version-locked.** After a game update, players on the old client cannot
  join. Update the server and the client together, and keep automatic updates off if you want to
  choose the moment.
- **`LD_PRELOAD` must be set on the game binary only.** SteamCMD is a 32-bit program and fails
  immediately if it inherits a 64-bit preload, so never set it for the whole container, user or
  service — only on the line that starts the server binary.
- **A SteamCMD `validate` deletes the plugin if it lives in the Steam tree.** Keep
  `libtakaro-dragonwilds.so` in a directory outside the game install (mounted read-only in Docker).
- **The server writes the world password into its own log in cleartext.** The connector redacts it
  before anything is forwarded to Takaro, but the file on disk still contains it — do not paste
  server logs into public issues.
- **Broadcasts and whispers render under the receiving player's name**, with a `[sender]` prefix.
  The game has no "server" chat sender, so this cannot be fixed from the outside. Whispers reach the
  right player, but with a single test account it was never confirmed that no one else sees them.
- **Bans are enforced by the plugin, not by the game's login gate.** The game only reads its ban
  list when it starts, so a banned player briefly connects and is dropped again by the plugin within
  about two seconds. Timed bans are lifted by the connector; if the connector is down at the moment a
  ban expires, it is lifted when it comes back.
- **The entity catalogue is not the full bestiary.** Dragonwilds streams creatures in on demand, so
  only the ones the server has loaded so far are known.
- **Log events are forwarded but not stored.** Takaro does not keep server log lines as searchable
  events, so they cannot be used in modules.
- **There is no map.** Takaro does not support map tiles for this connector type.
- **A game update can switch a feature off.** The plugin finds game code by name in the server's own
  symbol file and self-checks at load; after a game update a feature it can no longer find reports
  `degraded` in `/health` and in Takaro's reachability reason, while the server and everything else
  keep running. That feature then needs a new plugin build.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
