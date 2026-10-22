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

## Runtime verification (CI)

`takaro-maint verify --game 7d2d` installs a target's exact depot set into a throwaway
directory, refuses it unless the declared files (`7DaysToDieServer.x86_64`, both
`Assembly-CSharp` assemblies and Harmony) match the record's sha256, unpacks the built mod
zip into `Mods/Takaro/` after checking it carries the target's fingerprint, and boots the
pinned server image against a local fake Takaro. No host port is published.

| Check | What has to happen |
|---|---|
| `build` | The manifest's artifact carries this target's fingerprint. |
| `startup` | The server logs `StartGame done` and the pinned files are still byte-identical. |
| `handshake` | The mod is loaded (`Loaded Mod: Takaro (<version>)`), sends `identify`, and logs Takaro's `identifyResponse`. |
| `heartbeat`, `players` | Websocket ping/pong and `testReachability` answer; `getPlayers` on the empty server is `[]`. |
| `console` | `executeConsoleCommand` runs on the server and is echoed in its log. |
| `action` | `sendMessage` reaches the server's chat log. |
| `reconnect` | Takaro drops the socket (1001); the mod identifies again within 120 s and answers on the new socket. |
| `stop` | `shutdown` makes the server log its quit, the container exits 0 and the pinned files are intact. |

The release workflow (`.github/workflows/7d2d.yml` → `connector-release.yml`) runs it for
every candidate and maintained target in the `release / verify (<target>)` jobs. They run
on every push to `main`, on release and recovery runs, and on pull requests that change this
connector's build inputs (`scripts/pr-build-scope.sh`). `publish` needs every one of them to
pass.

Runner requirements: `ubuntu-latest` (x86_64, 4 vCPU, 16 GB RAM) with docker. The server
container is capped at 6 GB. The depot set is 17.7 GB, and it is on disk twice: once in
`~/.cache/takaro-maint/steam` and once in the throwaway install. That is more than the hosted
image has free, so the leg first removes SDKs it never uses (`runtime_large_server`). The
depot set is also larger than the repository's whole Actions cache, so it is downloaded from
Steam on each run: anonymously, no credentials. Trimming it does not help either, because the
server hangs in `GameManager.Awake` without the asset bundles under `Data/Addressables`. The
leg's budget is 90 minutes and the server's startup budget is 900 s. The run's containers
carry `tm.run=<run id>` and are removed on success, failure, `SIGINT`/`SIGTERM`, and by the
job's last step if the job is cancelled.

Reproduce it locally (about 36 GB free disk and 8 GB free RAM):

```sh
maintenance/bin/takaro-maint build --game 7d2d --target linux-3.3.0.b18 \
  --version 0.0.0-local --out dist --toolchain container
maintenance/bin/takaro-maint verify --game 7d2d --target linux-3.3.0.b18 \
  --artifacts dist --out reports --startup-timeout 900
```

`reports/<target>/report.json` ties every check to the source revision, the target
fingerprint, the artifact sha256 and the image digest that booted, with `server.log`,
`fake-takaro.log`, `docker.log`, `install.json` and `deploy.json` beside it.

Still manual, because they need a real game client (see the target record's evidence):
`player-connected`/`player-disconnected`, `chat-message` from a player, `player-death`,
`entity-killed`, `command-executed`, `getPlayer`/`getPlayerLocation`/`getPlayerInventory`,
`giveItem`, `teleportPlayer`, `kickPlayer`, `banPlayer`/`unbanPlayer`/`listBans` with a
rejoin, the Discord bridge, and registration against a hosted Takaro over TLS.

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
flow, the state-mirror diagram and the staleness bounds. Its "Protocol" section
describes what the identify handshake declares (protocol versions, Capability
Manifest, game identifier, Connector Migration support) and how events are
numbered and acknowledged. A new action must be added to
`RequestRouter.SupportedActions` and to `ProtocolHandshake.ActionCapabilities`
(and the harness fixture) so the manifest keeps matching what the mod answers.

## Configuration

One file: `<server cwd>/Takaro/Config.xml` (`API.BasePath`, next to `Mods/`). The release
zip does not contain it, so replacing `Mods/Takaro/` on an upgrade never touches it. The mod
creates it in `InitMod` (before the world loads) when it is missing, with every key, an empty
`RegistrationToken` and a new UUID `IdentityToken` (mode 0600 on Linux); when the file has no
identity, the mod generates one and writes it back (atomically, keeping the file's comments and permissions).
Nothing else is ever written. A failed write keeps the generated identity in memory and is
retried. There are no environment variables. The dev rig (`dev-servers/lib/render.sh`) and
`takaro-maint verify` render this same file.

The file is read every 5 s (it is tiny, and comparing the text cannot miss an edit); a change
counts once the text has been stable for 1 s, and a half-saved file (invalid XML) is ignored
until it changes again. A change to the URL, a token, the identity or `Enabled` drops the
socket and reconnects at once, so a pasted or corrected token needs no restart. Without a
registration token the mod does not connect (emptying it disconnects); it logs a banner naming
the file instead. `src/Config/ConfigFiles.cs` has no game types and is covered by the
contract harness.

The mod writes its own log to `<server>/Takaro/logs/<M-D-YYYY>.log`. On the dev
rig, DEBUG logging is toggled with the `takaro-debug` console command (it resets
to off on every server restart).

## Versioning

release-please owns `games/7d2d/mod/ModInfo.xml` and `games/7d2d/version.txt` — do not bump them in
a feature branch. Whatever version a PR branch carries is overwritten by the next
release PR, which is why the 2026-09-15 hardening work shipped as release
`7d2d-v0.1.4` even though the branch was labelled 0.1.6.
