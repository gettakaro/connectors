# Changelog

All notable changes to the Takaro Project Zomboid connector are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/) and the
project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

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
