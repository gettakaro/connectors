# Changelog

## [0.2.0](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.1.0...dragonwilds-v0.2.0) (2026-09-17)


### Features

* **dragonwilds:** RuneScape Dragonwilds connector (LD_PRELOAD plugin + sidecar) ([#179](https://github.com/gettakaro/connectors/issues/179)) ([15baea9](https://github.com/gettakaro/connectors/commit/15baea9986f98c680a00f5e3179f728ad03d62b9))

## 0.1.0 (2026-09-16)

### Features

* **dragonwilds:** initial RuneScape: Dragonwilds connector — `libtakaro-dragonwilds.so`
  (LD_PRELOAD native plugin resolving the server's own `.sym` symbols, loopback HTTP API) plus a
  TypeScript sidecar speaking the Takaro Generic Connector Protocol. Players, positions,
  inventories, items, entities, chat, teleports, gives, kicks, bans, shutdown and the
  join/leave/chat/death/kill events. See README.md for what is proven and what is not.
