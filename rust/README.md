# Takaro Rust Connector

Carbon plugin that implements the [Takaro Generic Connector Protocol](https://docs.takaro.io/advanced/adding-support-for-a-new-game) for Rust game servers.

## Architecture

The plugin runs inside the Rust server via the [Carbon](https://carbonmod.gg/) mod framework and connects outbound to Takaro via WebSocket. No port forwarding required.

```
[Takaro Backend]  <->  [TakaroConnector.cs]  <->  [Rust Server + Carbon]
                 WebSocket (outbound)         Direct C# game API access
```

## Features

- All 17 Takaro actions: player management, inventory, teleport, items, bans, commands
- 6 game events: player connect/disconnect, chat, death, entity killed, log
- WebSocket with automatic reconnection and exponential backoff
- Hot-reload support via Carbon (no server restart needed)

## Setup

### Prerequisites

- Docker
- Node.js v22+ (for the RCON reload script)
- A Takaro account with a registration token ([get one from your Takaro dashboard](https://docs.takaro.io/advanced/adding-support-for-a-new-game))

### Quick Start

```bash
# Configure (from repo root)
cp ../.env.example ../.env
# Edit ../.env: set TAKARO_WS_URL and TAKARO_REGISTRATION_TOKEN

# Build and start Rust server (first build downloads ~6GB)
docker compose up -d rust

# Wait for server to finish booting (first boot takes several minutes)
# Check with: docker compose logs -f rust | grep "Server startup complete"

# Deploy plugin
./scripts/deploy.sh

# Hot-reload
./scripts/reload.sh
```

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `TAKARO_WS_URL` | Takaro WebSocket endpoint | `wss://connect.takaro.io/` |
| `TAKARO_REGISTRATION_TOKEN` | Server registration token | (required) |
| `TAKARO_IDENTITY_TOKEN` | Unique server identity | `takaro-rust-dev` |
| `TAKARO_DEBUG` | Enable debug logging | `false` |
| `RCON_PASSWORD` | RCON password (dev only) | `takaro123` |

## Development

```bash
# Edit the plugin
vim plugin/TakaroConnector.cs

# Deploy and reload (no build step — Carbon compiles .cs at runtime)
./scripts/deploy.sh
./scripts/reload.sh  # requires Node.js v22+

# View logs
just rust-logs
```
