# Takaro 7D2D Mod — Development

Notes for working on the mod itself. Server operators only need
[`README.md`](README.md).

## Quick Start

From the monorepo root:

```sh
just sevend2d-setup
just sevend2d-build
just sevend2d-build-deploy
```

Or from inside `7d2d/`:

```sh
./scripts/setup-environment.sh
./scripts/build-mod.sh
./scripts/build-mod.sh deploy
```

Local server files and build outputs live under `7d2d/_data/`. The packaged
release zip (`takaro-7d2d-mod.zip`, containing a single `Takaro/` folder) is
produced by `./scripts/build-release.sh <version> <out-dir>`, which is also what
CI runs in the `package` job of `.github/workflows/7d2d.yml`.

## Architecture

The mod mirrors game state into an in-memory LiteDB database so that Takaro
read requests never touch the game simulation:

- Game events (join, disconnect, player data saves) and a ~3s position sampler
  update the mirror from the game main thread; DB writes happen on a dedicated
  writer thread.
- Read requests (`getPlayers`, `getPlayerLocation`, `listItems`, `listBans`, …)
  are answered from LiteDB on the WebSocket thread.
- Action requests (`giveItem`, `teleportPlayer`, `banPlayer`, …) are marshalled
  onto the game main thread via a dispatcher and awaited asynchronously.

It is server-side only and connects to Takaro through the Generic Connector
Protocol over an outbound WebSocket. See
[`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md) for the per-endpoint data
flow, the state-mirror diagram and the staleness bounds.

## Configuration

`Config.xml` is created by the mod on first start at `<server>/Takaro/Config.xml`
(that is `Directory.GetCurrentDirectory() + "/Takaro"`, i.e. the server's working
directory — *not* inside `Mods/`). It is seeded with the production Takaro
WebSocket URL:

```xml
<Url>wss://connect.takaro.io/</Url>
```

`RegistrationToken` must be set to the token from the Takaro game server
connector setup before the server can identify successfully. `IdentityToken` is
generated automatically the first time the config is created.

The mod writes its own log to `<server>/Takaro/logs/<M-D-YYYY>.log`. On the dev
rig, DEBUG logging is toggled with the `takaro-debug` console command (it resets
to off on every server restart).

## Versioning

release-please owns `7d2d/ModInfo.xml` and `7d2d/version.txt` — do not bump them in
a feature branch. Whatever version a PR branch carries is overwritten by the next
release PR, which is why the 2026-09-15 hardening work shipped as release
`7d2d-v0.1.4` even though the branch was labelled 0.1.6.
