# Changelog

## [0.2.3](https://github.com/gettakaro/connectors/compare/vein-v0.2.2...vein-v0.2.3) (2026-09-23)


### Documentation

* **vein:** fix release download link ([#262](https://github.com/gettakaro/connectors/issues/262)) ([f4b0d45](https://github.com/gettakaro/connectors/commit/f4b0d45260cd3f75099d6be6b17d45915f971301))

## [0.2.2](https://github.com/gettakaro/connectors/compare/vein-v0.2.1...vein-v0.2.2) (2026-09-22)


### Documentation

* make connector READMEs the source of takaro.io game docs ([#218](https://github.com/gettakaro/connectors/issues/218)) ([b45d6cd](https://github.com/gettakaro/connectors/commit/b45d6cd682a61b11d485b1724e79b1e0b61e5305))

## [0.2.1](https://github.com/gettakaro/connectors/compare/vein-v0.2.0...vein-v0.2.1) (2026-09-17)


### Bug Fixes

* **vein:** release packaging and install docs ([#185](https://github.com/gettakaro/connectors/issues/185)) ([eb85b8d](https://github.com/gettakaro/connectors/commit/eb85b8daf3a87341e1ed4da142579b8e7f8be5de))

## [0.2.0](https://github.com/gettakaro/connectors/compare/vein-v0.1.0...vein-v0.2.0) (2026-09-17)


### Features

* **vein:** VEIN connector (LD_PRELOAD plugin + sidecar) ([#182](https://github.com/gettakaro/connectors/issues/182)) ([da272a2](https://github.com/gettakaro/connectors/commit/da272a2d58223eb36af84ccefa645eae72fd32e4))

## 0.1.0 (2026-09-17)

### Features

* **vein:** initial VEIN connector — `libtakaro-vein.so` (LD_PRELOAD native plugin for the Linux
  dedicated server, resolving the game's own code by symbol table / depot symbols / signature scan
  and serving a loopback HTTP API) plus a TypeScript sidecar speaking the Takaro Generic Connector
  Protocol. Players, positions, inventories, items, entities, chat, broadcasts and whispers,
  gives, teleports, kicks, timed and permanent bans, shutdown, a connector-provided console
  command set, and the join/leave/chat/death/kill events. See README.md for what is proven and
  what is not.
* **vein:** guaranteed event delivery across a Takaro outage — every game event is held until
  Takaro's heartbeat confirms it arrived, and anything unconfirmed when the socket dies is re-sent
  in order after the reconnect.
* **vein:** the plugin self-checks every resolved game address at load; a feature a game update
  broke reports `degraded` in `/health` and in Takaro's reachability reason while the server and
  every other feature keep running.
* **vein:** the connector restarts itself into the game container's new network namespace after a
  game-container restart (`SIDECAR_EXIT_AFTER_UNREACHABLE_MS`), and lifts timed bans itself at
  expiry because the game has no ban expiry of its own.

### Bug Fixes

* **vein:** kill events are attributed from the game's own damage instigator instead of "the only
  player online", so the AI's kills among itself are no longer reported as player kills, and every
  kill event carries the killer's held item as the required `weapon` string (`unknown` when nothing
  identifiable was held, `debug` for a debug-endpoint kill).
* **vein:** a fall or drowning death no longer reports the victim as their own attacker; the cause
  is carried in the death message instead.
* **vein:** items VEIN stores as one object per unit (corn, MREs, insulin, jambalaya) are handled
  correctly — a give of N delivers N objects instead of one, and the inventory reports one row per
  item with the units summed instead of N rows of one.
* **vein:** the inventory is read from the character's own inventory component on the controller's
  pawn, so corpse containers and the offline-character cache can no longer be picked up, and a
  player with no character answers "no character" instead of an empty inventory.
