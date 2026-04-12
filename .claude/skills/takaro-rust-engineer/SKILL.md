---
name: takaro-rust-engineer
description: "Takaro Rust connector knowledge — Carbon plugin architecture, Docker dev server, deploy/reload workflow, and development patterns. Use when working on the Rust connector."
---

# Takaro Rust Engineer

Carbon plugin that implements the Takaro Generic Connector Protocol for Rust game servers. Single `.cs` file — no build step.

## Quick Reference

| Area | File | Key Command |
|------|------|-------------|
| Architecture | [ARCHITECTURE.md](ARCHITECTURE.md) | — |
| Docker Dev Server | [DOCKER.md](DOCKER.md) | `just rust-up -d` |
| Testing | [TESTING.md](TESTING.md) | `just rust-deploy && just rust-reload` |

## Architecture Overview

```
[Takaro Backend]
       ↕ (WebSocket - outbound from plugin)
[TakaroConnector.cs]  ← Single Carbon plugin
       ↕ (Direct C# game API access)
[Rust Dedicated Server + Carbon]
```

## Project Structure

```
connectors/
├── rust/
│   ├── plugin/
│   │   └── TakaroConnector.cs       # The Carbon plugin (single file)
│   ├── scripts/
│   │   ├── deploy.sh                # Copy plugin to server
│   │   └── reload.sh                # Hot-reload via RCON (requires Node.js v22+)
│   ├── Dockerfile                   # Custom Ubuntu 22.04 + SteamCMD + Rust + Carbon
│   ├── start.sh                     # Container entrypoint (sources Carbon env)
│   ├── docker-compose.yml
│   ├── README.md
│   └── _data/                       # Runtime server data (gitignored)
│       ├── plugins/                 # Carbon plugins volume
│       ├── server/                  # World saves volume
│       └── carbon-logs/             # Carbon logs volume
├── .env.example                     # Shared env config (root level)
├── justfile                         # All commands
└── .claude/skills/takaro-rust-engineer/
```

## Dev Workflow

1. Edit `rust/plugin/TakaroConnector.cs`
2. `just rust-deploy` — copies to server
3. `just rust-reload` — hot-reloads plugin via RCON (no server restart)
4. `just rust-logs` — check logs
