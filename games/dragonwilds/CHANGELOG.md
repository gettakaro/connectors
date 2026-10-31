# Changelog

## [0.7.1](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.7.0...dragonwilds-v0.7.1) (2026-10-31)


### Documentation

* one README layout for every connector ([#703](https://github.com/gettakaro/connectors/issues/703)) ([1b4955d](https://github.com/gettakaro/connectors/commit/1b4955d01385777ea8db589c52adf5eec20173f9))

## [0.7.0](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.6.0...dragonwilds-v0.7.0) (2026-10-30)


### Features

* **dragonwilds:** resolve the server chat name in one helper ([#693](https://github.com/gettakaro/connectors/issues/693)) ([399fce2](https://github.com/gettakaro/connectors/commit/399fce260f5a49e14a6fa3f0650b10eabba60c48))


### Documentation

* **dragonwilds:** list builds 26161880 and 26087591 from the 0.6.0 release ([#664](https://github.com/gettakaro/connectors/issues/664)) ([8f0f9e4](https://github.com/gettakaro/connectors/commit/8f0f9e44afa8036693c61c5437b44a44630c7ef8))

## [0.6.0](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.5.0...dragonwilds-v0.6.0) (2026-10-26)


### Features

* **dragonwilds:** speak protocol v1 and report the game profile, connector identity and game icon ([#562](https://github.com/gettakaro/connectors/issues/562)) ([f14f237](https://github.com/gettakaro/connectors/commit/f14f237884786c89f7756190f99646e4e4d90d6e))
* **dragonwilds:** support Steam build 26161880 ([#618](https://github.com/gettakaro/connectors/issues/618)) ([0b4c757](https://github.com/gettakaro/connectors/commit/0b4c757fb615e52d125e0f6b7264762542df8798))


### Bug Fixes

* **dragonwilds:** declare the world map Dimension and tag positions with it ([#624](https://github.com/gettakaro/connectors/issues/624)) ([d833657](https://github.com/gettakaro/connectors/commit/d833657a5e925437df538c0c24f5a4cbfca2a07a))
* **dragonwilds:** list players only once they are in world ([#640](https://github.com/gettakaro/connectors/issues/640)) ([ebd771d](https://github.com/gettakaro/connectors/commit/ebd771d6bc7845598a9fb01ebdeac6cf815046b6))


### Documentation

* **dragonwilds:** date the works table and name the per-build plugin archives ([#584](https://github.com/gettakaro/connectors/issues/584)) ([2ff22b9](https://github.com/gettakaro/connectors/commit/2ff22b9a31049e58f4dcdccf80cae0ab49bdd42f))

## [0.5.0](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.4.0...dragonwilds-v0.5.0) (2026-10-18)


### Features

* **dragonwilds:** debug map survey for an operator map upload ([#490](https://github.com/gettakaro/connectors/issues/490)) ([e4ed9a2](https://github.com/gettakaro/connectors/commit/e4ed9a2c638758f9c336d75ef360cfdceecc99b6))
* **dragonwilds:** support Steam build 25991736 ([#533](https://github.com/gettakaro/connectors/issues/533)) ([a4633e7](https://github.com/gettakaro/connectors/commit/a4633e77c436174f4ec1ef05ac5277c09d3cef5a))


### Bug Fixes

* **dragonwilds:** keep the config in one file the connector creates ([#469](https://github.com/gettakaro/connectors/issues/469)) ([2f55ab1](https://github.com/gettakaro/connectors/commit/2f55ab1198bef83c126ed3bb6c74ccbefa01a467))
* **dragonwilds:** say which takaro.cfg keys reload, document DRAGONWILDS_LOG_RATE ([#504](https://github.com/gettakaro/connectors/issues/504)) ([367a753](https://github.com/gettakaro/connectors/commit/367a75366845cef12549740bc7e4ce4953968372))

## [0.4.0](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.3.1...dragonwilds-v0.4.0) (2026-10-11)


### Features

* **dragonwilds:** ship takaro.cfg and apply token changes without a restart ([#444](https://github.com/gettakaro/connectors/issues/444)) ([a38eae3](https://github.com/gettakaro/connectors/commit/a38eae3c985e42281736b2f8ffaa5d920699ccc3))

## [0.3.1](https://github.com/gettakaro/connectors/compare/dragonwilds-v0.3.0...dragonwilds-v0.3.1) (2026-10-11)


### Bug Fixes

* **dragonwilds:** release the plugin for Steam build 25808123 ([#429](https://github.com/gettakaro/connectors/issues/429)) ([e2d463f](https://github.com/gettakaro/connectors/commit/e2d463fef0ae272d11955d5178e2e8a667779260))

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
