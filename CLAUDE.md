# Takaro Game Server Connectors

Monorepo for connector plugins implementing the Takaro Generic Connector Protocol.

## Engineer Skills

- `.claude/skills/takaro-rust-engineer/` — Rust connector
- `.claude/skills/takaro-minecraft-engineer/` — Minecraft connector

## Commands

All operations are in the `justfile`. Run `just --list` to see available commands.

## Connectors

Each connector is self-contained in its own directory with its own docker-compose and build system.

| Connector | Directory | Language | Build |
|-----------|-----------|----------|-------|
| Rust | `rust/` | C# | None (Carbon runtime compile) |
| Minecraft | `minecraft/` | Java 21 | Gradle |

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
```

## Documentation

- https://docs.takaro.io/advanced/generic-connector-protocol
- https://docs.takaro.io/advanced/connection-architecture
- https://docs.takaro.io/advanced/adding-support-for-a-new-game

## Rules

- Always use non-interactive verify: `/verify --mode=report-only --scope=branch`
- Use `docker compose` (not `docker-compose`)
