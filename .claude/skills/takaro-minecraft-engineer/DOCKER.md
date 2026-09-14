# Docker Dev Servers

## dev-servers rig (live)

`dev-servers/` is the **live** rig — use it for anything real. The `minecraft/docker-compose.yml`
environment documented further down is **legacy** (and is the only place the Mineflayer bot runs).

| Service | Platform | Image | Minecraft | Game port | RCON (localhost) | Container |
|---|---|---|---|---|---|---|
| `minecraft-paper` | Paper | `itzg/minecraft-server:java21` | 1.21.11 (`MINECRAFT_VERSION`) | 25565/tcp | 127.0.0.1:25575 | `takaro-dev-minecraft-paper` |
| `minecraft-neoforge` | NeoForge | `itzg/minecraft-server:java21` | 1.21.11 (`MINECRAFT_VERSION`) | 25566/tcp | 127.0.0.1:25576 | `takaro-dev-minecraft-neoforge` |
| `minecraft-fabric` | Fabric | `itzg/minecraft-server:java25` | 26.2 (`MINECRAFT_FABRIC_VERSION`) | 25567/tcp | 127.0.0.1:25577 | `takaro-dev-minecraft-fabric` |

RCON password comes from `RCON_PASSWORD` in `dev-servers/.env` (**not** `takaro123` — that is
the legacy compose only). `rcon-cli` inside the container picks it up automatically:

```bash
docker exec takaro-dev-minecraft-fabric rcon-cli list
docker exec takaro-dev-minecraft-fabric rcon-cli 'kill @e[type=zombie,distance=..20]'
```

Data: `dev-servers/_data/minecraft/<platform>/`
- mods/plugins deploy target, plus `config/takaro.json` (Fabric) — env vars override the file
- logs: `logs/latest.log` (raw WebSocket frames when `TAKARO_DEBUG=true`)

Deploy + restart (Fabric has **no** hot reload):

```bash
dev-servers/scripts/deploy-connector.sh minecraft-fabric   # builds in eclipse-temurin:25-jdk
just dev-stop minecraft-fabric && just dev-start minecraft-fabric
just dev-logs minecraft-fabric -f
```

All three Minecraft platforms build in the JDK 25 image; only the *runtime* image differs.

## Legacy compose (`just minecraft-up`)

Everything below refers to `minecraft/docker-compose.yml`, kept for connector development
and the Mineflayer bot. It is not the rig used for hard tests.

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
