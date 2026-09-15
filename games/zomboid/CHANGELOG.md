# Changelog

All notable changes to the Takaro Project Zomboid connector are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/) and the
project adheres to [Semantic Versioning](https://semver.org/).

## 1.0.0 (2026-09-14)


### Features

* **zomboid:** add Project Zomboid Takaro connector ([#96](https://github.com/gettakaro/connectors/issues/96)) ([63b2616](https://github.com/gettakaro/connectors/commit/63b261617c53591e980f1688e95e6e97a1fb68e7))


### Bug Fixes

* **zomboid:** keep dead players online and reformat README ([#98](https://github.com/gettakaro/connectors/issues/98)) ([10a1f81](https://github.com/gettakaro/connectors/commit/10a1f817c4fa2550d4105b89b4c67941e66e50b8))
* **zomboid:** record player-disconnected in Takaro ([#99](https://github.com/gettakaro/connectors/issues/99)) ([a362647](https://github.com/gettakaro/connectors/commit/a3626475b74605ac9d7c40b37ed81e919c1c62e3))

## [Unreleased]

### Added — M2 (remaining actions + events + repo integration)

- **All 17 actions implemented** — the five M2 actions no longer throw
  `UnsupportedOperationException`:
  - `shutdown` → `GameServer.rcon("quit")` (the `QuitCommand` path).
  - `listItems` → `ScriptManager.getAllItems()` mapped to
    `{code=fullName, name=displayName, description=displayCategory}`, de-duplicated
    by code and cached ~10 min (5092 `Base.*` items on the live 42.20.4 server).
  - `getPlayerInventory` → `ItemContainer.getItems()` grouped by `getFullType()`
    (summed counts, `quality=condition/conditionMax`).
  - `listEntities` → static `Zombie` + `ScriptManager.getAllGameEntityTemplates()`
    (`GameEntityTemplate`) + `getAllVehicleScripts()` (`VehicleScript`)
    (242 entries on the live server).
  - `listLocations` → safehouses from the static `SafeHouse.safehouseList`
    (empty on a world with no claimed safehouses).
- **Events `entity-killed` and `log` implemented:**
  - `entity-killed` — Advice on `IsoZombie.onKilled`, emitted only when the killer
    is an `IsoPlayer`; `entity="Zombie"`, `weapon=HandWeapon.getFullType()`.
    Token-bucket rate-limited (20/s, burst 50) with a single count-only drop
    warning.
  - `log` — `EventManager.registerCallback(IEventController)` forwarding
    `process(String)`, plus an Advice on the `ZLogger.write(String,String,boolean)`
    funnel filtered to the `user` logger. Off unless `logEvents=true`;
    rate-limited.
- `TokenBucket` rate limiter and `Catalog` (item-catalogue / inventory transforms)
  with unit tests (10 new tests; 57 total).
- `debugCatalog` config flag logs `listItems`/`listEntities`/`listLocations` sizes
  once at start (a server-side catalogue check when no Takaro REST driver exists).
- **Repo integration (shippable):** registered in `dev-servers`
  (`deploy-connector.sh zomboid`, registry row, env-var config), CI workflow
  `zomboid.yml` (JDK 25, SteamCMD jar staging with sha-keyed cache),
  release-please package `zomboid@0.0.0`, `justfile` targets,
  `scripts/build-release.sh`, and the `takaro-zomboid-engineer` skill.

### Added — M1 (core lift + milestone-1 surface)

- **`core/` module** lifted from `minecraft/core` (package
  `io.takaro.zomboid.core`), a WebSocket connector that speaks the Takaro
  protocol over Java-WebSocket + Gson. Three behavioural fixes over the
  Minecraft original:
  - application-level `ping` → `pong` heartbeat (previously fell through to
    "unknown message type");
  - tolerant `gameId` extraction from flat `{gameId}`, nested
    `{player:{gameId}}` / `{playerRef:{gameId}}`, JSON-string args, `{}` and
    `[]`;
  - a 10 s timeout on main-thread actions so a stalled game loop cannot hang a
    Takaro request forever.
  - `PlayerInfo` platform prefix set to `steam:`.
  - `GameAdapter.onConnectionEstablished()` hook so the adapter can re-seed its
    player registry silently on every (re)connect.
- **`agent/` module** — a `-javaagent` (`Premain-Class`) that instruments the
  Project Zomboid B42 (42.20.4) server JVM with ByteBuddy 1.18.13:
  - **Actions:** `testReachability`, `getPlayers`, `getPlayer`,
    `getPlayerLocation`, `sendMessage` (global + targeted),
    `executeConsoleCommand`, `kickPlayer`, `banPlayer`, `unbanPlayer`,
    `listBans`, `teleportPlayer`, `giveItem`.
  - **Events:** `player-connected`, `player-disconnected`, `chat-message`,
    `player-death`.
  - Actions reserved for M2 (`shutdown`, `listItems`, `getPlayerInventory`,
    `listEntities`, `listLocations`) return a clean protocol error rather than
    hanging.
  - Connector starts from the first `RCONServer.update()` tick (not from
    `premain`, which runs three times per container start), on the game main
    thread, after the game classes are loaded. Hooks fail loud via
    first-invocation sentinels and a tick watchdog.
  - Timed bans are tracked in `Takaro/bans.json` and expired by the reconciler
    (Project Zomboid has no native ban expiry).

[Unreleased]: https://github.com/gettakaro/connectors/compare/main...HEAD
