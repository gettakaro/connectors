# Changelog

## [0.7.1](https://github.com/gettakaro/connectors/compare/7d2d-v0.7.0...7d2d-v0.7.1) (2026-10-31)


### Documentation

* one README layout for every connector ([#703](https://github.com/gettakaro/connectors/issues/703)) ([1b4955d](https://github.com/gettakaro/connectors/commit/1b4955d01385777ea8db589c52adf5eec20173f9))

## [0.7.0](https://github.com/gettakaro/connectors/compare/7d2d-v0.6.0...7d2d-v0.7.0) (2026-10-30)


### Features

* **7d2d:** show the configured server chat name instead of "Server" ([#671](https://github.com/gettakaro/connectors/issues/671)) ([ed75c42](https://github.com/gettakaro/connectors/commit/ed75c425385a211071775398ea3cf124fb37fba9))
* **7d2d:** support V 3.4.0 b6 (Steam build 26167630) ([#662](https://github.com/gettakaro/connectors/issues/662)) ([10e7078](https://github.com/gettakaro/connectors/commit/10e70786d67f9c5751ae4ebf2300b7a5e61ed0e5))
* **7d2d:** support V 3.4.0 b9 (Steam build 26237934) ([#676](https://github.com/gettakaro/connectors/issues/676)) ([f8517a2](https://github.com/gettakaro/connectors/commit/f8517a22f52e2a49de1897d68bc71e1b4cbf633e))

## [0.6.0](https://github.com/gettakaro/connectors/compare/7d2d-v0.5.0...7d2d-v0.6.0) (2026-10-26)


### Features

* **7d2d:** declare the world map on protocol v1 ([#591](https://github.com/gettakaro/connectors/issues/591)) ([e126a7b](https://github.com/gettakaro/connectors/commit/e126a7b1fc83911678368b1061d44448e89d7f39))
* **7d2d:** PrismaCore-compatible command foundation with rl, rs and isbloodmoon ([#604](https://github.com/gettakaro/connectors/issues/604)) ([cbb77eb](https://github.com/gettakaro/connectors/commit/cbb77ebb438622514aa9cf6e57fcf986b15d5b4e))
* **7d2d:** report Game Profile and connector identity ([#549](https://github.com/gettakaro/connectors/issues/549)) ([dae5520](https://github.com/gettakaro/connectors/commit/dae55201308cb8a9bfe5a92a38835c6c6a7b0b2d))


### Bug Fixes

* **7d2d:** include the attacker on player-vs-player deaths ([#630](https://github.com/gettakaro/connectors/issues/630)) ([6f466b0](https://github.com/gettakaro/connectors/commit/6f466b00ef8b5f6bdb1a92df90f7623b03b7a0ed))
* **7d2d:** stop shipping assemblies the game already provides ([#601](https://github.com/gettakaro/connectors/issues/601)) ([73fd69f](https://github.com/gettakaro/connectors/commit/73fd69fd626190bc70c497202f0dabbb116c2c1b))


### Documentation

* **7d2d:** bring the README to the published-docs contract ([#586](https://github.com/gettakaro/connectors/issues/586)) ([0eed6c8](https://github.com/gettakaro/connectors/commit/0eed6c839e15a7e622c177a0ed5f5e78bd59de2c))

## [0.5.0](https://github.com/gettakaro/connectors/compare/7d2d-v0.4.0...7d2d-v0.5.0) (2026-10-18)


### Features

* **7d2d:** connector migration support for takaro[#4349](https://github.com/gettakaro/connectors/issues/4349) ([#458](https://github.com/gettakaro/connectors/issues/458)) ([cc64e0a](https://github.com/gettakaro/connectors/commit/cc64e0a7590ed0e92d44d5a3542c1288d2f520e5))


### Bug Fixes

* **7d2d:** keep the config in one file the connector creates ([#462](https://github.com/gettakaro/connectors/issues/462)) ([32f0f14](https://github.com/gettakaro/connectors/commit/32f0f14a7c63fe4bf03da8386bba9f61880db193))
* **7d2d:** show the config file as the mod creates it, drop stale versions ([#505](https://github.com/gettakaro/connectors/issues/505)) ([23d9ef0](https://github.com/gettakaro/connectors/commit/23d9ef0192c8bdd1cb3e161ce91b035498eb7a5c))

## [0.4.0](https://github.com/gettakaro/connectors/compare/7d2d-v0.3.1...7d2d-v0.4.0) (2026-10-11)


### Features

* **7d2d:** ship Config.xml in Mods/Takaro and apply token changes without a restart ([#431](https://github.com/gettakaro/connectors/issues/431)) ([5e4976f](https://github.com/gettakaro/connectors/commit/5e4976f6001b6be582f2e55704ce6d1d9d565fef))

## [0.3.1](https://github.com/gettakaro/connectors/compare/7d2d-v0.3.0...7d2d-v0.3.1) (2026-10-08)


### Bug Fixes

* **7d2d:** send no player-disconnected for a client that never spawned ([#397](https://github.com/gettakaro/connectors/issues/397)) ([b9fbec3](https://github.com/gettakaro/connectors/commit/b9fbec376bf08c871c853d65ec091128c8ce0783))

## [0.3.0](https://github.com/gettakaro/connectors/compare/7d2d-v0.2.1...7d2d-v0.3.0) (2026-10-06)


### Features

* **7d2d:** support V 3.3.0 b18 (Steam build 25661908) ([#378](https://github.com/gettakaro/connectors/issues/378)) ([ab07ee5](https://github.com/gettakaro/connectors/commit/ab07ee5dd95e69aea489a08815000a6ca6173cb3))


### Bug Fixes

* **7d2d:** send steamId and epicOnlineServicesId so Takaro does not duplicate players ([#368](https://github.com/gettakaro/connectors/issues/368)) ([54de81a](https://github.com/gettakaro/connectors/commit/54de81a62dfc6046b8e9f585f3b4bdf64d1e9d28))

## [0.2.1](https://github.com/gettakaro/connectors/compare/7d2d-v0.2.0...7d2d-v0.2.1) (2026-10-01)


### Bug Fixes

* **7d2d:** inventory give, report rejected identify, no lost events on dead links, name killed entities ([#344](https://github.com/gettakaro/connectors/issues/344)) ([c9293a3](https://github.com/gettakaro/connectors/commit/c9293a35b6150b60f3d7f10fc931c5e6077206a8))

## [0.2.0](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.6...7d2d-v0.2.0) (2026-09-22)


### Features

* **maintenance:** exact Steam targets for the remaining connectors — [#148](https://github.com/gettakaro/connectors/issues/148) phase 2 ([#249](https://github.com/gettakaro/connectors/issues/249)) ([7e5481b](https://github.com/gettakaro/connectors/commit/7e5481be878f4617297afaba121f136fea544c94))


### Miscellaneous Chores

* retire the legacy per-game compose files in favour of dev-servers ([#146](https://github.com/gettakaro/connectors/issues/146)) ([1f20486](https://github.com/gettakaro/connectors/commit/1f20486e470230c3c5ebb531f5827aa94ae4ba67))


### Documentation

* make connector READMEs the source of takaro.io game docs ([#218](https://github.com/gettakaro/connectors/issues/218)) ([b45d6cd](https://github.com/gettakaro/connectors/commit/b45d6cd682a61b11d485b1724e79b1e0b61e5305))

## [0.1.6](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.5...7d2d-v0.1.6) (2026-09-15)


### Code Refactoring

* move connectors under games/&lt;game&gt;/ with a dedicated mod/ folder per game ([#112](https://github.com/gettakaro/connectors/issues/112)) ([fb03711](https://github.com/gettakaro/connectors/commit/fb03711d2bbd2cc45463f9963961adceebbee32b))

## [0.1.5](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.4...7d2d-v0.1.5) (2026-09-15)


### Documentation

* **7d2d:** README version matches the published 0.1.4 release ([#109](https://github.com/gettakaro/connectors/issues/109)) ([0ba4e5d](https://github.com/gettakaro/connectors/commit/0ba4e5dd4183b73dd3bc396f4d3501fe174330b2))
* **7d2d:** README with install steps and current status only ([#107](https://github.com/gettakaro/connectors/issues/107)) ([e1d3697](https://github.com/gettakaro/connectors/commit/e1d369796384ea451bac81cbca9ee3899156ed47))

## [0.1.4](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.3...7d2d-v0.1.4) (2026-09-15)


### Bug Fixes

* **7d2d:** chat echo, map endpoints, reconnect buffering, dead-socket detection and 0.1.6 release ([#105](https://github.com/gettakaro/connectors/issues/105)) ([a6efdaa](https://github.com/gettakaro/connectors/commit/a6efdaaf265b41569434b275ddf80158d846c819))
* **7d2d:** restore V3 connector behavior ([#88](https://github.com/gettakaro/connectors/issues/88)) ([7762ae2](https://github.com/gettakaro/connectors/commit/7762ae22d37f0ef12665879704bced5cb15ee177))


### Code Refactoring

* **7d2d:** re-architect connector around event-driven LiteDB state mirror ([#66](https://github.com/gettakaro/connectors/issues/66)) ([030c8ef](https://github.com/gettakaro/connectors/commit/030c8ef19cc339d965bb1c9071532e200e3cbec9))


### Build System

* **7d2d:** pin Assembly-CSharp to the live-proven V3.2.0 b10 build ([#106](https://github.com/gettakaro/connectors/issues/106)) ([9fcd206](https://github.com/gettakaro/connectors/commit/9fcd2062d5e325729d678c2f20c165c5dac88317))


### Documentation

* **7d2d:** document Takaro coverage in the README ([#90](https://github.com/gettakaro/connectors/issues/90)) ([252f073](https://github.com/gettakaro/connectors/commit/252f07301f203fe3e1ff4258e07335aedbd89591))

## [0.1.6](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.5...7d2d-v0.1.6) (2026-09-15)


### Bug Fixes

* **7d2d:** detect a dead-but-open socket from missing inbound traffic, and wait for the identify acknowledgement instead of Takaro's welcome frame before releasing the backlog

## [0.1.5](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.4...7d2d-v0.1.5) (2026-09-15)


### Bug Fixes

* **7d2d:** only drain the outbound backlog after the connection is confirmed, count consecutive reconnect failures, and log the real cause of a 1006

## [0.1.3](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.2...7d2d-v0.1.3) (2026-06-04)


### Miscellaneous Chores

* **deps:** update vinanrra/7dtd-server docker digest to 1e44548 ([#55](https://github.com/gettakaro/connectors/issues/55)) ([1f97697](https://github.com/gettakaro/connectors/commit/1f97697806d44158d16660d9e02062788b824283))
* **deps:** update vinanrra/7dtd-server docker digest to 2e5349e ([#63](https://github.com/gettakaro/connectors/issues/63)) ([8c91432](https://github.com/gettakaro/connectors/commit/8c9143238964e0e3849e17dca2938e0a142e5b4f))
* **deps:** update vinanrra/7dtd-server docker digest to 410e0b6 ([#61](https://github.com/gettakaro/connectors/issues/61)) ([308a873](https://github.com/gettakaro/connectors/commit/308a8734fdd04a761df8f6ebca348f27e0e9a718))
* **deps:** update vinanrra/7dtd-server docker digest to ae0aeaa ([#57](https://github.com/gettakaro/connectors/issues/57)) ([eda8f26](https://github.com/gettakaro/connectors/commit/eda8f26146f43a3b91ce4e7126839d088289f5b4))
* **deps:** update vinanrra/7dtd-server docker digest to f4f1785 ([#51](https://github.com/gettakaro/connectors/issues/51)) ([d8bdf53](https://github.com/gettakaro/connectors/commit/d8bdf53ba135a31004408a885951a3c41d1042e7))

## [0.1.2](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.1...7d2d-v0.1.2) (2026-05-30)


### Miscellaneous Chores

* **deps:** pin dependencies ([be404f6](https://github.com/gettakaro/connectors/commit/be404f652951b72ca7dee4c20933fda5f420741c))
* **deps:** update dependency csharpier to v0.30.6 ([512a37f](https://github.com/gettakaro/connectors/commit/512a37f9ee49653d6b4a4a49dc2b4b936714e378))
* **deps:** update vinanrra/7dtd-server docker digest to ceecd5a ([#41](https://github.com/gettakaro/connectors/issues/41)) ([3529c99](https://github.com/gettakaro/connectors/commit/3529c997786508e75c9fdea379c451392080d809))

## [0.1.1](https://github.com/gettakaro/connectors/compare/7d2d-v0.1.0...7d2d-v0.1.1) (2026-05-29)


### Bug Fixes

* **7d2d:** correct grammar in shutdown log message ([#12](https://github.com/gettakaro/connectors/issues/12)) ([8fa279f](https://github.com/gettakaro/connectors/commit/8fa279f4f901532674f4d5defd260f390000d407))

## 0.1.0 (2026-05-29)


### Features

* **7d2d:** add 7 Days to Die connector ([a249d15](https://github.com/gettakaro/connectors/commit/a249d157f16f583ed5f2aaf46a860c34c10c2d0b))


### Bug Fixes

* **7d2d:** retry SteamCMD and cache managed DLLs in CI ([b0c6a86](https://github.com/gettakaro/connectors/commit/b0c6a86b719cde3e7aad8b07dd9391c9d87cbce7))
* **7d2d:** unblock CI — valid csharpier pin and archive.debian.org apt sources ([8e4f8ae](https://github.com/gettakaro/connectors/commit/8e4f8ae644be652ce12fea8f9ee01d9d1b793207))
