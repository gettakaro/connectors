# Takaro Dune: Awakening Connector

Connects a self-hosted Dune: Awakening battlegroup to Takaro. A sidecar (a small Node.js service
next to the battlegroup) is required; a native plugin for the map server is optional and adds live
positions and kill events. Players do not install anything.

## Install

### 1. Requirements

- A self-hosted Dune battlegroup (Linux) with a Funcom self-hosting token, on a supported build:
  26081962, 26051014, 25936522, 25789279 or 25689360.
- Docker Compose, or Node.js 22, on a host that can reach the battlegroup's Postgres and game RabbitMQ.
- A Takaro game server of type **Generic** and its registration token.
- About 16 GB of free memory for one map server; the CPU must support AVX2.

### 2. Download

From <https://takaro.io/connectors/dune> or the latest `dune-v*` release on
<https://github.com/gettakaro/connectors/releases>, download the archives whose `<build>` matches
your server, and check them against `SHA256SUMS`:

- `takaro-dune-sidecar-linux-<build>-<version>.tar.gz` (required)
- `takaro-dune-plugin-linux-<build>-<version>.tar.gz` (optional)

### 3. Configure the map server

Add both arguments to every map server process:

```text
-ini:engine:[ConsoleVariables]:server.NotificationSystem.Enabled=true
-ini:engine:[FuncomLiveServices]:ServerCommandsAuthToken=<a-long-random-secret>
```

Add these lines to `DuneSandbox/Saved/Config/LinuxServer/UserGame.ini`, then restart the map server:

```ini
[AdminSetting.Global]
+Allowed_GM_Commands=KickPlayer
+Allowed_GM_Commands=ServiceBroadcast
```

### 4. Install the sidecar

Unpack the sidecar archive and rename its folder to `sidecar`. Move `docker-compose.example.yml`
and `.env.example` out of it, next to it, and rename `.env.example` to `.env` (an upgrade replaces
`sidecar/`, so `.env` must live outside it):

```text
dune-connector/
├── docker-compose.example.yml
├── .env
└── sidecar/
```

Set these values in `.env`:

| Setting | Value |
|---|---|
| `TAKARO_REGISTRATION_TOKEN` | Registration token of the Takaro game server. |
| `TAKARO_IDENTITY_TOKEN` | Leave empty; the sidecar creates one and keeps it in `/data`. |
| `TAKARO_SERVER_NAME` | Optional; must be unique in Takaro. Empty uses `Dune (<identity>)`. |
| `TAKARO_SENDER_NAME` | The same value as Takaro's `serverChatName` setting. |
| `DUNE_PG_URL` | Read-only Postgres URL. The database name differs per build; list them with `\l`. |
| `DUNE_RMQ_URL` | AMQPS URL of the game RabbitMQ (not the admin RabbitMQ). |
| `DUNE_RMQ_TLS_INSECURE` | `true` with Funcom's self-signed broker certificate. |
| `DUNE_GM_AUTH_TOKEN` | The same secret as `ServerCommandsAuthToken` in step 3. |
| `DUNE_GM_PUBLISHER` | `amqp`, `docker-exec` or `kubectl-exec`, see below. |
| `DUNE_LOG_FILE` | Optional: map-server log path, for log events and the game version in Takaro. |

Pick the publisher that matches your battlegroup:

- `amqp`: enable RabbitMQ's `internal` authentication backend and create an `fls` user.
- `docker-exec`: set `DUNE_RMQ_CONTAINER` and mount the Docker socket.
- `kubectl-exec`: set `DUNE_K8S_NAMESPACE` and `DUNE_K8S_POD` (stock Funcom k3s).

Start it:

```bash
docker compose -f docker-compose.example.yml --env-file .env up -d --build
```

Without Docker, run `npm ci --omit=dev`, `npm run catalogue` and `node dist/index.js` inside
`sidecar/`, with the `DUNE_*` values in the environment (the `TAKARO_*` values are read from `../.env`).

The token, identity, name and URL apply within a few seconds of saving `.env`; other changes need
`docker compose ... up -d` again.

### 5. Check that it worked

The sidecar log shows `Identified with Takaro`, the server is online in Takaro, and
`http://127.0.0.1:18891/health` reports `ok`. Without a token the log shows a
`TAKARO_REGISTRATION_TOKEN not set` banner.

### 6. Optional: plugin

The plugin adds live player positions, entity kill events and detailed death causes. Stop the map
server, unpack `libtakaro-dune.so` outside the game directory and start only the map process with it:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dune.so \
TAKARO_PLUGIN_TOKEN=<shared-secret> \
./DuneSandbox/Binaries/Linux/DuneSandboxServer-Linux-Shipping ...
```

In `.env`, set `DUNE_PLUGIN_TOKEN` to the same secret and `DUNE_PLUGIN_URL=http://127.0.0.1:18890`.
The sidecar must run on the map server's host or share its container network. Never preload the
library into the whole battlegroup.

### 7. Optional: shutdown and map

- **Shutdown:** Dune's own shutdown only shows a countdown. Set `DUNE_SHUTDOWN_CMD` to a script that
  stops your map server, or Takaro's shutdown stays unavailable.
- **Map:** in the Takaro dashboard, open the game server's map and upload an image for
  **Hagga Basin**. Use the game's minimap texture `T_Survival_WorldMap_MiniMap_D` (4096 x 4096),
  exported as PNG from your own game client (`DuneSandbox/Content/Paks`, for example with FModel),
  uncropped. Do not enter any bounds.

### Upgrade

Replace only the `sidecar` folder with the new one and run the start command again. Keep `.env` and
the `/data` volume (identity and bans). For the plugin, stop the map server, replace the `.so` with
the one for your build and start it again.

## What works, what doesn't

Tested on server build 26081962 (game 1.5.15.2) on 2026-10-26.

| Feature | Status | Note |
|---|---|---|
| Connection and reconnect | ✅ | Recovers after network, server or sidecar restarts. |
| Player list and lookup | ✅ | Name, Funcom and Steam IDs, position. |
| Player location | ✅ | Live with the plugin; last saved position without it. |
| Player inventory | ✅ | Backpack and equipped gear. |
| Give item | ✅ | No relog needed; refused when the backpack is full. |
| Item and entity catalogue | ✅ | In-game names; the full creature list needs the plugin. |
| Locations | ⚠️ | Map regions only, without positions. |
| Console commands | ✅ | Unknown commands return a clear failure. |
| Broadcast message | ⚠️ | Sender shows as `[]` in chat; also an on-screen panel. |
| Whisper | ✅ | |
| Teleport | ✅ | |
| Kick | ✅ | Player can rejoin right away. |
| Ban, unban, ban list | ✅ | Timed and permanent; enforced when the player joins. |
| Shutdown | ⚠️ | Needs `DUNE_SHUTDOWN_CMD`. |
| Join and leave events | ✅ | Includes kicks and crashes. |
| Chat events | ✅ | |
| Death events | ✅ | Environment, fall and NPC causes. |
| Entity kill events | ✅ | Needs the plugin. |
| Log events | ⚠️ | Needs `DUNE_LOG_FILE`; Takaro does not store them. |
| Map | ⚠️ | Needs Takaro protocol 1 and an uploaded map image. |
| Game name, version and icon | ⚠️ | Needs Takaro protocol 1 and `DUNE_LOG_FILE`. |
| Modules (commands, hooks, cronjobs, teleports) | ✅ | |
| Shop and economy | ✅ | Bundles and offline orders included. |
| Discord chat both ways | ✅ | Use the `chatBridgeNoEcho` module. |
| Events during a Takaro outage | ✅ | Queued and delivered after reconnecting. |
| Whisper hidden from other players | ⚠️ | Not tested with two players. |

## Known issues

- Only Hagga Basin is on the Takaro map; Deep Desert and social hubs are not.
- After a game update, plugin features that no longer match degrade; the rest keeps working.
