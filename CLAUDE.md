# Takaro Game Server Connectors

Monorepo for connector plugins implementing the Takaro Generic Connector Protocol.

## Engineer Skills

- `.claude/skills/takaro-rust-engineer/` — Rust connector
- `.claude/skills/takaro-minecraft-engineer/` — Minecraft connector
- `.claude/skills/takaro-7d2d-engineer/` — 7D2D connector
- `.claude/skills/takaro-zomboid-engineer/` — Project Zomboid connector

## Commands

All operations are in the `justfile`. Run `just --list` to see available commands.

## Connectors

Each connector is self-contained in its own directory with its own docker-compose and build system.

| Connector | Directory | Language | Build |
|-----------|-----------|----------|-------|
| Rust | `rust/` | C# | None (Carbon runtime compile) |
| Minecraft | `minecraft/` | Java 21 | Gradle |
| 7D2D | `7d2d/` | C# / .NET Framework 4.8 | Dockerized Mono `msbuild` |
| Project Zomboid | `zomboid/` | Java 25 (`-javaagent`, ByteBuddy) | Gradle |

## Test Environment

`dev-servers/` runs a server for every connector (plus Palworld via a third-party bridge)
with the real mod installed. One Compose file per game, own ports, own `_data/`.
See `dev-servers/README.md`.

```bash
just dev-install-all          # sequential install; games are left stopped
just dev-start <game>         # rust, minecraft-paper, 7d2d, zomboid, valheim, terraria, conan-exiles, palworld, ...
just dev-status               # installed / running / RAM / disk
just dev-validate             # docker compose config + port-collision check
```

## Working on a Connector

```bash
# One-time setup
cp .env.example .env          # Fill in credentials

# Rust
just rust-up -d               # Start dev server
just rust-deploy              # Deploy plugin
just rust-reload              # Hot-reload

# Minecraft
just minecraft-build          # Build all modules
just minecraft-up -d paper    # Start Paper server
just minecraft-deploy paper   # Deploy JAR
just minecraft-reload paper   # Reload plugin (Paper only — NeoForge/Fabric require container restart)

# 7D2D
just sevend2d-setup           # Download server files and build dependencies
just sevend2d-build           # Build the mod
just sevend2d-build-deploy    # Build and deploy to the local test server
just sevend2d-up -d 7dtdserver
just sevend2d-logs

# Project Zomboid
just zomboid-setup            # Stage the PZ server jar for compilation
just zomboid-build            # Build (unit tests + shaded -javaagent jar)
just zomboid-deploy           # Build + deploy the agent into dev-servers/_data
just zomboid-up
just zomboid-logs
```

## Documentation

- https://docs.takaro.io/advanced/generic-connector-protocol
- https://docs.takaro.io/advanced/connection-architecture
- https://docs.takaro.io/advanced/adding-support-for-a-new-game

## Rules

- Always use non-interactive verify: `/verify --mode=report-only --scope=branch`
- Use `docker compose` (not `docker-compose`)
- `7d2d/` does not use the shared root `.env`; its runtime config is generated in `7d2d/Config.xml`
- PR titles MUST follow Conventional Commits — the `pr-title` check (`scripts/check-commit-title.sh`) enforces it and fails otherwise. Allowed types: `feat, fix, docs, style, refactor, perf, test, build, ci, chore, revert` (append `!` for breaking). Example: `ci: open release PRs for every commit type`. Validate locally with `bash scripts/check-commit-title.sh "<title>"`.
