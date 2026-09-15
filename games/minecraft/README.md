# Takaro Minecraft Integration

Multi-platform Minecraft connector for the [Takaro](https://takaro.io) game management platform. Supports **Paper**, **NeoForge**, and **Fabric**.

## Requirements

| Platform | Minecraft | Java (runtime) |
|----------|-----------|----------------|
| Fabric | 26.2 | Java 25 |
| Paper | 1.21.11 | Java 21 |
| NeoForge | 1.21.11 | Java 21 |

- **Building requires JDK 25** for the whole multi-project build (Fabric Loom 1.17.20 needs
  Gradle >= 9.5 on a JDK 25 Gradle JVM, and the fabric project is evaluated even when you
  build only `:paper:build`). Paper, NeoForge and core are still compiled with `--release 21`,
  so their class files stay Java 21.
- Docker and Docker Compose (for running servers)

### Compatibility

A client's Minecraft version must match the server's. The Fabric connector runs on
Minecraft 26.2, so players need **26.2 clients** — 26.x clients cannot join 1.21.11 servers,
and 1.21.11 clients cannot join 26.x servers. Paper and NeoForge stay on 1.21.11 clients.

## Feature status

Hard-tested end to end on 2026-09-14 with a real Minecraft 26.2 client (Fabric, connector 0.0.3 + fixes on this branch). ✅ works, ⚠️ partial or with a caveat, ❌ does not work. Paper and NeoForge have not been re-tested at this level.

| Feature | Fabric (26.2) | Notes |
|---|---|---|
| Server shows as online in Takaro | ✅ | Reconnects by itself after a server restart |
| Player list, player info, position | ✅ | |
| Player joins / leaves | ✅ | Leaves are also reported when the server itself shuts down (fixed in this branch) |
| Chat messages to Takaro | ✅ | Global chat |
| Takaro commands in chat (e.g. `@ping`, `@tp`, `@shop`) | ✅ | Prefix is your domain's command prefix |
| Messages from Takaro to players | ✅ | Broadcast and whisper |
| Console commands from Takaro | ✅ | Command output is not returned, only success/failure |
| Give item | ✅ | Shop deliveries work (a null-quality bug was fixed in this branch) |
| Teleport | ⚠️ | Works, but the target Y is snapped to the ground |
| Kick | ✅ | |
| Ban / unban / ban list | ✅ | Permanent and timed bans |
| Player death events | ✅ | Killer is included when it is a player |
| Entity killed events | ✅ | Includes the entity type and the weapon |
| Item and entity lists (shop catalogue) | ✅ | Synced via the Takaro sync jobs |
| Player inventory | ✅ | |
| Shop and economy (buy in chat or via Takaro) | ✅ | |
| Discord chat bridge (game → Discord) | ✅ | |
| Discord chat bridge (Discord → game) | ⚠️ | Not yet confirmed with a human Discord post |
| Modules: hooks, cron jobs, teleports, onboarding | ✅ | |
| Server shutdown from Takaro | ✅ | The server process exits; your host must restart it |
| Locations list | ❌ | Not implemented (Minecraft has no fixed location list) |
| Item quality / durability in give item | ❌ | Not supported by the connector |

## Quick Start

### Build

```bash
(cd mod && ./gradlew build)
```

This produces 3 JARs:
- `paper/build/libs/takaro-paper-<version>.jar` — Paper/Spigot plugin
- `neoforge/build/libs/takaro-neoforge-<version>.jar` — NeoForge mod
- `fabric/build/libs/takaro-fabric-<version>.jar` — Fabric mod

### Run servers with Docker Compose

> **Legacy.** The `games/minecraft/docker-compose.yml` environment below (including the Mineflayer
> bot) is kept for connector development only. The live rig is `dev-servers/` — see
> [dev-servers/README.md](../dev-servers/README.md) — which runs Fabric on 26.2 / Java 25 and
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

### Deploy

```bash
# Build and deploy to Paper
just minecraft-build
just minecraft-deploy paper

# Repeat for other platforms as needed (neoforge, fabric)
```

### Configure

#### Environment variables (recommended for Docker)

Environment variables override file-based config when set:

| Variable | Description |
|----------|-------------|
| `TAKARO_WS_URL` | WebSocket URL for your Takaro instance |
| `TAKARO_IDENTITY_TOKEN` | Unique identity token for this server |
| `TAKARO_REGISTRATION_TOKEN` | Registration token from the Takaro dashboard |
| `TAKARO_DEBUG` | Enable debug logging (`true` or `1`) — shows raw WebSocket messages |

In Docker Compose, these are passed to containers automatically from your `.env` file. Each container gets a hardcoded `TAKARO_IDENTITY_TOKEN` (e.g. `takaro-paper-dev`).

#### Config files

Each platform has its own config format:

**Paper** (`_data/paper/plugins/TakaroMinecraft/config.yml`):
```yaml
takaro:
  websocket:
    url: "wss://connect.takaro.io/"
  authentication:
    identity_token: "your-identity-token"
    registration_token: "your-registration-token"
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

## Project Structure

```
games/minecraft/
├── mod/            # Gradle project for the in-game component
│   ├── core/       # Shared logic (WebSocket client, config, protocol)
│   ├── paper/      # Paper/Spigot adapter
│   ├── neoforge/   # NeoForge adapter
│   └── fabric/     # Fabric adapter
├── bot/            # Mineflayer test bot
├── scripts/        # Deploy, reload scripts
└── docker-compose.yml
```

## Development

```bash
# Build everything
(cd mod && ./gradlew build)

# Build a specific module
(cd mod && ./gradlew :paper:build)

# Deploy and reload (Paper only)
just minecraft-deploy paper
just minecraft-reload paper
```

> **Note:** `just minecraft-reload` (which calls `scripts/reload.sh`) only works for **Paper**. For NeoForge and Fabric, use `docker compose restart neoforge` or `docker compose restart fabric` instead.

## Takaro Integration

The connector implements the [Takaro game connector protocol](https://docs.takaro.io/advanced/generic-connector-protocol). See the [adding a new game guide](https://docs.takaro.io/advanced/adding-support-for-a-new-game) for protocol details.

## License

This project is part of the Takaro platform ecosystem.
