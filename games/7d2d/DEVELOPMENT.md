# Takaro 7D2D Mod — Development

Notes for working on the mod itself. Server operators only need
[`README.md`](README.md).

## The target

Everything here is built against one catalog target at a time. The default is
[`catalog/7d2d/targets/linux-3.3.0.b18.json`](../../catalog/7d2d/targets/linux-3.3.0.b18.json)
(V 3.3.0 b18); [`linux-3.2.0.b10.json`](../../catalog/7d2d/targets/linux-3.2.0.b10.json) is still
built. A target records the Steam app, branch, build id and depot manifest of its build, the sha256 of the
assemblies the mod compiles against, the Mono image the build runs in, the pinned third-party
dependencies and the server image the rig boots. No script here hard-codes any of it —
`scripts/lib-target.sh` resolves the target and exports `SEVEND2D_*` for the rest.

Each catalog target builds its own artifact from the same sources. Where the game's API changed
between builds, `sevend2d_api_defines` in `scripts/lib-target.sh` turns the target's revision into
a compile-time symbol (`SEVEND2D_V3_3` for V 3.3.0 and later), which `build-mod.sh` and
`test-contract.sh` pass to the compiler. V 3.3.0 keeps player inventories in serialised blobs and
replaced `ItemValue.Modifications` with accessors, so `StateMirror` and `GiveItemHandler` have one
branch per API generation.

There is no SteamCMD anywhere in this directory. `takaro-maint steam references` downloads only
the assemblies the build needs, straight from the pinned depot manifests. See
[maintenance/docs/steam-install.md](../../maintenance/docs/steam-install.md).

## Quick start

From the monorepo root:

```sh
just sevend2d-setup          # reference assemblies + dependencies for the default target
just sevend2d-build          # compile the mod
just sevend2d-build-deploy   # build and deploy into the dev rig
just sevend2d-test-contract  # the Generic Connector contract harness
```

Or from inside `games/7d2d/`:

```sh
./scripts/setup-environment.sh --target linux-3.3.0.b18
./scripts/build-mod.sh --target linux-3.3.0.b18
./scripts/build-release.sh <version> <out-dir> --target linux-3.3.0.b18
```

`--target` may be left out everywhere except `build-release.sh`, which insists on it: a release
artifact is named after the build it was made for, so it may not be built for "whatever the
default is today".

What lands where:

| Path | What |
|---|---|
| `_data/7dtd-binaries/<fingerprint>/` | The target's reference assemblies and the built dependencies, plus `.takaro/references.json` and `.takaro/deps.json`. |
| `_data/build/` | msbuild output (`Mods/Takaro`) and the staged folder the release zip is made from. |
| `_data/dist/<fingerprint>/` | `takaro-7d2d-mod-<target>-<version>.zip` and its `.meta.json`. |

A different target has a different fingerprint and therefore its own directories; nothing is
shared between builds of different server versions.

## The rig

`dev-servers/scripts/install.sh 7d2d` installs the exact pinned build into
`dev-servers/_data/7d2d/ServerFiles` through `takaro-maint install`, records it in
`ServerFiles/.takaro/installed-target.json` and renders `Takaro/Config.xml`. The compose file
pins the server image by digest and runs it with `START_MODE=1` — start what is installed. The
install writes `DONT_REMOVE.txt`, which is what stops the image's own LinuxGSM auto-install from
replacing the pinned build with the current branch head.

`dev-servers/scripts/deploy-connector.sh 7d2d` builds the target's artifact and deploys it by
manifest row, so the mod in `Mods/Takaro` is the one the ledger records.

## Moving to a new server build

1. `just sevend2d-pin` — what does Steam serve on `public` now? `changed` lists the depots that
   moved.
2. `takaro-maint steam pin --game 7d2d --buildid <new> --record-files <each declared file> --write`
   — re-record the hashes from the new manifest and write the new pin.
3. `./scripts/setup-environment.sh` and `./scripts/test-contract.sh` — the contract harness must
   pass against the new assemblies.
4. Re-prove the behaviour that matters on the rig, update the table in `README.md`, and open a PR.
   The target id, its fingerprint and every artifact name change with the build, which is the
   point: nothing claims to have been proven on bytes it was not proven on.

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

The mod reads two files (`src/Config/ConfigFiles.cs`):

- `Mods/Takaro/Config.xml` ships in the release zip (`mod/Config.xml`) with an empty
  `RegistrationToken` and `IdentityToken`. This is the file people edit.
- `<server>/Takaro/Config.xml` (the server's working directory, *not* inside `Mods/`) is
  where releases up to 0.3 kept everything. The mod now writes the settings it uses there,
  so deleting `Mods/Takaro/` on an upgrade keeps the token and the identity. The dev rig
  (`dev-servers/lib/render.sh`) and `takaro-maint verify` still render this file, and it
  works because an empty field in the mod config falls back to it.

The tokens come from the mod config when set there, else from the saved one. `Url`,
`Enabled` and `ReconnectIntervalSeconds` come from the mod config only when it changes them
from the default (the shipped file spells the defaults out, and an upgrade must not let them
override a saved custom value), else from the saved one, else the default. The saved file
gets the tokens in use but keeps its own other settings, so setting something back to the
default in the mod config takes effect. An identity neither file holds is generated and
written to both; if those writes fail, the mod keeps using the identity it already announced
and retries. The mod never copies the registration token into the mod config, so a token
rendered into the saved file is not shadowed by a stale copy.

Both files are read every 5 s (they are tiny, and comparing the text cannot miss an edit). A change to the URL, a token or `Enabled` drops the socket
and reconnects at once, so a pasted or corrected token needs no restart. Without a
registration token the mod does not connect; it logs a banner naming the file instead.

The mod writes its own log to `<server>/Takaro/logs/<M-D-YYYY>.log`. On the dev
rig, DEBUG logging is toggled with the `takaro-debug` console command (it resets
to off on every server restart).

## Versioning

release-please owns `games/7d2d/mod/ModInfo.xml` and `games/7d2d/version.txt` — do not bump them in
a feature branch. Whatever version a PR branch carries is overwritten by the next
release PR, which is why the 2026-09-15 hardening work shipped as release
`7d2d-v0.1.4` even though the branch was labelled 0.1.6.
