# Takaro Minecraft Integration

Multi-platform Minecraft connector for the [Takaro](https://takaro.io) game management platform. Supports **Paper**, **NeoForge**, and **Fabric**.

## Requirements

- Java 21
- Docker and Docker Compose (for running servers)

## Quick Start

### Build

```bash
./gradlew build
```

This produces 3 JARs:
- `paper/build/libs/takaro-paper-<version>.jar` — Paper/Spigot plugin
- `neoforge/build/libs/takaro-neoforge-<version>.jar` — NeoForge mod
- `fabric/build/libs/takaro-fabric-<version>.jar` — Fabric mod

### Run servers with Docker Compose

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
minecraft/
├── core/           # Shared logic (WebSocket client, config, protocol)
├── paper/          # Paper/Spigot adapter
├── neoforge/       # NeoForge adapter
├── fabric/         # Fabric adapter
├── scripts/        # Deploy, reload scripts
└── docker-compose.yml
```

## Development

```bash
# Build everything
./gradlew build

# Build a specific module
./gradlew :paper:build

# Deploy and reload (Paper only)
just minecraft-deploy paper
just minecraft-reload paper
```

> **Note:** `just minecraft-reload` (which calls `scripts/reload.sh`) only works for **Paper**. For NeoForge and Fabric, use `docker compose restart neoforge` or `docker compose restart fabric` instead.

## Takaro Integration

The connector implements the [Takaro game connector protocol](https://docs.takaro.io/advanced/generic-connector-protocol). See the [adding a new game guide](https://docs.takaro.io/advanced/adding-support-for-a-new-game) for protocol details.

## License

This project is part of the Takaro platform ecosystem.
