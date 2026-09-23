# Takaro VEIN Connector

A server-side-only connector (plugin + sidecar) for a VEIN **Linux dedicated server**; players install nothing. Install both parts.

## Install

Download the latest release: <https://takaro.io/connectors/vein>

### 1. Before you start

- A **VEIN Linux dedicated server** (Steam app **2131400**) you can stop, start, copy files to and change the launch line of.
- **Docker** with the Compose plugin, or **Node.js 22**, on the game server host.
- A **Takaro** game server of type **Generic** and its **registration token**.

Nothing has to be compiled.

### 2. Download

From the latest `vein-vX.Y.Z` release on <https://github.com/gettakaro/connectors/releases>:

- **`takaro-vein-plugin.tar.gz`** — the game-server plugin (`libtakaro-vein.so`)
- **`takaro-vein-sidecar.tar.gz`** — the sidecar that talks to Takaro
- **`SHA256SUMS`** — verify with `sha256sum -c SHA256SUMS`

Not the "Source code" links, not `vein-dev`, not `pr-<number>-vein`.

### 3. Copy it into place

Stop the game server, then unpack both archives:

```bash
mkdir -p data/vein data/vein-plugin data/vein-sidecar
tar -xzf takaro-vein-plugin.tar.gz && cp TakaroVein/libtakaro-vein.so data/vein-plugin/
tar -xzf takaro-vein-sidecar.tar.gz && mv TakaroVeinSidecar sidecar
mv sidecar/docker-compose.example.yml sidecar/.env.example .
```

```
<your compose dir>/
    docker-compose.example.yml    <- from the sidecar archive
    .env.example                  <- from the sidecar archive
    sidecar/                      <- TakaroVeinSidecar renamed (dist/, Dockerfile, package.json)
    data/vein/                    (game data)
    data/vein-plugin/libtakaro-vein.so
    data/vein-sidecar/            (the sidecar's /data volume)
```

Keep the `.so` **outside the Steam/game tree**: a SteamCMD `validate` deletes files it does not know.

### 4. Load the plugin

Put `LD_PRELOAD` on the game binary's launch line:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-vein.so ./Vein/Binaries/Linux/VeinServer-Linux-Test -Port=7777 -QueryPort=27015 -log
```

The game process also needs `TAKARO_PLUGIN_TOKEN` in its environment (the example compose passes it).

Never set `LD_PRELOAD` globally for the container, user or service — 32-bit steamcmd fails if it inherits it.

### 5. Configure

`cp .env.example .env` and fill in:

| Key | What to put there |
|---|---|
| `TAKARO_PLUGIN_TOKEN` | Shared secret for game server and sidecar, e.g. `openssl rand -hex 32`. Required. |
| `TAKARO_REGISTRATION_TOKEN` | Your Takaro registration token. Required. |
| `TAKARO_IDENTITY_TOKEN` | A name for this server in Takaro, e.g. `my-vein-server`. Required. |
| `TAKARO_PLUGIN_URL` | Where the sidecar reaches the plugin. `http://127.0.0.1:18890`. Required. |
| `VEIN_LOG_FILE` | Path to the server log, e.g. `.../Vein/Saved/Logs/Vein.log`. Required. |
| `TAKARO_CURSOR_FILE` | `/data/event-cursor.json`, on a volume that survives restarts (`./data/vein-sidecar`). Keeps the event cursor; without it events replay after a restart. |
| `TAKARO_ADMIN_STEAMIDS` | Comma-separated SteamID64s to grant in-game admin. |
| `TAKARO_PLUGIN_DEBUG` | Optional: `1` for verbose plugin logging. |

The sidecar must reach the plugin on loopback, so run it in the game server's network namespace or on its host.

Docker: `docker compose -f docker-compose.example.yml --env-file .env up -d --build`.
Node.js 22: `cd sidecar && npm ci --omit=dev && node dist/index.js`.

### 6. Check that it worked

In the plugin log (`<serverdir>/Vein/Binaries/Linux/takaro/plugin.log`):

```
takaro vein plugin <version> starting (pid ..., bootId ...)
http: listening on 127.0.0.1:18890
```

In the sidecar log:

```
Identified with Takaro (gameServerId=...)
```

`curl http://127.0.0.1:18891/health` reports `"ok": true` and `"takaroIdentified": true`, plus the
plugin version and the state of every capability.

And the server shows **online** in Takaro. If it stays offline, re-check the registration token.

### 7. Upgrading

Stop the game server (it holds the `.so` open). Replace `libtakaro-vein.so` and the `sidecar/` folder with the ones from the same release, then start again.
Your `.env`, the world and `data/vein-sidecar/` survive; confirm the new version in the plugin's starting log line.

## What works, what doesn't

Verified end to end on 2026-09-17 against game build 25035268 (v0.024h8) with a real client, on the
connector release. ✅ = works, ⚠️ = works with a caveat, ❌ = does not work / is not supported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The server shows as reachable while the sidecar is up. |
| Player list | ✅ | Name, ping and spawned state; `gameId` is the SteamID64. |
| Single player lookup | ✅ | Same data for one player; offline players answer with a last-known record. |
| Player location | ✅ | The live position, following walking and teleports. |
| Player inventory | ✅ | Matches the in-game bag; empty while the player is dead. |
| Give an item | ✅ | The item appears in the bag without a relog. |
| Item catalogue | ✅ | Every loaded item class. Readable names need `TAKARO_ITEM_NAMES=1`. |
| Entity catalogue | ⚠️ | Creature types sync, but Takaro never deletes rows from older builds. |
| Locations / points of interest | ⚠️ | Served by the connector, but Takaro never asks for them. |
| Run a console command | ✅ | The connector's own set: `help`, `players`, `say`, `give`, `tp`, `ban`, `save`, … |
| Broadcast a message | ✅ | Everyone sees it, as a chat line prefixed with the server name. |
| Whisper a player | ✅ | Reaches the one player, as an on-screen notification. |
| Teleport a player | ✅ | The player is moved to the requested position. |
| Kick a player | ✅ | The player is dropped and can rejoin afterwards. |
| Ban a player (timed and permanent) | ✅ | Works offline too; the connector lifts timed bans at expiry. |
| Unban a player | ✅ | The player can rejoin at once. |
| Ban list | ✅ | The game's bans plus the connector's, with reason and expiry. |
| Shut the server down | ✅ | Saves the world first, then quits cleanly. |
| Player joined event | ✅ | Arrives on every join, with the SteamID64. |
| Player left event | ✅ | Arrives on a clean quit and after a crash. |
| Player chat event | ✅ | Real player chat reaches Takaro; the connector's own messages are not echoed. |
| Player death event | ✅ | Position and cause included; falls and drowning have no attacker. |
| Entity kill event | ✅ | Creature, player and held item. AI-on-AI kills are not reported. |
| Log events | ⚠️ | Forwarded with secrets redacted, but Takaro does not store log events. |
| Map info | ❌ | Takaro does not support map info for Generic game servers. |
| Map tiles | ❌ | Takaro does not support map tiles for Generic game servers. |
| Modules: chat commands | ✅ | In-game commands reach the module and answer in chat. |
| Modules: hooks | ✅ | Chat and join hooks fire and run their code. |
| Modules: cronjobs | ✅ | Scheduled module runs fire and can message the server. |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ | The teleports module's in-game commands move the player. |
| Modules: server messages / onboarding | ✅ | Timed messages and the welcome message are delivered in game. |
| Shop: buy in game | ✅ | Buying with the in-game chat command works. |
| Shop: order in Takaro and claim in game | ✅ | The items are delivered to the player. |
| Shop: bundle of several items | ✅ | One claim delivers every item in the listing. |
| Shop: order while offline, claim later | ✅ | Refused while offline, succeeds after rejoining. |
| Shop: not enough currency | ✅ | The purchase is refused and the balance is unchanged. |
| Economy: currency | ✅ | Balances are set, read and debited by Takaro. |
| Economy: balance / top list in game | ✅ | The in-game economy commands answer in chat. |
| Discord: game chat → Discord | ✅ | In-game chat is relayed to the linked channel. |
| Discord: Discord → game chat | ⚠️ | Works, but only a real human post can confirm it — bots are ignored. |
| Discord: module hook / cronjob posts | ✅ | Hooks and cronjobs post to Discord and edit their messages. |
| Discord: join/leave notices | ✅ | Posted to Discord by the chat-bridge module. |
| Discord: no echo of server messages | ✅ | Stock `chatBridge` re-posts server messages; the `chatBridgeNoEcho` fork does not. |
| Events while the Takaro connection is down | ✅ | Kept and delivered in order once the connection is back. |
| Reconnects after a server or container restart | ✅ | Re-identifies on its own; keep the sidecar's restart policy on. |
| No duplicate events after a connector restart | ✅ | The event cursor is persisted, so nothing is replayed. |
| Survives a network drop to Takaro | ✅ | The WebSocket reconnects by itself and re-identifies. |
| Timed bans expire on their own | ✅ | Lifted at expiry, including across a restart. |
| Keeps running after a game update breaks a feature | ✅ | The broken feature reports `degraded`; everything else keeps working. |

### Known issues

- Discord → game chat can only be confirmed by a real human message; the bridge ignores bots.
- A broadcast shows as a chat line prefixed with the server name; a whisper as an on-screen notification.
- Local and global chat cannot be told apart — proximity chat also reports as global.
- `AdminSteamIDs` in `Game.ini` is ignored by the game. Use `TAKARO_ADMIN_STEAMIDS` instead.
- The server log contains the join password and Steam tickets; the connector redacts them from Takaro.
- Renamed or removed items and creatures linger in Takaro's catalogue, because its sync never deletes.
- A timed ban shows as permanent in Takaro until gettakaro/takaro#3981; it is still lifted on time.
- Characters do not survive a server restart. This is the game's own behaviour — warn your players.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
