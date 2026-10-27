# Changelog

## [0.9.0](https://github.com/gettakaro/connectors/compare/enshrouded-v0.8.1...enshrouded-v0.9.0) (2026-10-26)


### Features

* **enshrouded:** give items at a level from the console ([#638](https://github.com/gettakaro/connectors/issues/638)) ([e1576b1](https://github.com/gettakaro/connectors/commit/e1576b1e11c526765dba14fc5f6fbba921b63fc8))
* **enshrouded:** name an unsupported game build in plugin.log, reachability and console ([#655](https://github.com/gettakaro/connectors/issues/655)) ([48090b9](https://github.com/gettakaro/connectors/commit/48090b9db217fbadc5813b32045ea5ec2f13f520))
* **enshrouded:** report character levels ([#639](https://github.com/gettakaro/connectors/issues/639)) ([d20182a](https://github.com/gettakaro/connectors/commit/d20182ab97f1af2d61ed2e1eeb30a4f3e4041d8d))
* **enshrouded:** report the game profile and connector identity to Takaro ([#553](https://github.com/gettakaro/connectors/issues/553)) ([8b8ea9e](https://github.com/gettakaro/connectors/commit/8b8ea9e9416b1da9eaae7ce203e9053094df071b))
* **enshrouded:** support Enshrouded 1.0 build 26083076 (game build 1273043) ([#582](https://github.com/gettakaro/connectors/issues/582)) ([0ef32f3](https://github.com/gettakaro/connectors/commit/0ef32f3647a226d228e1118c75499a198af4e189))


### Bug Fixes

* **enshrouded:** accept player names with spaces in console commands ([#656](https://github.com/gettakaro/connectors/issues/656)) ([948dd32](https://github.com/gettakaro/connectors/commit/948dd32789cc1f33532042b4fcd5e72e59acf597))
* **enshrouded:** give items at the quality Takaro sends ([#654](https://github.com/gettakaro/connectors/issues/654)) ([dd535ef](https://github.com/gettakaro/connectors/commit/dd535ef487b05034fe10712ca16dd7fa3f8becee))
* **enshrouded:** list map places under their in-game names, one marker per place ([#621](https://github.com/gettakaro/connectors/issues/621)) ([63bdcb0](https://github.com/gettakaro/connectors/commit/63bdcb057d789dba87d3f952650e1302705807d9))
* **enshrouded:** match logins to Steam accounts by peer and Steam auth ([#637](https://github.com/gettakaro/connectors/issues/637)) ([ee33767](https://github.com/gettakaro/connectors/commit/ee33767d7aa58f324690a728eba787a08263fd93))
* **enshrouded:** name every entity by its in-game name on 1.0 ([#658](https://github.com/gettakaro/connectors/issues/658)) ([e6ace3d](https://github.com/gettakaro/connectors/commit/e6ace3db7459397aa198c24996a4223ec7b5b629))
* **enshrouded:** place player-disconnected at the player's last real position ([#659](https://github.com/gettakaro/connectors/issues/659)) ([3fa059e](https://github.com/gettakaro/connectors/commit/3fa059e31a6e9aecd6733aa153ab0c64085edfd7))
* **enshrouded:** report the character name on connect and in the ban list ([#633](https://github.com/gettakaro/connectors/issues/633)) ([e97febb](https://github.com/gettakaro/connectors/commit/e97febb17aa1e56f26430431d7e6b7cd1eb6a9d1))
* **enshrouded:** upgrade the on-disk game data file to the plugin's embedded sections ([#657](https://github.com/gettakaro/connectors/issues/657)) ([3d4720e](https://github.com/gettakaro/connectors/commit/3d4720e66afefbb67d8a8939b472949dafc2b667))


### Documentation

* **enshrouded:** fix the README install steps a new server owner follows ([#589](https://github.com/gettakaro/connectors/issues/589)) ([6d78728](https://github.com/gettakaro/connectors/commit/6d78728fa68bd790c62b3a57ee2f27b291089bd1))
* **enshrouded:** research private Takaro commands in server chat ([#605](https://github.com/gettakaro/connectors/issues/605)) ([17b4717](https://github.com/gettakaro/connectors/commit/17b4717d63809f481bbfbaf91b278c5cc3addde6))


### Continuous Integration

* **enshrouded:** boot the pinned server with the built plugin in the release workflow's verify leg ([#612](https://github.com/gettakaro/connectors/issues/612)) ([0343e7b](https://github.com/gettakaro/connectors/commit/0343e7b168ea724fd1123e82b0599b27cf082b7a))

## [0.8.1](https://github.com/gettakaro/connectors/compare/enshrouded-v0.8.0...enshrouded-v0.8.1) (2026-10-18)


### Bug Fixes

* **enshrouded:** complete plugin.json and apply protocol on save ([#513](https://github.com/gettakaro/connectors/issues/513)) ([34bd1a4](https://github.com/gettakaro/connectors/commit/34bd1a484b98b1f9a2ff11195f968e66885383ac))

## [0.8.0](https://github.com/gettakaro/connectors/compare/enshrouded-v0.7.0...enshrouded-v0.8.0) (2026-10-16)


### Features

* **enshrouded:** speak Takaro protocol 1 and declare the world map ([#488](https://github.com/gettakaro/connectors/issues/488)) ([918ce6e](https://github.com/gettakaro/connectors/commit/918ce6e76283da25f64d6e92c28fd19a0cc2e943))
* **enshrouded:** support Enshrouded 1.0 (Steam 25967645) on Proton ([#525](https://github.com/gettakaro/connectors/issues/525)) ([e194a95](https://github.com/gettakaro/connectors/commit/e194a9572e0899ec05a782101bc2ad1d0e5b6214))


### Bug Fixes

* **enshrouded:** keep the config in one file the connector creates ([#464](https://github.com/gettakaro/connectors/issues/464)) ([27cb11a](https://github.com/gettakaro/connectors/commit/27cb11a248b6d9ab37d72a44fa933b80d9b1f31d))

## [0.7.0](https://github.com/gettakaro/connectors/compare/enshrouded-v0.6.1...enshrouded-v0.7.0) (2026-10-11)


### Features

* **enshrouded:** ship takaro/plugin.json and apply token changes without a restart ([#440](https://github.com/gettakaro/connectors/issues/440)) ([4a4f628](https://github.com/gettakaro/connectors/commit/4a4f6281b53ae1062df2d92294ed006507d0a4e3))

## [0.6.1](https://github.com/gettakaro/connectors/compare/enshrouded-v0.6.0...enshrouded-v0.6.1) (2026-10-07)


### Bug Fixes

* **enshrouded:** only send a real SteamID64 as steamId ([#371](https://github.com/gettakaro/connectors/issues/371)) ([577c407](https://github.com/gettakaro/connectors/commit/577c407b412b2c7008a77a4acb64be597b42a9b5))

## [0.6.0](https://github.com/gettakaro/connectors/compare/enshrouded-v0.5.0...enshrouded-v0.6.0) (2026-10-05)


### Features

* **enshrouded:** connect the plugin directly to Takaro and drop the sidecar ([#340](https://github.com/gettakaro/connectors/issues/340)) ([90c249f](https://github.com/gettakaro/connectors/commit/90c249f1cc03ee6c2f9181d0705cc82cf5b35eb5))

## [0.5.0](https://github.com/gettakaro/connectors/compare/enshrouded-v0.4.2...enshrouded-v0.5.0) (2026-09-22)


### Features

* **maintenance:** exact Steam targets for the remaining connectors — [#148](https://github.com/gettakaro/connectors/issues/148) phase 2 ([#249](https://github.com/gettakaro/connectors/issues/249)) ([7e5481b](https://github.com/gettakaro/connectors/commit/7e5481be878f4617297afaba121f136fea544c94))


### Documentation

* make connector READMEs the source of takaro.io game docs ([#218](https://github.com/gettakaro/connectors/issues/218)) ([b45d6cd](https://github.com/gettakaro/connectors/commit/b45d6cd682a61b11d485b1724e79b1e0b61e5305))

## [0.4.2](https://github.com/gettakaro/connectors/compare/enshrouded-v0.4.1...enshrouded-v0.4.2) (2026-09-16)


### Code Refactoring

* move connectors under games/&lt;game&gt;/ with a dedicated mod/ folder per game ([#112](https://github.com/gettakaro/connectors/issues/112)) ([cd949b7](https://github.com/gettakaro/connectors/commit/cd949b7f3477fa0fc2f313bd1f8e75f2860f15c2))


### Documentation

* **enshrouded:** README with install steps and current status only ([#123](https://github.com/gettakaro/connectors/issues/123)) ([b66dbe9](https://github.com/gettakaro/connectors/commit/b66dbe990587f32b7f19dd75bb2e99429a9a5b8b))


### Continuous Integration

* **enshrouded:** publish plugin and sidecar release assets ([#144](https://github.com/gettakaro/connectors/issues/144)) ([de07af8](https://github.com/gettakaro/connectors/commit/de07af87da1012eb8a8d839a61a4364fc1298281))
