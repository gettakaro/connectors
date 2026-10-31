# Takaro Dune: Awakening Connector

Connects a self-hosted Dune: Awakening battlegroup (Linux) to Takaro: a required sidecar, plus an
optional map-server plugin for live positions and entity kills. Players do not install anything.

## Install

You need: a self-hosted Dune battlegroup, Docker Compose (or Node.js 22) on a host that can reach
the battlegroup's Postgres and game RabbitMQ, and a Takaro game server of type **Generic** with its
registration token.

### 1. Download

From https://takaro.io/connectors/dune, download for your server build:

- `takaro-dune-sidecar-linux-<build>-<version>.tar.gz` (required)
- `takaro-dune-plugin-linux-<build>-<version>.tar.gz` (optional)

### 2. Install

**Map server.** Add to every map server process:

```text
-ini:engine:[ConsoleVariables]:server.NotificationSystem.Enabled=true
-ini:engine:[FuncomLiveServices]:ServerCommandsAuthToken=<a-long-random-secret>
```

Add to `DuneSandbox/Saved/Config/LinuxServer/UserGame.ini` and restart the map server:

```ini
[AdminSetting.Global]
+Allowed_GM_Commands=KickPlayer
+Allowed_GM_Commands=ServiceBroadcast
```

**Sidecar.** Unpack the archive and rename its folder to `sidecar`. Move
`docker-compose.example.yml` and `.env.example` next to it and rename `.env.example` to `.env`:

```text
dune-connector/
├── docker-compose.example.yml
├── .env
└── sidecar/
```

**Plugin (optional).** Unpack `libtakaro-dune.so` outside the game directory and preload it on the
map process only (never the whole battlegroup):

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dune.so TAKARO_PLUGIN_TOKEN=<shared-secret> \
./DuneSandbox/Binaries/Linux/DuneSandboxServer-Linux-Shipping ...
```

The sidecar must then share the map container's network or run on its host.

### 3. Configure

Set in `.env`:

| Setting | Value |
|---|---|
| `TAKARO_REGISTRATION_TOKEN` | Your registration token. |
| `DUNE_PG_URL` | Read-only Postgres URL, with the build-specific database name. |
| `DUNE_RMQ_URL` | AMQPS URL of the game RabbitMQ (not the admin one). |
| `DUNE_RMQ_TLS_INSECURE` | `true` with Funcom's self-signed certificate. |
| `DUNE_GM_AUTH_TOKEN` | The `ServerCommandsAuthToken` from step 2. |
| `DUNE_GM_PUBLISHER` | `amqp` (needs an `fls` RabbitMQ user), `docker-exec` (set `DUNE_RMQ_CONTAINER`, mount the Docker socket) or `kubectl-exec` (set `DUNE_K8S_NAMESPACE` and `DUNE_K8S_POD`). |
| `DUNE_PLUGIN_URL`, `DUNE_PLUGIN_TOKEN` | Only with the plugin: `http://127.0.0.1:18890` and the `TAKARO_PLUGIN_TOKEN` secret. |

Then start it:

```bash
docker compose -f docker-compose.example.yml --env-file .env up -d --build
```

| Optional setting | What it does |
|---|---|
| `TAKARO_SERVER_NAME` | Server name in Takaro. |
| `TAKARO_SENDER_NAME` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |
| `DUNE_SHUTDOWN_CMD` | Your script that stops the map server; without it Takaro cannot shut down. |
| `DUNE_LOG_FILE` | The map-server log, so Takaro shows the game build. |

For the world map, upload a square Hagga Basin image in the Takaro dashboard: the game's
`T_Survival_WorldMap_MiniMap_D` texture exported from your own client fits exactly
(corners `-457200` and `355600`).

### 4. Check that it works

The sidecar log shows `Identified with Takaro` and the server shows as online in Takaro. Without a
token the log shows a `TAKARO_REGISTRATION_TOKEN not set` banner; paste it into `.env` and save.

**Upgrading:** replace only the `sidecar` folder and start again. Keep `.env` and the `/data`
volume.

## What works, what doesn't

Checked on server build 26081962, 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Online while the sidecar runs; reconnects by itself. |
| Player list | ✅ | |
| Player location | ✅ | Live with the plugin; last saved position without it. |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ✅ | The full list needs the plugin. |
| Locations | ⚠️ | Takaro does not use them for Generic servers. |
| World map | ⚠️ | Hagga Basin only, after you upload the image. Not checked yet. |
| Send a message | ⚠️ | Broadcasts show as `[] Name: message` in chat. |
| Console commands | ✅ | |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent. |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ✅ | |
| Shut down the server | ⚠️ | Needs your own stop script in `DUNE_SHUTDOWN_CMD`. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | |
| Entity killed | ⚠️ | Needs the plugin. |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ✅ | Both ways. |
| Modules | ✅ | |
| Game name, version and icon | ⚠️ | The game build needs `DUNE_LOG_FILE`. Not checked yet. |
| Restarts and outages | ✅ | Events are delivered after reconnecting. |
| Game updates | ✅ | Plugin features a new build breaks switch off; the rest keeps working. |

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
