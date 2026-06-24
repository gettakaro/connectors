# Build & Deploy

## Requirements

- Java 21 (Gradle manages its own toolchain — system Java version may differ)
- Gradle 9.4.0 (wrapper included)

## Build Commands

```bash
just minecraft-build              # Build all modules (core, paper, neoforge, fabric)
just minecraft-build-module paper # Build single module
cd minecraft && ./gradlew :core:build  # Build core only
```

Output JARs:
- `minecraft/paper/build/libs/takaro-paper-<version>.jar`
- `minecraft/neoforge/build/libs/takaro-neoforge-<version>.jar`
- `minecraft/fabric/build/libs/takaro-fabric-<version>.jar`

## Deploy to Dev Servers

```bash
just minecraft-deploy paper        # Deploy to Paper
just minecraft-deploy all          # Deploy to all platforms
```

The deploy script copies built JARs into `minecraft/_data/<platform>/plugins|mods/TakaroMinecraft.jar`.

**You must build before deploying.** The deploy script does not trigger a build.

## Reload After Deploy

```bash
just minecraft-reload paper        # Paper: sends `reload confirm` via RCON
```

NeoForge and Fabric cannot hot-reload — restart the container instead:

```bash
cd minecraft && docker compose restart neoforge
cd minecraft && docker compose restart fabric
```

## Version Catalog

Dependencies are managed in `minecraft/gradle/libs.versions.toml`:
- Minecraft 1.21.11
- Paper API 1.21.11-R0.1-SNAPSHOT
- NeoForge 21.11.38-beta
- Fabric Loader 0.18.4
- Shadow Plugin 9.3.2

## Build Gotchas

- System `java --version` may show Java 11, but Gradle uses its own Java 21 toolchain
- Shadow plugin relocates dependencies to `io.takaro.libs.*` to avoid classpath conflicts
- NeoForge uses ModDevGradle; Fabric uses Fabric Loom — these manage platform-specific build steps
