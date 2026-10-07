# Changelog

## [0.3.0](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.2.0...dragonwilds-v0.3.0) (2026-10-07)


### ⚠ BREAKING CHANGES

* **dragonwilds:** connect the Linux plugin directly to Takaro and drop the sidecar ([#352](https://github.com/gettakaro/connectors/issues/352))

### Features

* **dragonwilds:** add catalog target for Steam build 25630937 ([#360](https://github.com/gettakaro/connectors/issues/360)) ([a254c4a](https://github.com/gettakaro/connectors/commit/a254c4a25f02434960513a23759cb4932e08ac76))
* **dragonwilds:** add first catalog target for Steam build 25465077 ([#325](https://github.com/gettakaro/connectors/issues/325)) ([44365ec](https://github.com/gettakaro/connectors/commit/44365ec41193d8dff57a813b88746b958ffbb9b5))
* **dragonwilds:** connect the Linux plugin directly to Takaro and drop the sidecar ([#352](https://github.com/gettakaro/connectors/issues/352)) ([6682328](https://github.com/gettakaro/connectors/commit/66823289ddef447c307dedefb0cdde89392471e4))


### Bug Fixes

* **dragonwilds:** read the Steam account behind the EOS id from EOS Connect ([#376](https://github.com/gettakaro/connectors/issues/376)) ([bba1606](https://github.com/gettakaro/connectors/commit/bba1606c5503fbe1d5c40b1fbf3e110549b004a9))

## [0.2.0](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.1.0...dragonwilds-v0.2.0) (2026-09-22)


### Features

* **dragonwilds:** RuneScape Dragonwilds connector (LD_PRELOAD plugin + sidecar) ([#179](https://github.com/gettakaro/connectors/issues/179)) ([15baea9](https://github.com/gettakaro/connectors/commit/15baea9986f98c680a00f5e3179f728ad03d62b9))


### Documentation

* make connector READMEs the source of takaro.io game docs ([#218](https://github.com/gettakaro/connectors/issues/218)) ([b45d6cd](https://github.com/gettakaro/connectors/commit/b45d6cd682a61b11d485b1724e79b1e0b61e5305))

## 0.1.0 (2026-09-16)

### Features

* **dragonwilds:** initial RuneScape: Dragonwilds connector — `libtakaro-dragonwilds.so`
  (LD_PRELOAD native plugin resolving the server's own `.sym` symbols, loopback HTTP API) plus a
  TypeScript sidecar speaking the Takaro Generic Connector Protocol. Players, positions,
  inventories, items, entities, chat, teleports, gives, kicks, bans, shutdown and the
  join/leave/chat/death/kill events. See README.md for what is proven and what is not.
