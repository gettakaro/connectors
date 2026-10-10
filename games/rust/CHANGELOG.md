# Changelog

## [0.1.4](https://github.com/gettakaro/connectors/compare/rust-v0.1.3...rust-v0.1.4) (2026-10-10)


### Bug Fixes

* **rust:** target the 2026-10-09 Rust update build 25823813 ([#423](https://github.com/gettakaro/connectors/issues/423)) ([ee43910](https://github.com/gettakaro/connectors/commit/ee43910227ce120ed93aba91764ba329d25479af))

## [0.1.3](https://github.com/gettakaro/connectors/compare/rust-v0.1.2...rust-v0.1.3) (2026-10-07)


### Bug Fixes

* **rust:** never send NPCs as Takaro players; full identity on ban entries ([#370](https://github.com/gettakaro/connectors/issues/370)) ([faeb6fc](https://github.com/gettakaro/connectors/commit/faeb6fc95c5056b2ac041f457a0f61efee6c5058))
* **rust:** target the 2026-10-07 Rust hotfix build 25773132 ([#394](https://github.com/gettakaro/connectors/issues/394)) ([8fd4db4](https://github.com/gettakaro/connectors/commit/8fd4db4d8d119454e31b0e284212972e7e4e1e5b))

## [0.1.2](https://github.com/gettakaro/connectors/compare/rust-v0.1.1...rust-v0.1.2) (2026-10-05)


### Bug Fixes

* **rust:** target the 2026-10-02 Rust hotfix build 25681086 ([#355](https://github.com/gettakaro/connectors/issues/355)) ([79c8023](https://github.com/gettakaro/connectors/commit/79c80239506ccedf9759378b312897a70100abd9))

## [0.1.1](https://github.com/gettakaro/connectors/compare/rust-v0.1.0...rust-v0.1.1) (2026-10-01)


### Bug Fixes

* **rust:** restore timed bans, reply before shutdown, target the 2026-10-01 Rust update ([#346](https://github.com/gettakaro/connectors/issues/346)) ([4e85099](https://github.com/gettakaro/connectors/commit/4e85099670420318da88e75892f2e01e5f8d5513))

## [0.1.0](https://github.com/gettakaro/connectors/compare/rust-v0.0.5...rust-v0.1.0) (2026-09-22)


### Features

* **maintenance:** exact Steam targets for the remaining connectors — [#148](https://github.com/gettakaro/connectors/issues/148) phase 2 ([#249](https://github.com/gettakaro/connectors/issues/249)) ([7e5481b](https://github.com/gettakaro/connectors/commit/7e5481be878f4617297afaba121f136fea544c94))


### Miscellaneous Chores

* retire the legacy per-game compose files in favour of dev-servers ([#146](https://github.com/gettakaro/connectors/issues/146)) ([1f20486](https://github.com/gettakaro/connectors/commit/1f20486e470230c3c5ebb531f5827aa94ae4ba67))


### Documentation

* make connector READMEs the source of takaro.io game docs ([#218](https://github.com/gettakaro/connectors/issues/218)) ([b45d6cd](https://github.com/gettakaro/connectors/commit/b45d6cd682a61b11d485b1724e79b1e0b61e5305))

## [0.0.5](https://github.com/gettakaro/connectors/compare/rust-v0.0.4...rust-v0.0.5) (2026-09-16)


### Miscellaneous Chores

* **main:** release rust 0.0.4 ([#132](https://github.com/gettakaro/connectors/issues/132)) ([ae0b3ec](https://github.com/gettakaro/connectors/commit/ae0b3ec001f6983539d94eb9c28b91db5dd1ee11))
* **main:** release rust 0.0.4 ([#133](https://github.com/gettakaro/connectors/issues/133)) ([1f39b3d](https://github.com/gettakaro/connectors/commit/1f39b3dd0b968b62a42e659aa61ef7ac3359b52a))
* **rust:** repair release manifest and changelog after duplicate 0.0.4 release PR ([#134](https://github.com/gettakaro/connectors/issues/134)) ([d158c4a](https://github.com/gettakaro/connectors/commit/d158c4aca21d7ca16dd6df2916437e50b5254f7d))

## [0.0.4](https://github.com/gettakaro/connectors/compare/rust-v0.0.3...rust-v0.0.4) (2026-09-15)


### Bug Fixes

* **rust:** honour ban expiry and report it in listBans ([#128](https://github.com/gettakaro/connectors/issues/128)) ([bdcd411](https://github.com/gettakaro/connectors/commit/bdcd4114594c0e7fdffc79474a97b743ae5db118))


### Code Refactoring

* move connectors under games/&lt;game&gt;/ with a dedicated mod/ folder per game ([#112](https://github.com/gettakaro/connectors/issues/112)) ([fb03711](https://github.com/gettakaro/connectors/commit/fb03711d2bbd2cc45463f9963961adceebbee32b))


### Documentation

* **rust:** README with install steps and current status only ([#120](https://github.com/gettakaro/connectors/issues/120)) ([90c1428](https://github.com/gettakaro/connectors/commit/90c1428f7b1ee3692241bc04ef42669311549cbb))

## [0.0.3](https://github.com/gettakaro/connectors/compare/rust-v0.0.2...rust-v0.0.3) (2026-05-30)


### Miscellaneous Chores

* **deps:** pin dependencies ([be404f6](https://github.com/gettakaro/connectors/commit/be404f652951b72ca7dee4c20933fda5f420741c))
* **deps:** update ubuntu docker tag to v26 ([#45](https://github.com/gettakaro/connectors/issues/45)) ([4ffc306](https://github.com/gettakaro/connectors/commit/4ffc3066cccd13f90716dcfecfc2f8489a9f6bdb))


### Continuous Integration

* automate connector releases with conventional commits ([#4](https://github.com/gettakaro/connectors/issues/4)) ([69e1a43](https://github.com/gettakaro/connectors/commit/69e1a4320ebf2d90f77746c05b619e9b77d014c8))
