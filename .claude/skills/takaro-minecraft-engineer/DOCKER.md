# Docker Dev Servers

## Services

| Service | Platform | Game Port | RCON Port | Container |
|---------|----------|-----------|-----------|-----------|
| paper | Paper | 25565 | 25575 | minecraft-paper |
| neoforge | NeoForge | 25566 | 25576 | minecraft-neoforge |
| fabric | Fabric | 25567 | 25577 | minecraft-fabric |
| bot | Mineflayer | 3001 (API) | — | minecraft-bot |

## Starting Servers

```bash
just minecraft-up -d paper           # Single platform
just minecraft-up -d                 # All services including bot
```

## Configuration

Requires `.env` file (at repo root) with Takaro credentials:

```bash
cp .env.example .env
# Fill in TAKARO_WS_URL, TAKARO_REGISTRATION_TOKEN
```

Each server gets a hardcoded `TAKARO_IDENTITY_TOKEN` (e.g., `takaro-paper-dev`).

### Debug Logging

Set `TAKARO_DEBUG=true` in `.env` to enable debug logging in all services.

## RCON Access

Password: `takaro123`

```bash
cd minecraft && docker compose exec paper rcon-cli          # Interactive RCON
cd minecraft && docker compose exec paper rcon-cli list     # Run single command
```

## Common Operations

```bash
just minecraft-logs --tail=50 paper         # View logs
cd minecraft && docker compose restart paper # Restart after deploy
cd minecraft && docker compose ps --format json  # Check running containers
```

## Data Directories

Server data lives in `minecraft/_data/<platform>/`:
- `minecraft/_data/paper/plugins/` — Paper plugins (deploy target)
- `minecraft/_data/neoforge/mods/` — NeoForge mods (deploy target)
- `minecraft/_data/fabric/mods/` — Fabric mods (deploy target)

Config files per platform:
- Paper: `minecraft/_data/paper/plugins/TakaroMinecraft/config.yml`
- NeoForge: `minecraft/_data/neoforge/config/takaro.properties`
- Fabric: `minecraft/_data/fabric/config/takaro.json`
