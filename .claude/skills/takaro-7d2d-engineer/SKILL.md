---
name: takaro-7d2d-engineer
description: "Takaro 7D2D connector knowledge — Mono/MSBuild mod build, Docker dev services, decompiler-first workflow, and deployment/testing patterns."
---

# Takaro 7D2D Engineer

7 Days to Die mod that implements the Takaro Generic Connector Protocol for dedicated servers.

## Quick Reference

| Area | File | Key Command |
|------|------|-------------|
| Build and setup | `7d2d/scripts/` | `just sevend2d-setup` |
| Docker dev services | `7d2d/docker-compose.yml` | `just sevend2d-up -d 7dtdserver` |
| Build and deploy | `7d2d/scripts/build-mod.sh` | `just sevend2d-build-deploy` |

## Project Structure

```
connectors/
├── 7d2d/
│   ├── src/                 # Mod source
│   ├── scripts/             # setup-environment.sh, build-mod.sh
│   ├── Dockerfile.builder   # Mono/MSBuild builder image
│   ├── docker-compose.yml   # steamcmd, builder, deps, 7dtdserver
│   ├── Takaro.csproj
│   ├── Takaro.sln
│   ├── ModInfo.xml
│   └── _data/               # Runtime data, build output, local server files
└── .claude/skills/takaro-7d2d-engineer/
```

## Workflow Notes

- Ground code changes in decompiled 7D2D APIs rather than memory or generic docs.
- Use `./scripts/build-mod.sh` to verify the mod still compiles.
- Use `./scripts/build-mod.sh deploy` to copy the built mod into the local test server.
- Use `docker compose logs` inside `7d2d/` to inspect server behavior.
- Keep `IMPLEMENTATION_STATUS.md` current when behavior or protocol coverage changes.
