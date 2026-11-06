# Changelog

## [0.8.0](https://github.com/gettakaro/connectors/compare/terraria-v0.7.0...terraria-v0.8.0) (2026-10-31)


### Features

* **terraria:** show the configured server chat name instead of "Server" ([#699](https://github.com/gettakaro/connectors/issues/699)) ([70b9631](https://github.com/gettakaro/connectors/commit/70b96310e9461960641fe7165d0ec5fb97af69eb))


### Bug Fixes

* **terraria:** deliver giveItem to players on servers without SSC ([#725](https://github.com/gettakaro/connectors/issues/725)) ([7bd314f](https://github.com/gettakaro/connectors/commit/7bd314f76704ccf4fe8e8aba891d5527c71a8507))


### Documentation

* one README layout for every connector ([#703](https://github.com/gettakaro/connectors/issues/703)) ([1b4955d](https://github.com/gettakaro/connectors/commit/1b4955d01385777ea8db589c52adf5eec20173f9))

## [0.7.0](https://github.com/gettakaro/connectors/compare/terraria-v0.6.0...terraria-v0.7.0) (2026-10-26)


### Features

* **terraria:** report the game profile and connector identity to Takaro ([#555](https://github.com/gettakaro/connectors/issues/555)) ([7731664](https://github.com/gettakaro/connectors/commit/77316643eae1e5ecaac2bf7604ae125d8de9df39))


### Bug Fixes

* **terraria:** name the map Dimension and drop the fake (0, 0) in getPlayerLocation ([#623](https://github.com/gettakaro/connectors/issues/623)) ([6925629](https://github.com/gettakaro/connectors/commit/69256294597e09153cfb45b0e5d9c451fba2a0fc))
* **terraria:** report a leave from TShock's disconnect line ([#643](https://github.com/gettakaro/connectors/issues/643)) ([49220c9](https://github.com/gettakaro/connectors/commit/49220c96933a5b25334a04320c48f4388c6dfe6f))


### Documentation

* **terraria:** re-verify the README on TShock 6.2.1, keep timed bans timed and credit kills correctly ([#596](https://github.com/gettakaro/connectors/issues/596)) ([6c6d64d](https://github.com/gettakaro/connectors/commit/6c6d64d3236b8b2312ed3c825a24a5490c168483))


### Continuous Integration

* **terraria:** boot the pinned TShock server with the built plugin and bridge in the verify leg ([#611](https://github.com/gettakaro/connectors/issues/611)) ([a52152a](https://github.com/gettakaro/connectors/commit/a52152a78ad79836f1f4c06b351f504f728be810))

## [0.6.0](https://github.com/gettakaro/connectors/compare/terraria-v0.5.0...terraria-v0.6.0) (2026-10-18)


### Features

* **terraria:** world map on Takaro via protocol v1 ([#483](https://github.com/gettakaro/connectors/issues/483)) ([43b85e8](https://github.com/gettakaro/connectors/commit/43b85e8640be155b684d8684727206044a538443))


### Bug Fixes

* **terraria:** keep the config in one file the connector creates ([#461](https://github.com/gettakaro/connectors/issues/461)) ([30c5df6](https://github.com/gettakaro/connectors/commit/30c5df6a5d47fe2fee3eace3117bb0264fb10f3d))
* **terraria:** resolve mapDir and logFiles from the config folder, document the map ([#503](https://github.com/gettakaro/connectors/issues/503)) ([5bce9d5](https://github.com/gettakaro/connectors/commit/5bce9d5bff427fa2008edeff655dcd2616552794))

## [0.5.0](https://github.com/gettakaro/connectors/compare/terraria-v0.4.1...terraria-v0.5.0) (2026-10-11)


### Features

* **terraria:** ship TakaroConfig.txt in the bridge zip, keep settings across upgrades and apply token changes without a restart ([#433](https://github.com/gettakaro/connectors/issues/433)) ([1cce164](https://github.com/gettakaro/connectors/commit/1cce16407b6b5d34a619b28d0d349508f262e7fe))

## [0.4.1](https://github.com/gettakaro/connectors/compare/terraria-v0.4.0...terraria-v0.4.1) (2026-10-07)


### Bug Fixes

* **terraria:** send one Takaro-valid player identity on every path ([#374](https://github.com/gettakaro/connectors/issues/374)) ([34c3de9](https://github.com/gettakaro/connectors/commit/34c3de955e18d707b708d3c4eae8b64cfc762798))

## [0.4.0](https://github.com/gettakaro/connectors/compare/terraria-v0.3.0...terraria-v0.4.0) (2026-10-05)


### Features

* **terraria:** add TShock v6.2.1 target ([#322](https://github.com/gettakaro/connectors/issues/322)) ([42304e8](https://github.com/gettakaro/connectors/commit/42304e8e89d3f8a07b1452f940df0845cc9b8055))

## [0.3.0](https://github.com/gettakaro/connectors/compare/terraria-v0.2.1...terraria-v0.3.0) (2026-09-22)


### Features

* **maintenance:** exact Steam targets for the remaining connectors — [#148](https://github.com/gettakaro/connectors/issues/148) phase 2 ([#249](https://github.com/gettakaro/connectors/issues/249)) ([7e5481b](https://github.com/gettakaro/connectors/commit/7e5481be878f4617297afaba121f136fea544c94))


### Documentation

* make connector READMEs the source of takaro.io game docs ([#218](https://github.com/gettakaro/connectors/issues/218)) ([b45d6cd](https://github.com/gettakaro/connectors/commit/b45d6cd682a61b11d485b1724e79b1e0b61e5305))

## [0.2.1](https://github.com/gettakaro/connectors/compare/terraria-v0.2.0...terraria-v0.2.1) (2026-09-16)


### Code Refactoring

* move connectors under games/&lt;game&gt;/ with a dedicated mod/ folder per game ([#112](https://github.com/gettakaro/connectors/issues/112)) ([fb03711](https://github.com/gettakaro/connectors/commit/fb03711d2bbd2cc45463f9963961adceebbee32b))


### Documentation

* **terraria:** README with install steps and current status only ([#122](https://github.com/gettakaro/connectors/issues/122)) ([85c2132](https://github.com/gettakaro/connectors/commit/85c2132db9540a1b56bf0b716fe25ecb093602b3))


### Continuous Integration

* **terraria:** upload release assets on tagged releases ([#126](https://github.com/gettakaro/connectors/issues/126)) ([1ba29bb](https://github.com/gettakaro/connectors/commit/1ba29bb20831a5606cac621c330deff61e4fa44e))

## [0.2.0](https://github.com/gettakaro/connectors/compare/terraria-v0.1.0...terraria-v0.2.0) (2026-09-02)


### Features

* **terraria:** add Takaro bridge sidecar with inventory, kill weapon, and resilience fixes ([#89](https://github.com/gettakaro/connectors/issues/89)) ([79a84a7](https://github.com/gettakaro/connectors/commit/79a84a71f9f5257164c8132e31add7ebb40f6544))
* **terraria:** add TShock events plugin ([#75](https://github.com/gettakaro/connectors/issues/75)) ([f2f9104](https://github.com/gettakaro/connectors/commit/f2f910471a4eef8075e39222435f032bbf5c1d64))


### Bug Fixes

* **terraria:** enforce bans on the player UUID, not their display name ([#92](https://github.com/gettakaro/connectors/issues/92)) ([cb97b6b](https://github.com/gettakaro/connectors/commit/cb97b6b7c59b5ae218c6628370de303cbb5a25f8))
* **terraria:** name the killer on player-death and stop re-ingesting the bridge's own log lines ([#93](https://github.com/gettakaro/connectors/issues/93)) ([d3a721f](https://github.com/gettakaro/connectors/commit/d3a721f4bd4fbfd78fc3b96c348353e7f55b5d50))

## Changelog
