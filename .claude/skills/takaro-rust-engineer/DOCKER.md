# Docker Dev Server

## Service

| Service | Platform | Game Port | RCON Port | Container |
|---------|----------|-----------|-----------|-----------|
| rust | Rust + Carbon | 28015/udp | 28016 | rust-carbon |

## First-Time Setup

```bash
cp .env.example .env
# Fill in TAKARO_WS_URL, TAKARO_REGISTRATION_TOKEN

# Build and start server (first build downloads Rust ~6GB)
cd rust && docker compose up -d rust
```

The custom Dockerfile (Ubuntu 22.04) installs SteamCMD, Rust server, and Carbon during the image build. No separate Carbon install step needed.

## Configuration

Requires `.env` file (at repo root) with Takaro credentials:

```bash
TAKARO_WS_URL=wss://connect.takaro.io/
TAKARO_REGISTRATION_TOKEN=your-token-here
TAKARO_IDENTITY_TOKEN=takaro-rust-dev
TAKARO_DEBUG=false
RCON_PASSWORD=takaro123
```

### Debug Logging

Set `TAKARO_DEBUG=true` in `.env` to see raw WebSocket messages in logs.

## RCON Access

Rust uses WebSocket RCON (not traditional TCP RCON). Port: 28016, Password: `takaro123`

```bash
# Reload plugin
just rust-reload

# Send custom RCON command
cd rust && ./scripts/reload.sh "status"
```

## Common Operations

```bash
just rust-logs                        # View logs (tail 100)
just rust-down && just rust-up -d     # Restart server
just rust-down                        # Stop everything
```

## Data Directory

Server data is bind-mounted as separate volumes:
- `rust/_data/plugins/` — Carbon plugins (deploy target, maps to `/rust/carbon/plugins/`)
- `rust/_data/carbon-logs/` — Carbon logs (maps to `/rust/carbon/logs/`)
- `rust/_data/server/` — World saves (maps to `/rust/server/`)

## Carbon Plugin Hot-Reload

Carbon supports reloading plugins without restarting the server:

```bash
just rust-deploy    # Copy plugin file
just rust-reload    # RCON: c.reload TakaroConnector
```
