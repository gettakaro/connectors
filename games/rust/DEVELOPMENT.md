# Rust Connector — Development

Developer notes for the Takaro Rust connector. Operators want [README.md](README.md) instead.

## Architecture

The plugin implements the [Takaro Generic Connector Protocol](https://docs.takaro.io/advanced/adding-support-for-a-new-game).
It runs inside the Rust server via the [Carbon](https://carbonmod.gg/) mod framework (it is written
against the Oxide/uMod plugin API, so Oxide loads it too) and connects outbound to Takaro over a
WebSocket. No port forwarding required.

```
[Takaro Backend]  <->  [TakaroConnector.cs]  <->  [Rust Server + Carbon]
                 WebSocket (outbound)         Direct C# game API access
```

Everything lives in one file: `mod/TakaroConnector.cs`.

## Features

- All 17 Takaro actions: `testReachability`, `getPlayer`, `getPlayers`, `getPlayerLocation`,
  `getPlayerInventory`, `listItems`, `listEntities`, `listLocations`, `executeConsoleCommand`,
  `sendMessage`, `giveItem`, `teleportPlayer`, `kickPlayer`, `banPlayer`, `unbanPlayer`,
  `listBans`, `shutdown`.
- 6 game events: `player-connected`, `player-disconnected`, `chat-message`, `player-death`,
  `entity-killed`, `log`.
- WebSocket with automatic reconnection and exponential backoff (5 s initial, 1.5x, 5 min cap).
- Hot-reload support via Carbon (no server restart needed).

## Target and build

This connector is built and verified against one **catalog target**, not against "whatever Steam
and Carbon serve today". The record is `catalog/rust/targets/carbon-25454815.json`, and it pins:

| Half | Pinned by |
|---|---|
| The server | Steam app 258550, branch `public`, build id 25454815, os `linux`, and the **manifest id of each of the two depots** (258552 linux binaries, 258554 shared content) — plus a self-recorded sha256 for seven declared files |
| Carbon | The GitHub release asset `Carbon.Linux.Release.tar.gz` by **sha256 and size**. Carbon's `production_build` tag is re-uploaded in place, so the tag is an address, never an identity |
| The runtime | `mcr.microsoft.com/dotnet/runtime-deps:8.0.31-noble` by index digest |
| The toolchain | `mcr.microsoft.com/dotnet/sdk:8.0.425-noble` by index digest |
| The build dependency | `BepInEx.AssemblyPublicizer.MSBuild` 0.4.2 by nupkg sha256, restored with `--locked-mode` against a committed `packages.lock.json` |

Everything tracked reads that record through `takaro-maint targets resolve`:

```bash
maintenance/bin/takaro-maint targets resolve --game rust --format env --prefix RUST
```

`scripts/lib-target.sh` is the shell side of that: every script here sources it, so no script,
compose file or workflow holds a game build, a Carbon release, an image digest or an artifact name.

### The caches

Each is keyed by the target's **fingerprint** (`<fp16>`), so a re-pinned target never compiles
against the previous target's bytes, and a directory left over from another fingerprint is refused
rather than reused (exit 7; `--force` replaces it).

| Directory | Holds |
|---|---|
| `_data/rust-binaries/<fp16>` | the `RustDedicated_Data/Managed/*.dll` reference assemblies, fetched from the pinned depot manifests by `takaro-maint steam references` — a filelist, never the whole 5.9 GB depot set |
| `_data/carbon-refs/<fp16>` | `carbon/managed/*.dll` out of the verified Carbon archive |
| `_data/nuget` | the restored publicizer package, hash-checked after the restore |
| `_data/compile/<fp16>` | the generated compile project |
| `_data/dist/<fp16>` | the staged artifact |

CI keys its cache `rust-refs-<fp16>` for the same reason.

### The scripts

```bash
./scripts/setup-environment.sh [--target ID] [--force]   # pinned game + Carbon reference assemblies
./scripts/compile-check.sh     [--target ID]             # compile the plugin in the pinned SDK image
./scripts/build-release.sh <version> <out-dir> [--target ID]
```

`compile-check.sh` is **the authoritative build for the fingerprint**. The released artifact is
source — Carbon compiles it when it loads it — so nothing else proves it compiles at all. It
generates one `<Reference>` per pinned assembly (the game's are publicized, because most of Rust's
plugin API is `internal`) and runs `dotnet build -c Release` inside the pinned SDK container.

`build-release.sh` runs both of the above, then stamps the version into the `[Info(...)]` attribute,
prepends a two-line identity header (target, fingerprint, source revision, Carbon digest, Rust build
and depot manifests, `Assembly-CSharp.dll` sha256 — no timestamps, so two builds of one commit are
byte-identical) and writes `takaro-rust-plugin-carbon-25454815-<version>.cs` plus a `.meta.json`
(which target these bytes belong to, for `artifact validate`) and a `.provenance.json` (the
Carbon pin, the depot manifests, the `Assembly-CSharp.dll` sha256 and the toolchain digest this
build used). They are two documents because `takaro-maint build --out DIR` writes its own
generic sidecar under the `.meta.json` name when it copies the artifact into a release
directory; the adapter carries the `.provenance.json` there alongside it.

**`[Info]` only takes three integers.** Oxide's `VersionNumber`, which Carbon uses to read the
attribute, parses the version as `int.int.int`, and it throws inside the attribute's constructor on
anything else — which Carbon reports as `Invalid plugin format in 'TakaroConnector.cs'. Namespace
must be Carbon|Oxide.Plugins …`, a message about something that is not wrong. So a dev or PR version
(`0.0.5-dev.abc1234`) is reduced to its numeric head (`0.0.5`) for the attribute, and the exact build
is carried by the artifact's file name, its identity header and its `.provenance.json` — none of which any
framework parses. `verify`'s `carbon-compile` check reduces the recorded connector version the same
way before comparing it with the version the server logged.

### Install, deploy, verify

```bash
maintenance/bin/takaro-maint install --game rust --dest <dir>     # the two depots + Carbon
maintenance/bin/takaro-maint build   --game rust --version V --out dist
maintenance/bin/takaro-maint deploy  --game rust --dest <dir> --from dist/build-manifest.json
maintenance/bin/takaro-maint verify  --game rust --artifacts dist --out reports
```

`install` is the adapter's own, because this game's installation has two halves. The Steam depots go
through the shared exact-install path; Carbon is fetched into the **staging** directory and verified
there, so a missing asset (exit 4) or altered bytes (exit 5) leave an existing install
byte-identical and leave no staging directory behind. Afterwards the ledger records the Carbon
archive and four witnesses (`carbon/managed/Carbon.dll`, `carbon/managed/Carbon.Common.dll`,
`libdoorstop.so`, `carbon/tools/environment.sh`), so `ledger check` guards the framework half too,
and `already-installed` is only answered once those rows still hash as recorded.

`server/`, `carbon/plugins/`, `carbon/configs/`, `carbon/data/`, `carbon/logs/`, `carbon/config.json`,
`takaro/` and `.takaro/` are preserved across a reinstall; the previous install is kept beside the
new one and `install --rollback` puts it back.

`deploy` copies the versioned artifact to `carbon/plugins/TakaroConnector.cs`, because Carbon and
Oxide both load a plugin by its class-named file. The versioned file in `takaro/` stays as the
ledger's record of what was deployed.

### Carbon's self-updater

Carbon replaces its own `carbon/managed/*.dll` on boot as soon as a newer production build exists,
which would silently unpin the framework half of the target. The install seeds `carbon/config.json`
with self-updating disabled — only when Carbon has not written that file yet, never over an
operator's own — and a good boot answers `Skipped self-updating process as it's disabled in the
config.`

The seed has to be in **Carbon's own shape**: `SelfUpdating` is an object
(`{"Enabled": false, "HookUpdates": false, "RedirectUri": null}`), not a flag. This is worth knowing
because of how it fails: Carbon's preloader reads this file before it logs anything, so a document it
cannot deserialise takes the entire framework down without a word — the server then boots perfectly,
answers RCON, and is simply unmodded.

The hard guard is the ledger: `verify`'s `startup` check re-hashes the declared
game files and the four Carbon witnesses after the boot and fails if anything moved
(`startup.detail.inputsIntact`).

## The runtime container

There is **no Dockerfile**. No public Rust-server image carries an immutable tag and every one of
them runs a Steam update on start, so the runtime container is the digest-pinned base image from the
record plus two mounts: the installed tree at `/rust` and the tracked launcher `start.sh` at
`/takaro/start.sh`, which is also the container's command. The launcher sources
`/rust/carbon/tools/environment.sh` (Carbon's Doorstop loader), makes `$HOME`, builds the argv from
the `RUST_SERVER_*` / `RUST_RCON_PORT` / `RCON_PASSWORD` environment and `exec`s `RustDedicated`. It
never installs, updates or downloads anything; if `/rust` is empty it says so and exits.

`dev-servers/compose/rust.yml` uses the same image (`${RUST_IMAGE}`, resolved into
`dev-servers/_data/.targets/rust.env` by `install.sh`) and the same two mounts.

**Memory.** The `verify` runner boots every game with the same 3 GB limit and offers no per-game
hook, so the adapter ships none. A verification world (size 1000, ten slots) does pass inside it —
twice, measured — but it does so sitting *at* the ceiling: sampled peak 2.999 GiB of 3, surviving on
page-cache reclaim. Verifying a bigger world here needs a per-game memory limit in the runner first.
A server anybody plays on wants considerably more, which is why the rig's compose file sets no limit
at all.

## Dev environment

### Prerequisites

- Docker
- Node.js v22+ (for the RCON reload script)
- A Takaro account with a registration token ([get one from your Takaro dashboard](https://docs.takaro.io/advanced/adding-support-for-a-new-game))

### Quick start

The Rust test server runs in the shared dev rig under `dev-servers/`.

```bash
# Configure (from repo root), one time
cp dev-servers/.env.example dev-servers/.env
# Edit dev-servers/.env: set TAKARO_WS_URL, TAKARO_REGISTRATION_TOKEN and RCON_PASSWORD

# Install the pinned target (first install downloads ~5.9 GB) and start it
just dev-install rust
just dev-start rust

# Wait for the server to finish booting (first boot generates the world; several minutes)
# Check with: just dev-logs rust | grep "Server startup complete"

# Build, compile-check and deploy the plugin
just dev-deploy rust

# Hot-reload without a restart
cd games/rust && ./scripts/reload.sh        # honours RCON_PORT / RCON_PASSWORD
```

`install.sh rust` installs into `dev-servers/_data/rust/rust_dedicated`, which the container mounts
at `/rust`. A rig that predates the catalog target has the old three-mount layout
(`_data/rust/{plugins,server,carbon-logs}`); it is not migrated automatically. To keep an existing
world:

```bash
dev-servers/scripts/install.sh rust --force
mv dev-servers/_data/rust/server/takaro dev-servers/_data/rust/rust_dedicated/server/takaro
```

Starting a target-driven game whose data directory does not hold that exact target is refused before
anything boots (`ds_preflight_target`), with the install command to run.

### Config model

The plugin owns `<framework config dir>/TakaroConnector.json` (`Interface.Oxide.ConfigDirectory`:
`carbon/configs/` on Carbon, `oxide/config/` on Oxide) and does its own reads and writes instead of
the framework's `Config` object, because it needs the file's text to watch it:

- **First load** (file absent, empty, or a JSON object with none of the four keys, e.g. `{}`; a file
  that exists but cannot be read is never recreated): the plugin writes it with an empty
  `RegistrationToken` and a new UUID `IdentityToken`. If `TAKARO_REGISTRATION_TOKEN` or
  `TAKARO_IDENTITY_TOKEN` is set, the install already exists under the identity the environment
  gives it (possibly the empty one, which versions before the file sent), so `IdentityToken` is
  written empty and nothing is generated.
- **Precedence**: a non-empty environment variable wins over the file field, per field. An empty one
  counts as unset.
- **While running**: a 1 s main-thread tick re-reads the file every 5 s and compares the whole text.
  A change to the URL, the registration token or the identity drops the socket and connects at once
  (backoff reset, auth-close `_shouldReconnect = false` cleared). `Debug` applies without a
  reconnect. A missing, empty or unparseable file keeps the current settings; an absent token key
  keeps the token in use, an absent `WebSocketUrl` / `Debug` falls back to the default.
- **Threads**: connections are only started on the main thread. Each connection carries a generation;
  a stale socket task, auth close or reconnect request (reconnects are queued for the tick, not started
  from the socket task) changes nothing. Identify and responses go out on the socket the request came
  in on, and a queued action from a replaced connection is dropped before it runs. `Unload` bumps the generation and destroys the tick.
- **Banner**: an empty token, an identify rejection and an auth close print a `***` banner with the
  exact config path (or the environment variable that overrides it) through `PrintWarning`, which is
  the server console.

The pure part lives between `takaro:config-begin` / `takaro:config-end`; `tests/config/run.sh`
compiles that region on its own against the pinned `Newtonsoft.Json.dll` and runs the fresh, upgrade,
env-precedence, half-saved-file and identity cases (CI runs it in the `test` job).

### Environment variables

| Variable | Description | Default |
|----------|-------------|---------|
| `TAKARO_WS_URL` | Takaro WebSocket endpoint | `wss://connect.takaro.io/` |
| `TAKARO_REGISTRATION_TOKEN` | Server registration token | (required) |
| `TAKARO_IDENTITY_TOKEN` | Unique server identity | `takaro-dev-rust` |
| `TAKARO_DEBUG` | Enable debug logging | `false` |
| `RCON_PASSWORD` | RCON password (dev only) | (required, set in `dev-servers/.env`) |

### Edit loop

```bash
vim mod/TakaroConnector.cs
just dev-deploy rust      # compile-check against the pinned assemblies, then deploy
./scripts/reload.sh       # requires Node.js v22+
just dev-logs rust
```

## Verification coverage

`takaro-maint verify --game rust` boots the pinned target in the pinned container against a local
protocol harness — **no Rust client is involved**. What it proves:

compile/load (`carbon-compile`: Carbon compiled the deployed source, loaded the version the build
manifest names, wrote no compile error and did not self-update), `startup` (including input
integrity across the boot), `identify`, `heartbeat` (`testReachability`), `players` on an empty
server, `items` and `entities` (both catalogues, human display names spot-checked), `console`,
`action` (a broadcast), `reconnect` and `stop`.

`stop` replaces the base `shutdown` check, which gates on the container's exit code. **Rust's exit
code is not a shutdown signal**: the Unity player segfaults inside its own teardown on some runs
(139) *after* it has saved the world, unloaded the plugins and printed `Quitting`, and exits 0 on
others with the same lines in the same order. So `stop` asserts the sequence the server really
writes — saved, connector unloaded, quit — and that the process is gone afterwards, and records the
exit code without judging it.

Rust's console echoes neither a command it was handed nor a chat broadcast, so `console` and
`action` are observable only because the connector logs both (`console: <command>`,
`broadcast: <message>`) — lines worth having anyway, since they are an operator's only record of
what Takaro did on their server. What they prove is that the instruction reached the server and the
connector carried it out, not that a player saw it.

What it does **not** prove, and what therefore stays ⚠️ in the README: every player hook
(connect/disconnect/chat/death/kill), `giveItem`, `teleportPlayer`, `kickPlayer`, `banPlayer`,
`unbanPlayer`, `listBans`, `getPlayer*`, `listLocations`, Oxide (the file is Oxide-API compatible
but only Carbon is booted), and hosted Takaro over TLS. Those need a client session.

`connector-load`, `catalog-items` and `catalog-entities` are Minecraft-specific check ids — they
look for lines and spot values only that connector writes — and, with the base `shutdown` that
`stop` replaces, are never selected for Rust. A bare `verify --game rust` runs exactly the set
the target record's `verification.separate` names (plus `build`); an explicit `--checks` is left
as written, including one that names a check this game cannot pass.

## Discovery and readiness

`takaro-maint scan --game rust` watches two upstreams:

- **Steam** (`sources.steam`): the `public` branch of app 258550 is observed; `release` and
  `staging` are declared but disabled (they are what operators watch on purpose, and enabling them
  would file issues nobody is acting on); `aux02`, `debug`, `last-month` and anything matching
  `^aux[0-9]+$` are known branches, which produce nothing; any other branch Steam grows becomes a
  branch-review candidate.
- **Carbon** (`sources.carbon-api`): the GitHub release listing. The channel is declared
  `mutable: true`, so the asset's published digest is folded into the revision and re-uploading the
  same tag is a new revision (`2.0.259.<digest8>`) rather than a silent replacement. History is
  reported `heads-only`, because uploads replaced between two scans were never seen.

A moved Rust head files **one** support issue whose Readiness table reads `| carbon | missing |` and
whose state is `blocked-upstream`. That is deliberate, not a gap: GitHub's release metadata does not
say which Rust build a Carbon build targets (the `.info` sidecar carries a Carbon protocol date, not
a Steam build id), so nothing can claim the two are compatible until a target that pins both lands.
The source names sort `carbon-api` < `carbon-download` < `steam`, so the framework is observed
before the game head is reconciled.

## Build and release

- `.github/workflows/rust.yml`:
  - **test** — resolves the target, restores the fingerprint-keyed cache, runs
    `setup-environment.sh` and `compile-check.sh` on the hosted runner.
  - **params** — resolves the version, tag and publish channel.
  - **release** — delegates to `.github/workflows/connector-release.yml`, which builds every
    candidate/maintained target, checks the artifacts carry the target's identity, builds again and
    requires identical bytes, and publishes the set with a compat record and `SHA256SUMS`.
- The release ships `takaro-rust-plugin-carbon-25454815-<version>.cs`, a byte-identical
  `TakaroConnector.cs` alias (so an existing install path keeps working), `SHA256SUMS` and
  `takaro-rust-<version>.compat.json`. The compat record names the Steam pseudo-URL and the Carbon
  asset URL for each target and claims `verification: {required: "build", executed: null}` —
  the runtime leg is off for this connector, because a 5.9 GB depot set with an 8 GB RAM footprint
  does not fit a hosted runner. Runtime CI is tracked separately.
- Versioning is release-please driven; the `// x-release-please-version` marker on the `[Info(...)]`
  line in `mod/TakaroConnector.cs` and `version.txt` are the version sources.

## Testing status

The rows the README marks ✅ were proven by `takaro-maint verify` on Rust build 25353106 with Carbon
2.0.259 and no client. The current target, build 25454815, is compile-checked against those same
Carbon bytes; its runtime re-verification is tracked separately. Everything a client is needed for
is still ⚠️ and unproven; see **Verification coverage** above.
