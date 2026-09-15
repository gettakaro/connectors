# Takaro Minecraft Connector — Development

Notes for working on the connector itself. Server operators only need [`README.md`](README.md).

## Platform / toolchain matrix

| Platform | Minecraft | Java (runtime) |
|----------|-----------|----------------|
| Fabric | 26.2 | Java 25 |
| Paper | 1.21.11 | Java 21 |
| NeoForge | 1.21.11 | Java 21 |

- **Building requires JDK 25** for the whole multi-project build (Fabric Loom 1.17.20 needs
  Gradle >= 9.5 on a JDK 25 Gradle JVM, and the fabric project is evaluated even when you
  build only `:paper:build`). Paper, NeoForge and core are still compiled with `--release 21`,
  so their class files stay Java 21.
- Docker and Docker Compose (for running servers).

### Client compatibility

A client's Minecraft version must match the server's. The Fabric connector runs on
Minecraft 26.2, so players need **26.2 clients** — 26.x clients cannot join 1.21.11 servers,
and 1.21.11 clients cannot join 26.x servers. Paper and NeoForge stay on 1.21.11 clients.

## Build

```bash
(cd mod && ./gradlew build)
```

This produces 3 JARs:

- `paper/build/libs/takaro-paper-<version>.jar` — Paper/Spigot plugin
- `neoforge/build/libs/takaro-neoforge-<version>.jar` — NeoForge mod
- `fabric/build/libs/takaro-fabric-<version>.jar` — Fabric mod

Build a specific module:

```bash
(cd mod && ./gradlew :paper:build)
```

Release artifacts are collected by `scripts/build-release.sh <version> <out-dir>`, which is what
`.github/workflows/minecraft.yml` runs in CI before publishing the jars to the
`minecraft-v<version>` release.

## Run servers with Docker Compose

> **Legacy.** The `games/minecraft/docker-compose.yml` environment below (including the Mineflayer
> bot) is kept for connector development only. The live rig is `dev-servers/` — see
> [dev-servers/README.md](../../dev-servers/README.md) — which runs Fabric on 26.2 / Java 25 and
> Paper/NeoForge on 1.21.11 / Java 21.

```bash
# Copy and fill in your Takaro credentials (from repo root)
cp ../.env.example ../.env

# Start all 3 servers
docker compose up -d

# Or start a specific platform
docker compose up -d paper
```

| Service | Platform | Game Port | RCON Port |
|---------|----------|-----------|-----------|
| paper | Paper | 25565 | 25575 |
| neoforge | NeoForge | 25566 | 25576 |
| fabric | Fabric | 25567 | 25577 |

RCON password: `takaro123`

## Deploy and reload

```bash
# Build and deploy to Paper
just minecraft-build
just minecraft-deploy paper

# Repeat for other platforms as needed (neoforge, fabric)

just minecraft-reload paper
```

> **Note:** `just minecraft-reload` (which calls `scripts/reload.sh`) only works for **Paper**. For
> NeoForge and Fabric, use `docker compose restart neoforge` or `docker compose restart fabric`
> instead.

## Configuration

### Environment variables (recommended for Docker)

Environment variables override file-based config when set:

| Variable | Description |
|----------|-------------|
| `TAKARO_WS_URL` | WebSocket URL for your Takaro instance |
| `TAKARO_IDENTITY_TOKEN` | Unique identity token for this server |
| `TAKARO_REGISTRATION_TOKEN` | Registration token from the Takaro dashboard |
| `TAKARO_DEBUG` | Enable debug logging (`true` or `1`) — shows raw WebSocket messages |

In Docker Compose, these are passed to containers automatically from your `.env` file. Each
container gets a hardcoded `TAKARO_IDENTITY_TOKEN` (e.g. `takaro-paper-dev`).

### Config files

Each platform has its own config format and location. In the Docker Compose rig these live under
`_data/<platform>/`; on a real server they are relative to the server directory (see the README).

**Paper** (`_data/paper/plugins/TakaroMinecraft/config.yml`):

```yaml
takaro:
  websocket:
    url: "wss://connect.takaro.io/"
  authentication:
    identity_token: "your-identity-token"
    registration_token: "your-registration-token"
  reconnect:
    enabled: true
    delay: 5000
    max_delay: 300000
    backoff_multiplier: 1.5
  debug: false
```

**NeoForge** (`_data/neoforge/config/takaro.properties`):

```properties
takaro.websocket.url=wss://connect.takaro.io/
takaro.authentication.identity_token=your-identity-token
takaro.authentication.registration_token=your-registration-token
takaro.debug=false
```

**Fabric** (`_data/fabric/config/takaro.json`):

```json
{
  "websocket": { "url": "wss://connect.takaro.io/" },
  "authentication": {
    "identity_token": "your-identity-token",
    "registration_token": "your-registration-token"
  },
  "settings": { "debug": false }
}
```

## Project structure

```
games/minecraft/
├── mod/            # Gradle project for the in-game component
│   ├── core/       # Shared logic (WebSocket client, config, protocol)
│   ├── paper/      # Paper/Spigot adapter
│   ├── neoforge/   # NeoForge adapter
│   └── fabric/     # Fabric adapter
├── bot/            # Mineflayer test bot
├── scripts/        # Build-release, deploy, reload scripts
└── docker-compose.yml
```

`core` owns `TakaroConnector`, `TakaroWebSocketClient`, `TakaroConfig` and the `GameAdapter` /
`EventEmitter` interfaces; each platform module implements `GameAdapter` (for example
`FabricGameAdapter`) and wires the lifecycle hooks.

## Takaro integration

The connector implements the
[Takaro game connector protocol](https://docs.takaro.io/advanced/generic-connector-protocol). See
the [adding a new game guide](https://docs.takaro.io/advanced/adding-support-for-a-new-game) for
protocol details.

## License

This project is part of the Takaro platform ecosystem.
