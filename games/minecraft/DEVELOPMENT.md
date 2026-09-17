# Takaro Minecraft Connector — Development

Notes for working on the connector itself. Server operators only need [`README.md`](README.md).

## Platform / toolchain matrix

| Platform | Minecraft | Java (runtime) |
|----------|-----------|----------------|
| Fabric | 26.2 | Java 25 |
| Paper | 1.21.11 | Java 21 |
| NeoForge | 1.21.11 | Java 21 |

- **Building requires JDK 25** for the whole multi-project build (Fabric Loom 1.17.20 needs
  Gradle >= 9.5 on a JDK 25 Gradle JVM, and the fabric project is evaluated even when you
  build only one target). Paper, NeoForge and core are compiled with `--release 21`,
  so their class files stay Java 21.
- Docker and Docker Compose (for running servers).

### Client compatibility

A client's Minecraft version must match the server's. The Fabric connector runs on
Minecraft 26.2, so players need **26.2 clients** — 26.x clients cannot join 1.21.11 servers,
and 1.21.11 clients cannot join 26.x servers. Paper and NeoForge stay on 1.21.11 clients.

## Targets

Every platform is built from a **catalog target**: `catalog/minecraft/targets/<id>.json` pins the
exact Minecraft version, the loader or server build, its dependencies, the Java level, the toolchain
image and the runtime image, each by hash or digest. Nothing in the Gradle files chooses a game
version any more.

```
catalog/minecraft/targets/fabric-26.2.json       the record
games/minecraft/mod/targets/fabric-26.2/         its Gradle project (a one-line build file)
games/minecraft/mod/buildSrc/                    the conventions that read the record
```

The Gradle project is named after the target and reads its own record, so adding a target is adding
a JSON file and a directory — `settings.gradle.kts` picks it up on its own. `just maint targets list --game minecraft`
prints the targets that exist right now.

See [catalog/README.md](../../catalog/README.md) for the record format, the fingerprint and how to
add or retire a target.

## Build

```bash
just maint build --game minecraft --target fabric-26.2 --version 0.1.1 --out dist
```

That resolves the target, runs Gradle, checks the jar carries the target's identity, and writes
`dist/build-manifest.json`, `dist/SHA256SUMS` and a `.meta.json` beside the jar. Add
`--toolchain container` to build in the pinned `eclipse-temurin` image instead of the host JDK.

Gradle directly still works:

```bash
(cd mod && ./gradlew :fabric-26.2:build)          # one target
(cd mod && ./gradlew :paper-1.21.11:build)        # another target
(cd mod && ./gradlew :core:test :buildSrc:test)   # unit tests and fingerprint parity
```

Artifacts:

- `targets/fabric-26.2/build/libs/takaro-minecraft-mod-fabric-26.2-<version>.jar`
- `targets/paper-1.21.11/build/libs/takaro-minecraft-mod-paper-1.21.11-<version>.jar`
- `targets/neoforge-1.21.11/build/libs/takaro-minecraft-mod-neoforge-1.21.11-<version>.jar`

The target jar's name is part of its identity: `takaro-maint` looks it up by that exact name
rather than globbing `build/libs`, and the build fails if it does not match the catalog.

### Reproducible jars

Archives are built with fixed timestamps, ordering and permissions, and the source revision is
passed in rather than read inside the build, so two builds of the same tree produce **byte-identical**
jars. That is what makes a published release checkable: rebuild it and `cmp`.

```bash
(cd mod && ./gradlew clean :fabric-26.2:build -Pversion=0.1.1)
cp targets/fabric-26.2/build/libs/*.jar /tmp/first.jar
(cd mod && ./gradlew clean :fabric-26.2:build -Pversion=0.1.1)
cmp /tmp/first.jar mod/targets/fabric-26.2/build/libs/takaro-minecraft-mod-fabric-26.2-0.1.1.jar
```

### What the jar carries

`takaro.target-base` stamps every jar with `Takaro-Target`, `Takaro-Target-Fingerprint`,
`Takaro-Connector-Version`, `Takaro-Source-Revision`, `Takaro-Game-Version` and
`Takaro-Java-Release`, plus a `META-INF/takaro-target.json` the running mod reads back. On
startup the connector compares that stamp with what the server actually is and refuses to connect
on a mismatch (`TAKARO_TARGET_POLICY=warn` downgrades the refusal to a warning).

`verifyTargetInputs` resolves every build dependency the record pins a hash for and compares the
bytes before compiling, so a re-published artifact fails the build instead of shipping.

Release artifacts are collected by `scripts/build-release.sh <version> <out-dir>`, which is what
`.github/workflows/minecraft.yml` runs in CI before publishing the jars to the
`minecraft-v<version>` release.

## Verify a target

```bash
just maint verify --game minecraft --target fabric-26.2 --artifacts dist --out reports
```

This installs the pinned server files into a throwaway directory, deploys the built jar, boots the
pinned container against a local fake Takaro, and checks startup, the target check, identify,
heartbeat, players, the item and entity catalogues (display names, not registry ids), a console
command and a clean shutdown. It publishes no host ports and writes a report that validates
against `catalog/schema/v1/verify-report.schema.json`.

## Run servers

The Minecraft dev servers live in the shared rig `dev-servers/` — see
[dev-servers/README.md](../../dev-servers/README.md). It runs Fabric on 26.2 / Java 25 and
Paper/NeoForge on 1.21.11 / Java 21. The per-game compose file that used to be here is gone;
`games/minecraft/docker-compose.yml` now contains only the Mineflayer test bot, which has no rig
equivalent yet.

```bash
# Copy and fill in your Takaro credentials (from repo root), one time
cp dev-servers/.env.example dev-servers/.env

# Install and start a platform
just dev-install minecraft-paper
just dev-start minecraft-paper

# Repeat for minecraft-neoforge / minecraft-fabric as needed
just dev-logs minecraft-paper
```

| Rig game | Platform | Container | Game Port | RCON Port |
|----------|----------|-----------|-----------|-----------|
| minecraft-paper | Paper | takaro-dev-minecraft-paper | 25565 | 25575 |
| minecraft-neoforge | NeoForge | takaro-dev-minecraft-neoforge | 25566 | 25576 |
| minecraft-fabric | Fabric | takaro-dev-minecraft-fabric | 25567 | 25577 |

RCON password: set `RCON_PASSWORD` in `dev-servers/.env`.

### Mineflayer test bot

```bash
just minecraft-bot-up
```

The bot joins the rig servers over the host network on the ports above.

## Deploy and reload

```bash
# Build and deploy to Paper (builds from the working tree into the rig)
just dev-deploy minecraft-paper

# Repeat for other platforms as needed (minecraft-neoforge, minecraft-fabric)

just minecraft-reload paper
```

> **Note:** `just minecraft-reload` (which calls `scripts/reload.sh`) only works for **Paper**. For
> NeoForge and Fabric, restart the rig server instead:
> `just dev-stop minecraft-neoforge && just dev-start minecraft-neoforge`.

## Configuration

### Environment variables (recommended for Docker)

Environment variables override file-based config when set:

| Variable | Description |
|----------|-------------|
| `TAKARO_WS_URL` | WebSocket URL for your Takaro instance |
| `TAKARO_IDENTITY_TOKEN` | Unique identity token for this server |
| `TAKARO_REGISTRATION_TOKEN` | Registration token from the Takaro dashboard |
| `TAKARO_DEBUG` | Enable debug logging (`true` or `1`) — shows raw WebSocket messages |

In Docker Compose, these are passed to containers automatically from your `.env` file. Each
container gets a hardcoded `TAKARO_IDENTITY_TOKEN` (e.g. `takaro-paper-dev`).

### Config files

Each platform has its own config format and location. In the Docker Compose rig these live under
`_data/<platform>/`; on a real server they are relative to the server directory (see the README).

**Paper** (`dev-servers/_data/minecraft-paper/plugins/TakaroMinecraft/config.yml`):

```yaml
takaro:
  websocket:
    url: "wss://connect.takaro.io/"
  authentication:
    identity_token: "your-identity-token"
    registration_token: "your-registration-token"
  reconnect:
    enabled: true
    delay: 5000
    max_delay: 300000
    backoff_multiplier: 1.5
  debug: false
```

**NeoForge** (`dev-servers/_data/minecraft-neoforge/config/takaro.properties`):

```properties
takaro.websocket.url=wss://connect.takaro.io/
takaro.authentication.identity_token=your-identity-token
takaro.authentication.registration_token=your-registration-token
takaro.debug=false
```

**Fabric** (`dev-servers/_data/minecraft-fabric/config/takaro.json`):

```json
{
  "websocket": { "url": "wss://connect.takaro.io/" },
  "authentication": {
    "identity_token": "your-identity-token",
    "registration_token": "your-registration-token"
  },
  "settings": { "debug": false }
}
```

## Project structure

```
games/minecraft/
├── mod/            # Gradle project for the in-game component
│   ├── core/       # Shared logic (WebSocket client, config, protocol)
│   ├── paper/      # Paper/Spigot adapter
│   ├── neoforge/   # NeoForge adapter
│   └── fabric/     # Fabric adapter
├── bot/            # Mineflayer test bot
├── scripts/        # Build-release, deploy, reload scripts
└── docker-compose.yml  # Mineflayer test bot only (servers live in dev-servers/)
```

`core` owns `TakaroConnector`, `TakaroWebSocketClient`, `TakaroConfig` and the `GameAdapter` /
`EventEmitter` interfaces; each platform module implements `GameAdapter` (for example
`FabricGameAdapter`) and wires the lifecycle hooks.

## Takaro integration

The connector implements the
[Takaro game connector protocol](https://docs.takaro.io/advanced/generic-connector-protocol). See
the [adding a new game guide](https://docs.takaro.io/advanced/adding-support-for-a-new-game) for
protocol details.

## License

This project is part of the Takaro platform ecosystem.
