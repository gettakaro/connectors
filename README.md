# Takaro Game Server Connectors

Monorepo for connector plugins that implement the [Takaro Generic Connector Protocol](https://docs.takaro.io/advanced/generic-connector-protocol) for different game servers.

## Connectors

| Connector | Directory | Language | Build |
|-----------|-----------|----------|-------|
| Rust | [`games/rust/`](games/rust/) | C# | None (Carbon runtime compile) |
| Minecraft | [`games/minecraft/`](games/minecraft/) | Java 21 | Gradle |
| 7D2D | [`games/7d2d/`](games/7d2d/) | C# / .NET Framework 4.8 | Dockerized Mono `msbuild` |
| Project Zomboid | [`games/zomboid/`](games/zomboid/) | Java 25 (`-javaagent`, ByteBuddy) | Gradle |
| Conan Exiles | [`games/conan-exiles/`](games/conan-exiles/) | TypeScript | Node.js |
| Terraria | [`games/terraria/`](games/terraria/) | C# / .NET 9 | TShock reference build |
| Valheim | [`games/valheim/`](games/valheim/) | C# / .NET | BepInEx dedicated-server plugin and graphical-client companion |
| Enshrouded | [`games/enshrouded/`](games/enshrouded/) | C++ (`dbghelp.dll` proxy) + TypeScript sidecar | zig cross-compile + Node.js |
| RuneScape: Dragonwilds | [`games/dragonwilds/`](games/dragonwilds/) | C++ (`LD_PRELOAD` native plugin) + TypeScript sidecar | `debian:bookworm` g++ + Node.js |
| VEIN | [`games/vein/`](games/vein/) | C++ (`LD_PRELOAD` native connector) | `debian:bookworm` g++ |
| Dune: Awakening | [`games/dune/`](games/dune/) | TypeScript sidecar + optional C++ (`LD_PRELOAD` native plugin) | Node.js + `debian:bookworm` g++ |

Each connector is self-contained with its own Docker dev environment, build system, and scripts. See each connector's README for details.

## Setup

```bash
cp .env.example .env    # Fill in TAKARO_REGISTRATION_TOKEN at minimum
```

## Commands

All operations are in the `justfile`. Run `just --list` to see available commands.

## Maintenance

Every connector is built, verified and released against exact, reviewed server targets recorded in
[`catalog/`](catalog/README.md). One command, [`takaro-maint`](maintenance/README.md), resolves a target, installs its
pinned inputs, builds and validates the artifact, deploys it and proves it runs — on a laptop, in
[`dev-servers/`](dev-servers/README.md) and in CI alike. A scheduled run watches every upstream source, files a
maintenance issue when something new appears, and closes it only once a stable release demonstrably ships the target
([lifecycle](maintenance/docs/lifecycle.md)). Operators start at [operations](maintenance/docs/operations.md) and
[recovery](maintenance/docs/recovery.md); the [support policy](maintenance/docs/support-policy.md) says what is
maintained, what is retained and how a target retires; private deployments follow the
[integration contract](maintenance/docs/integration.md).

## Test Environment

[`dev-servers/`](dev-servers/README.md) is a unified Docker Compose environment that runs a
game server for every connector in this repo — plus Palworld through a third-party bridge —
with the real connector or server mod installed and configured, not just the base game.

```bash
cp dev-servers/.env.example dev-servers/.env   # fill in TAKARO_REGISTRATION_TOKEN
just dev-install-all                           # installs games one at a time
just dev-start minecraft-paper                 # start what you need
just dev-status                                # what is running and what it costs
```

Each game has its own Compose file, its own ports and its own `_data/` tree, and the
scripts refuse to start more games than the host's RAM budget allows. The per-connector
environments under `games/rust/`, `games/minecraft/` and `games/7d2d/` are unaffected.

## Releasing

Connectors are versioned and released independently through Release Please. Merging a connector's release PR is the only human gate. Release Please then creates the version tag and the GitHub release **as a draft**, the connector's workflow builds and verifies every target from that tag's source, attaches the whole set, reads it back from GitHub, and only then publishes the release. Nothing is downloadable until the complete set is there. To force a specific version, add a `Release-As: X.Y.Z` footer to a commit on `main`.

Every release carries, besides its artifacts: `SHA256SUMS`, a compatibility record (`takaro-<connector>-<version>.compat.json`) naming the exact inputs, source commit, target fingerprint, verification level and hash of everything in it, and one verification report per target. Connectors with several server targets name their artifacts per target (`takaro-minecraft-mod-fabric-26.2-0.1.2.jar`). A connector whose earlier releases used unversioned asset names ships byte-identical copies under those names for two stable releases after it moves to the catalog (`legacyAssetAliases` in its `game.json`). Minecraft's earlier names (`takaro-fabric-<version>.jar`, `takaro-paper-<version>.jar`, `takaro-neoforge-<version>.jar`) are carried this way too, each an alias of its platform's default target; `games/minecraft/README.md` maps old to new.

If a release ends up missing its assets, re-run the connector's workflow against the existing tag — it rebuilds from that tag's source, skips anything already there with identical bytes, and refuses to overwrite anything that differs:

```bash
gh workflow run minecraft.yml --ref main -f tag=minecraft-v0.1.2 -f version=0.1.2
```

Pre-release builds (`<connector>-dev` on every push to `main`, `pr-<n>-<connector>` on every push to a PR) are replaced atomically: the whole replacement is staged and verified before the old one is removed, so a dev build is never half-updated. PR builds are deleted when the PR closes.

See [`maintenance/docs/release.md`](maintenance/docs/release.md) for the full picture.

Build release artifacts locally with:

```bash
just build-release-rust 1.0.1
just build-release-minecraft 1.2.3
just build-release-7d2d 1.4.0
just build-release-zomboid 0.0.1
just build-release-conan 1.0.0
just build-release-terraria 0.1.0
just build-release-valheim 2.0.0
```

## Connector Notes

- `games/rust/` and `games/minecraft/` use the shared root [`.env.example`](.env.example) pattern.
- `games/7d2d/` keeps its runtime config in `Config.xml` generated by the mod and stores local state under `games/7d2d/_data/`.
- `games/zomboid/` is an in-game mod delivered as a Java agent (`-javaagent`) injected into the Project Zomboid B42 server JVM; it reads its Takaro config from environment variables and stores local build references under `games/zomboid/_deps/`.
- `games/conan-exiles/` is a server-side TypeScript sidecar that connects through Takaro WebSocket, Conan RCON, optional log tailing, optional save DB reads, and an optional server-side chat helper.
- `games/terraria/` is a server-side TShock plugin that emits Takaro event markers and
  coordinate helper commands. It stores local build references under `games/terraria/_data/`.
- [`games/valheim/`](games/valheim/README.md) provides a dedicated-server connector and a separately packaged graphical-client companion. Takaro credentials remain on the server; the companion reports client-owned gameplay observations through the server.

## Documentation

- [Generic Connector Protocol](https://docs.takaro.io/advanced/generic-connector-protocol)
- [Connection Architecture](https://docs.takaro.io/advanced/connection-architecture)
- [Adding Support for a New Game](https://docs.takaro.io/advanced/adding-support-for-a-new-game)
