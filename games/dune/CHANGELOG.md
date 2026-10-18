# Changelog

## [0.4.0](https://github.com/gettakaro/connectors/compare/dune-v0.3.0...dune-v0.4.0) (2026-10-18)


### Features

* **dune:** protocol v1 with the Hagga Basin map Dimension ([#481](https://github.com/gettakaro/connectors/issues/481)) ([0418d20](https://github.com/gettakaro/connectors/commit/0418d207491ea96f19d5a1de396b21b422e07043))


### Bug Fixes

* **dune:** keep the config in one file the connector creates ([#467](https://github.com/gettakaro/connectors/issues/467)) ([9256c31](https://github.com/gettakaro/connectors/commit/9256c315507228bdf7fe68749ec920ce6b9ffa1b))
* **dune:** read TAKARO_PROTOCOL from .env and pass it through compose ([#500](https://github.com/gettakaro/connectors/issues/500)) ([9855843](https://github.com/gettakaro/connectors/commit/9855843c13e036ba11d993e53d699ef151c2ca02))

## [0.3.0](https://github.com/gettakaro/connectors/compare/dune-v0.2.3...dune-v0.3.0) (2026-10-11)


### Features

* **dune:** apply Takaro token and identity changes from .env without a restart ([#438](https://github.com/gettakaro/connectors/issues/438)) ([01ea5a2](https://github.com/gettakaro/connectors/commit/01ea5a237c8a855c3d7d04830aea85e614a002a2))

## [0.2.3](https://github.com/gettakaro/connectors/compare/dune-v0.2.2...dune-v0.2.3) (2026-10-10)


### Bug Fixes

* **dune:** support Steam build 25789279, keep presence events flowing under log floods ([#418](https://github.com/gettakaro/connectors/issues/418)) ([c187efc](https://github.com/gettakaro/connectors/commit/c187efc52ceac40094425129a19a1cbd9fa15f72))

## [0.2.2](https://github.com/gettakaro/connectors/compare/dune-v0.2.1...dune-v0.2.2) (2026-10-08)


### Bug Fixes

* **dune:** support Steam build 25689360, render whispers on clients 25611704/25690505 ([#386](https://github.com/gettakaro/connectors/issues/386)) ([27d3e57](https://github.com/gettakaro/connectors/commit/27d3e57097a53354753a2300649469c19d3b0293))

## [0.2.1](https://github.com/gettakaro/connectors/compare/dune-v0.2.0...dune-v0.2.1) (2026-10-07)


### Bug Fixes

* **dune:** never send a platformId Takaro's IGamePlayer pattern rejects ([#369](https://github.com/gettakaro/connectors/issues/369)) ([4a48e10](https://github.com/gettakaro/connectors/commit/4a48e10fc47500ed622d4143e233f34c2010013f))

## [0.2.0](https://github.com/gettakaro/connectors/compare/dune-v0.1.0...dune-v0.2.0) (2026-09-23)


### Features

* **dune:** add Dune: Awakening connector ([#259](https://github.com/gettakaro/connectors/issues/259)) ([f350813](https://github.com/gettakaro/connectors/commit/f350813bc39b851b9339b1b213b2b7401aa0a5c3))

## 0.1.0 (2026-09-21)

### Features

* **dune:** initial Dune: Awakening connector — a TypeScript sidecar that adapts a self-hosted
  **battlegroup** to Takaro out of process (the game RabbitMQ GM command bus for every write, the
  `chat.*` exchanges for chat in and out, and a read-only connection to the battlegroup's Postgres
  for the roster, inventories and life state) plus an **optional** `LD_PRELOAD` native plugin
  (`libtakaro-dune.so`) for the things no out-of-process path gives: kill attribution, live player
  position and precise connect/disconnect. Dune has no RCON. See README.md for what is proven and
  what is not.
* **dune:** players, single-player lookups, positions, inventories, the item and entity catalogues,
  broadcasts and whispers, item grants, teleports, kicks, timed and permanent bans, a connector
  console command set, and the join/leave/chat/death events.
* **dune:** every mutation is **read back** from the game before it answers success — a grant
  re-reads the inventory, a teleport re-reads the position, a kick re-reads the roster — so a GM
  command the game silently ignored is reported as a failure instead of a success.
* **dune:** bans are owned by the connector (Dune has no native ban): a banned player is kicked on
  sight by the presence poller, timed bans are lifted by the connector at expiry, and both survive
  a sidecar restart and a Takaro outage.
* **dune:** guaranteed event delivery across a Takaro outage — events are held until Takaro
  confirms them and re-sent in order after a reconnect, with a persisted cursor so a restart
  replays nothing.
* **dune:** the plugin validates every address it resolves at load; a feature a game update broke
  reports `degraded` in `/health` and in Takaro's reachability reason while the map server and
  every other feature keep running. The server binary ships no function symbols, so resolution is
  anchored on RTTI and vtables and re-validated on every boot.
* **dune:** the item catalogue is **generated**, not bundled: `sidecar/scripts/gen-catalogue.mjs`
  builds it from the community wiki API and refuses to pass an asset-id-looking name to Takaro.
  See `sidecar/data/ATTRIBUTION.md`.
