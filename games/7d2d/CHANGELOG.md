# Changelog

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
