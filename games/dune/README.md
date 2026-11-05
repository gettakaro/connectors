# Takaro Dune: Awakening Connector

Connects a self-hosted Dune: Awakening battlegroup (Linux) to Takaro. A sidecar runs next to the
battlegroup and is required; a native plugin for the map server is optional and adds live
positions, entity kills and detailed death causes. Players do not install anything.

## Install

### 1. Before you start

- A self-hosted Dune battlegroup with a Funcom self-hosting token.
- Docker Compose or Node.js 22 on a host that can reach the battlegroup's Postgres and the game
  RabbitMQ.
- About 16 GB of free memory for one map server, on a CPU with AVX2.
- A Takaro game server of type **Generic** and its registration token.

### 2. Download

Download the latest release from https://takaro.io/connectors/dune (or the latest `dune-v<version>`
release on https://github.com/gettakaro/connectors/releases):

- `takaro-dune-sidecar-linux-<build>-<version>.tar.gz`: required.
- `takaro-dune-plugin-linux-<build>-<version>.tar.gz`: optional.

Pick the archives whose `<build>` matches your self-hosted server. Check them against `SHA256SUMS`.
Do not download the "Source code" archives.

### 3. Install

**Map server.** Add both arguments to every map server process:

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

**Sidecar.** Unpack the sidecar archive and rename its folder to `sidecar`. Move
`docker-compose.example.yml` and `.env.example` out of it to beside it, and rename `.env.example`
to `.env`. Keep `.env` outside `sidecar/`: an upgrade replaces that folder.

```text
dune-connector/
├── docker-compose.example.yml
├── .env
└── sidecar/
```

**Plugin (optional).** Stop the map server, unpack `libtakaro-dune.so` outside the game directory
(for example `/opt/takaro/`), and start only the map process with it:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dune.so \
TAKARO_PLUGIN_TOKEN=<shared-secret> \
./DuneSandbox/Binaries/Linux/DuneSandboxServer-Linux-Shipping ...
```

Never apply `LD_PRELOAD` to the whole battlegroup. The sidecar must share the map container's
network namespace or run on its host.

### 4. Configure

Set these values in `.env`:

| Setting | Value |
|---|---|
| `TAKARO_REGISTRATION_TOKEN` | Registration token from the Takaro game server. Can be pasted later. |
| `TAKARO_IDENTITY_TOKEN` | Leave empty. The sidecar generates one and keeps it in `/data`. |
| `TAKARO_SERVER_NAME` | Optional. Must be unique in Takaro; empty uses `Dune (<identity>)`. |
| `TAKARO_SENDER_NAME` | Optional chat sender name (alias `TAKARO_SERVER_CHAT_NAME`). The chat name set in Takaro wins when Takaro sends it; set the same value for a Takaro that does not. |
| `DUNE_PG_URL` | Read-only Postgres URL, including the build-specific database name. |
| `DUNE_RMQ_URL` | AMQPS URL for the game RabbitMQ, not the admin RabbitMQ. |
| `DUNE_RMQ_TLS_INSECURE` | `true` when using Funcom's self-signed broker certificate. |
| `DUNE_GM_AUTH_TOKEN` | The exact `ServerCommandsAuthToken` from step 3. |
| `DUNE_GM_PUBLISHER` | `amqp`, `docker-exec` or `kubectl-exec`, see below. |
| `DUNE_PLUGIN_URL` | Only with the plugin: `http://127.0.0.1:18890`. |
| `DUNE_PLUGIN_TOKEN` | Only with the plugin: the same secret as `TAKARO_PLUGIN_TOKEN`. |
| `DUNE_SHUTDOWN_CMD` | Optional: your own script that stops the map server, see below. |
| `DUNE_LOG_FILE` | Optional: the map-server log, so Takaro can show the game build. |
| `TAKARO_PROTOCOL` | Optional: `0` turns the world map off for a Takaro without map support. Needs a restart. |

Choose the publisher that matches the battlegroup:

- `amqp`: enable RabbitMQ's `internal` authentication backend and create an `fls` user.
- `docker-exec`: set `DUNE_RMQ_CONTAINER` and mount the Docker socket.
- `kubectl-exec`: set `DUNE_K8S_NAMESPACE` and `DUNE_K8S_POD` for stock Funcom k3s.

Dune's shutdown command shows a countdown but does not stop the map process. Set
`DUNE_SHUTDOWN_CMD` if Takaro should complete shutdowns; without it, shutdown is reported as
unavailable.

Start the sidecar:

```bash
docker compose -f docker-compose.example.yml --env-file .env up -d --build
```

For plain Node.js, run this inside the unpacked sidecar instead, with the `DUNE_*` values in the
environment (the Takaro values are also read from `../.env`):

```bash
npm ci --omit=dev
npm run catalogue
node dist/index.js
```

Saving `.env` applies `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL` and
`TAKARO_SERVER_NAME` within a few seconds, no restart needed. Other changes need
`docker compose ... up -d` again (or a restart of `node dist/index.js`).

**World map.** The connector tells Takaro the size of the Hagga Basin map, but the server has no
map image, so you upload one: open the game server's map in the Takaro dashboard and upload an
image for **Hagga Basin**. It must be square, with the north-west corner (`x = -457200`,
`y = -457200`) top-left and the south-east corner (`x = 355600`, `y = 355600`) bottom-right. The
game's minimap texture `T_Survival_WorldMap_MiniMap_D` (4096 x 4096) has exactly this frame: export
it as PNG from your own game client (`DuneSandbox/Content/Paks`, for example with FModel) and upload
it unchanged, or crop an in-game map screenshot to the same square. Do not share the exported image:
it is Funcom's artwork.

### 5. Check that it works

The sidecar log shows `Identified with Takaro`, `http://127.0.0.1:18891/health` reports `ok`, and
the game server shows as online in Takaro.

Without a registration token the log shows a `TAKARO_REGISTRATION_TOKEN not set` banner and the
sidecar stays offline. Paste the token into `.env` and save; it connects within a few seconds.

### 6. Upgrade

Unpack the new sidecar archive somewhere else, replace only the `sidecar` folder with it, and run
the start command again. Keep `.env` and the sidecar's `/data` volume: it holds the server's
identity and the connector's bans. To upgrade the plugin, stop the map server, replace the `.so`
with one for the same server build, and start it again.

## What works, what doesn't

Checked on server build 26081962, 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Online while the sidecar runs. |
| Player list | ✅ | Names, identifiers, state and position. |
| Player location | ✅ | Live with the plugin; the last saved position without it. |
| Teleport | ✅ | The position is re-read after moving. |
| Player inventory | ✅ | Backpack and equipped gear. |
| Item list | ✅ | Built during installation, with display names. |
| Entity list | ✅ | The full creature list needs the plugin. |
| Locations | ⚠️ | Available, but Takaro does not ask Generic servers for them. |
| World map | ⚠️ | Hagga Basin only, and you upload the image yourself. Players on other maps (Deep Desert, social hubs) do not show. Not checked on takaro.io yet. |
| Send a message | ⚠️ | Private messages work. Broadcasts show in the on-screen panel titled with the sender name; in chat as `[] Name: message`, because the game has no sender field. |
| Console commands | ✅ | Unknown commands return a clear failure. |
| Kick | ✅ | The player can reconnect afterwards. |
| Ban and unban | ✅ | Timed and permanent; the connector enforces bans when players join. |
| Ban list | ✅ | Expired bans are left out. |
| Give an item | ✅ | Arrives without a relog. |
| Shop | ✅ | Bundles and offline orders work; balances, debits and leaderboards. |
| Shut down the server | ⚠️ | Needs your own stop script in `DUNE_SHUTDOWN_CMD`. |
| Player joined / left | ✅ | Includes quits, kicks and crashes. |
| Chat messages | ✅ | Connector messages are not echoed back. |
| Player death | ✅ | Environment, fall and NPC causes. |
| Entity killed | ⚠️ | Needs the plugin; reports the entity and the weapon's display name. |
| Server log lines | ⚠️ | Secrets are redacted and at most 50 lines per 30 s are sent; Takaro does not keep them. |
| Discord chat bridge | ✅ | Both ways; bot posts are ignored. Hooks, cronjobs, joins and leaves post to Discord. |
| Modules | ✅ | Chat commands, hooks, cronjobs, teleports and welcome messages. |
| Game name, version and icon | ⚠️ | Takaro shows Dune: Awakening with its icon; the game build needs `DUNE_LOG_FILE`. Not checked on takaro.io yet. |
| Restarts and outages | ✅ | Events are queued and delivered once after reconnecting; reconnects by itself after network, server or sidecar restarts. |
| Game updates | ✅ | Plugin features a new build breaks switch off; the rest keeps working. |

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
