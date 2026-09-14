# Build & Deploy

## Requirements

- **JDK 25 as the Gradle JVM for the whole multi-project build.** Fabric Loom 1.17.20
  requires Gradle >= 9.5 on a JDK 25 daemon, and the `fabric` project is evaluated even
  when you only build `:paper:build` — so JDK 21 fails the entire build, not just Fabric.
- Gradle 9.5.1 (wrapper included)
- Output targets: `core`, `paper`, `neoforge` compile with `--release 21` (class files stay
  Java 21 for the 1.21.11 servers); `fabric` compiles with release 25 for Minecraft 26.2.
- The `neo` box has **no host JDK** — build via
  `dev-servers/scripts/deploy-connector.sh minecraft-<platform>` (or `ds_toolchain_run`),
  which runs the build inside `eclipse-temurin:25-jdk`.

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

| Key | Value |
|---|---|
| `minecraft-fabric` | `26.2` |
| `paper-api` | `1.21.11-R0.1-SNAPSHOT` |
| `neoforge-version` | `21.11.38-beta` |
| `fabric-loader` | `0.19.5` |
| `fabric-api` | `0.160.0+26.2` |
| `fabric-loom` | `1.17.20` (plugin id `net.fabricmc.fabric-loom`) |
| `shadow` | `9.4.2` |
| `neoforge-moddev` | `2.0.141` |
| Gradle wrapper | `9.5.1` |

Minecraft versions are year-based since 26.1 (26.1 → 26.1.2 → 26.2, released 2026-06-16).
1.21.11 (2025-12-09) is the last of the old scheme and is what Paper and NeoForge target.

## Build Gotchas

- **Loom plugin id changed**: it is `net.fabricmc.fabric-loom` (not the old `fabric-loom`).
- **From Minecraft 26.1 the game ships unobfuscated**, so the Fabric build has no remapping:
  no `mappings(...)`, no `modImplementation`, no `remapJar`. `shadowJar` is the final
  artifact, exactly like Paper.
- **The whole build needs a JDK 25 Gradle JVM** — `:fabric` is configured during evaluation
  of any task, so `./gradlew :paper:build` on JDK 21 also fails.
- Paper/NeoForge/core stay on Java 21 class files via `--release 21`; don't "simplify" that
  to the toolchain's 25 or the 1.21.11 servers will refuse the jars.
- On a host without a JDK (e.g. `neo`), use `dev-servers/scripts/deploy-connector.sh` /
  `ds_toolchain_run` so the build runs in `eclipse-temurin:25-jdk`.
- Shadow plugin relocates dependencies to `io.takaro.libs.*` to avoid classpath conflicts
- NeoForge uses ModDevGradle; Fabric uses Fabric Loom — these manage platform-specific build steps
