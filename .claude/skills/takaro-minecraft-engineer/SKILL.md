---
name: takaro-minecraft-engineer
description: "Takaro Minecraft connector repository knowledge — architecture, build system, testing, Docker dev servers, test bot, and development workflows. Use when working on this codebase, running tests, debugging, or understanding the project."
---

# Takaro Minecraft Engineer

Multi-platform Minecraft connector for the Takaro game management platform. Implements the Takaro Generic Connector Protocol across Paper, NeoForge, and Fabric using a shared core with platform-specific adapters.

## Quick Reference

| Area | File | Key Command |
|------|------|-------------|
| Architecture | [ARCHITECTURE.md](ARCHITECTURE.md) | — |
| Build & Deploy | [BUILD.md](BUILD.md) | `just minecraft-build` |
| Testing | [TESTING.md](TESTING.md) | `cd minecraft && ./gradlew :core:test` |
| Docker Dev Servers | [DOCKER.md](DOCKER.md) | `just dev-start minecraft-fabric` (live rig); `just minecraft-up -d paper` (legacy) |
| Test Bot | [BOT.md](BOT.md) | `curl http://localhost:3001/status` |
| Integration Testing | [INTEGRATION-TESTING.md](INTEGRATION-TESTING.md) | Takaro MCP tools + bot API |

## Architecture Overview

```
                [Takaro Backend]
                        ↕ (WebSocket)
            [TakaroWebSocketClient]
            (implements EventEmitter)
                        ↕
                [TakaroConnector]
                        ↕
                 [GameAdapter] ← Interface
                        ↑
        ┌───────────────┼───────────────┐
        ↓               ↓               ↓
  [Paper Plugin]  [NeoForge Mod]  [Fabric Mod]
```

**Fabric targets Minecraft 26.2 / Java 25; Paper and NeoForge stay on 1.21.11 / Java 21.**
The whole Gradle build must run on a **JDK 25** Gradle JVM (see [BUILD.md](BUILD.md)).

Core module is pure Java with no Minecraft dependency. Each platform module implements `GameAdapter` and translates platform events to `EventEmitter` calls.

## Project Structure

```
connectors/
├── minecraft/
│   ├── core/           # Pure Java — WebSocket client, config, GameAdapter interface, models
│   ├── paper/          # Paper/Bukkit adapter (Shadow JAR)
│   ├── neoforge/       # NeoForge adapter (ModDevGradle + Shadow)
│   ├── fabric/         # Fabric adapter (Fabric Loom + Shadow)
│   ├── bot/            # Mineflayer test bot with HTTP API
│   ├── net/            # Stub classes
│   ├── scripts/        # deploy.sh, reload.sh
│   └── docker-compose.yml   # LEGACY dev environment (bot lives here)
├── dev-servers/        # THE LIVE RIG — compose/, scripts/, _data/ for every game
├── .env.example        # Shared env config (root level)
├── justfile            # All commands
└── .claude/skills/takaro-minecraft-engineer/
```

## Maintenance

This skill should stay accurate. During work:

- **Discover something useful?** → Ask the human if it should be added
- **Find outdated info?** → Ask the human if it should be updated
- **Find significant changes?** → Update the relevant skill `.md` file directly
