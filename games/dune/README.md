# Takaro Dune: Awakening Connector

Server-side support for a self-hosted Dune: Awakening battlegroup. Players install nothing. The
sidecar is required; the plugin is optional.

## Install

### Requirements

- A self-hosted Dune battlegroup with a Funcom self-hosting token.
- Docker Compose or Node.js 22 on a host that can reach Postgres and the game RabbitMQ.
- A Takaro game server using the Generic connector and its registration token.
- About 16 GB of free memory for one map server; the CPU must support AVX2.

### Download

Open the latest `dune-vX.Y.Z` release on
<https://github.com/gettakaro/connectors/releases> and download:

- `takaro-dune-sidecar-linux-<build>-<version>.tar.gz` — required.
- `takaro-dune-plugin-linux-<build>-<version>.tar.gz` — optional.

Choose archives whose `<build>` matches your self-hosted server. Check them against
`SHA256SUMS`. Do not download the GitHub-generated “Source code” archives.

### Configure the map server

Add both arguments to every map server process:

```text
-ini:engine:[ConsoleVariables]:server.NotificationSystem.Enabled=true
-ini:engine:[FuncomLiveServices]:ServerCommandsAuthToken=<a-long-random-secret>
```

Add the commands below to
`DuneSandbox/Saved/Config/LinuxServer/UserGame.ini`, then restart the map server:

```ini
[AdminSetting.Global]
+Allowed_GM_Commands=KickPlayer
+Allowed_GM_Commands=ServiceBroadcast
```

### Install the sidecar

Unpack the sidecar archive and rename its folder to `sidecar`. Copy
`docker-compose.example.yml` and `.env.example` beside it, then rename `.env.example` to `.env`:

```text
dune-connector/
├── docker-compose.example.yml
├── .env
└── sidecar/
```

Set these values in `.env`:

| Setting | Value |
|---|---|
| `TAKARO_REGISTRATION_TOKEN` | Registration token from the Takaro game server. |
| `TAKARO_IDENTITY_TOKEN` | A unique name such as `my-dune-server`. |
| `TAKARO_SENDER_NAME` | The same value as Takaro's `serverChatName`. |
| `DUNE_PG_URL` | Read-only Postgres URL, including the build-specific database name. |
| `DUNE_RMQ_URL` | AMQPS URL for the game RabbitMQ, not the admin RabbitMQ. |
| `DUNE_RMQ_TLS_INSECURE` | `true` when using Funcom's self-signed broker certificate. |
| `DUNE_GM_AUTH_TOKEN` | The exact `ServerCommandsAuthToken` configured above. |
| `DUNE_GM_PUBLISHER` | `amqp`, `docker-exec`, or `kubectl-exec` as described below. |

Choose the publisher that matches the battlegroup:

- `amqp`: enable RabbitMQ's `internal` authentication backend and create an `fls` user.
- `docker-exec`: set `DUNE_RMQ_CONTAINER` and mount the Docker socket.
- `kubectl-exec`: set `DUNE_K8S_NAMESPACE` and `DUNE_K8S_POD` for stock Funcom k3s.

Start the sidecar:

```bash
docker compose -f docker-compose.example.yml --env-file .env up -d --build
```

For a plain Node.js installation, run this inside the unpacked sidecar instead:

```bash
npm ci --omit=dev
npm run catalogue
node dist/index.js
```

The installation is ready when the log contains `Identified with Takaro`, the game server is
online in Takaro, and `http://127.0.0.1:18891/health` reports `ok`.

### Optional plugin

The plugin adds live positions, entity kills, and detailed death attribution. Stop the map
server, unpack `libtakaro-dune.so` outside the game directory, and start only the map process with:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dune.so \
TAKARO_PLUGIN_TOKEN=<shared-secret> \
./DuneSandbox/Binaries/Linux/DuneSandboxServer-Linux-Shipping ...
```

Set the same secret as `DUNE_PLUGIN_TOKEN` and set
`DUNE_PLUGIN_URL=http://127.0.0.1:18890` for the sidecar. The sidecar must share the map
container's network namespace or run on its host. Never apply `LD_PRELOAD` to the complete
battlegroup.

### Optional shutdown hook

Dune's shutdown command displays a countdown but does not stop the map process. Set
`DUNE_SHUTDOWN_CMD` to an operator-owned stop script if Takaro should complete shutdowns. Without
the hook, the connector reports shutdown as unavailable.

### Upgrade

Replace the unpacked sidecar and restart it. To upgrade the plugin, stop the map server, replace
the `.so` with one for the same server build, and start it again. Keep the sidecar `/data` volume;
it contains connector-managed bans.

## What works, what doesn't

✅ supported · ⚠️ supported with a caveat · ❌ unavailable

| Feature | Status | Notes |
|---|---:|---|
| Connection and heartbeat | ✅ | Server remains online while the sidecar runs. |
| Player list and lookup | ✅ | Includes names, identifiers, state, and position. |
| Player location | ✅ | Live with plugin; last saved position without it. |
| Player inventory | ✅ | Includes backpack and equipped gear. |
| Give an item | ✅ | Delivered without requiring a relog. |
| Item catalogue | ✅ | Generated during installation with display names. |
| Entity catalogue | ✅ | Full creature list requires the plugin. |
| Locations and points of interest | ⚠️ | Available, but Generic Takaro servers do not request them. |
| Console commands | ✅ | Unknown commands return a clear failure. |
| Broadcast messages | ⚠️ | On-screen panel is named; chat sender appears as `[]`. |
| Private messages | ✅ | Delivered to one player. |
| Teleport player | ✅ | Position is re-read after moving. |
| Kick player | ✅ | Player can reconnect afterward. |
| Timed and permanent bans | ✅ | Connector enforces bans when players join. |
| Unban and ban list | ✅ | Expired bans are excluded automatically. |
| Server shutdown | ⚠️ | Requires an operator-provided stop hook. |
| Player joined and left events | ✅ | Includes quit, kick, and crash departures. |
| Player chat events | ✅ | Connector messages are not echoed back. |
| Player death events | ✅ | Includes environment, fall, and NPC causes. |
| Entity kill events | ✅ | Plugin reports entity and weapon display name. |
| Log events | ⚠️ | Secrets are redacted; at most 50 lines per 30 s are forwarded; Takaro does not retain logs. |
| Map information and tiles | ❌ | Generic Takaro servers do not support them. |
| Module chat commands | ✅ | Commands run and answer in game chat. |
| Module hooks and cronjobs | ✅ | Hooks and scheduled jobs run normally. |
| Module teleports | ✅ | Teleport commands move the player. |
| Welcome messages and starter kits | ✅ | Delivered when the player joins. |
| Shop purchases and claims | ✅ | Supports bundles and offline orders. |
| Economy balances and commands | ✅ | Supports balances, debits, and leaderboards. |
| Game chat to Discord | ✅ | Player chat reaches the linked channel. |
| Discord to game chat | ✅ | Human messages render; bot posts are ignored. |
| Discord hooks and notices | ✅ | Hooks, cronjobs, joins, and leaves are posted. |
| Events during Takaro outages | ✅ | Queued and delivered once after reconnecting. |
| Automatic reconnect | ✅ | Recovers after network, server, or sidecar restarts. |
| Game update degradation | ✅ | Unsupported plugin features degrade without stopping others. |
