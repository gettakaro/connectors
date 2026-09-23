# Changelog

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
