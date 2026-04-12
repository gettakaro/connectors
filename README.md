# Takaro Game Server Connectors

Monorepo for connector plugins that implement the [Takaro Generic Connector Protocol](https://docs.takaro.io/advanced/generic-connector-protocol) for different game servers.

## Connectors

| Connector | Directory | Language | Build |
|-----------|-----------|----------|-------|
| Rust | [`rust/`](rust/) | C# | None (Carbon runtime compile) |
| Minecraft | [`minecraft/`](minecraft/) | Java 21 | Gradle |

Each connector is self-contained with its own Docker dev environment, build system, and scripts. See each connector's README for details.

## Setup

```bash
cp .env.example .env    # Fill in TAKARO_REGISTRATION_TOKEN at minimum
```

## Commands

All operations are in the `justfile`. Run `just --list` to see available commands.

## Releasing

Connectors are versioned and released independently:

```bash
just release-rust 1.0.1        # Tags rust-v1.0.1 and pushes
just release-minecraft 1.2.3   # Tags minecraft-v1.2.3 and pushes
```

Pushing to `main` creates pre-releases automatically (per-connector, path-filtered).

## Documentation

- [Generic Connector Protocol](https://docs.takaro.io/advanced/generic-connector-protocol)
- [Connection Architecture](https://docs.takaro.io/advanced/connection-architecture)
- [Adding Support for a New Game](https://docs.takaro.io/advanced/adding-support-for-a-new-game)
