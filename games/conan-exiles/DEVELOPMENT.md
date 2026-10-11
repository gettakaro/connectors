# Conan Exiles connector — development

Developer, architecture and build notes for the Conan Exiles connector. Operator install steps live
in [README.md](README.md).

## Architecture

The shipped connector is native (`native/`): `libtakaro-conan-native.so` (`LD_PRELOAD`, Linux) and
`winmm.dll` (proxy DLL, Windows) run inside the server process and hold the Takaro WebSocket
themselves. No sidecar, no RCON, no mod. See [Native connector](#native-connector-native).

Layout:

```
games/conan-exiles/
    native/                     # the native connector (core/, platform/linux, platform/windows, tests/, tools/)
    mod/TakaroConanBridge/      # spec + DevKit handoff for the Takaro-owned .pak (no binary shipped)
    scripts/lib-target.sh       # resolves the catalog target every script builds against
    scripts/build-release.sh    # packages the native zip per target
    scripts/templates/          # the README.txt of the native zips (the shipped takaro.json is native/takaro.json)
    INSTALL.md                  # per-platform install, upgrade, rollback (ships in the zips)
    version.txt
    CHANGELOG.md

catalog/conan-exiles/
    game.json                     # the Steam watch: app 443030, depot 443032, branch public
    targets/linux-25792439.json   # Linux: the current pinned server build, images, deps, file hashes
    targets/windows-25792439.json # Windows: the same Steam build, depot 443031, zig toolchain
    targets/*-25738716.json       # the previous builds, still pinned and still built
    targets/*-25639945.json
```

## The pinned server build

The connector is built and verified against exact server builds. The default is declared in
`catalog/conan-exiles/targets/linux-25792439.json`: Steam app `443030`, branch `public`, build
`25792439`, depot `443032` manifest `7357061300451132906` plus the Steamworks redistributable depot
`1006` manifest `4559160656493359681`, with six declared file hashes. The previous build
`25738716` and `25639945` keep their targets: the library pins all three, so each target's zip carries the same code. Nothing here runs the Steam
updater any more — the depot manifests are the bytes:

```bash
maintenance/bin/takaro-maint install --game conan-exiles --dest /path/to/server
maintenance/bin/takaro-maint ledger check --game conan-exiles --dest /path/to/server
```

To move the pin to a newer head, read what Steam publishes and record it:

```bash
maintenance/bin/takaro-maint steam branches --game conan-exiles
maintenance/bin/takaro-maint steam pin --game conan-exiles --metadata \
  --record-files ConanSandboxServer.sh \
  --record-files ConanSandbox/Binaries/Linux/ConanSandboxServer-Linux-Shipping \
  --record-files ConanSandbox/Binaries/Linux/libDreamworld.so \
  --record-files ConanSandbox/Content/Paks/global.utoc \
  --record-files ConanSandbox/Content/Paks/pakchunk0-LinuxServer.utoc \
  --record-files linux64/steamclient.so \
  --write
```

`--write` refuses to record a manifest whose declared files it has not hashed, so a re-pin can
never leave a target half-describing its own bytes. The `conan-exiles-legacy` branch is the UE4
server: it is declared in the watch and disabled, so it is never observed and never files an issue.

## Conan server setup for development

The rig (`dev-servers/scripts/install.sh conan-exiles`) installs the pinned build from the catalog,
builds the native connector and unpacks it into `<server>/TakaroConanNative/TakaroConanNative/`. It
does not preload the library: to run the connector on the rig, add `LD_PRELOAD` and `takaro.json`
by hand exactly as an operator does ([INSTALL.md](INSTALL.md)). By hand, the native Linux launcher
is started like this:

```bash
LD_PRELOAD=/path/to/TakaroConanNative/libtakaro-conan-native.so \
./ConanSandboxServer.sh -log -server -nosteamclient \
  -MULTIHOME=127.0.0.1 \
  -Port=7777 \
  -QueryPort=27015
```

The connector does not use RCON; leave it off.

## Building

```bash
games/conan-exiles/native/build.sh --tests          # Linux library + its tests, in the buster toolchain
games/conan-exiles/native/platform/windows/build.sh  # winmm.dll, with the pinned zig (ZIG=...)
```

A release is built per catalog target, through the shared maintenance command:

```bash
maintenance/bin/takaro-maint build --game conan-exiles [--target linux-25792439] \
  --version 1.0.2 --out dist
```

That runs `scripts/build-release.sh <version> <out-dir> [--target <id>]`, which resolves the target
and builds inside pinned images only, so the host needs no compiler. The result is
`takaro-conan-exiles-native-<platform>-<build>-<version>.zip` holding one `TakaroConanNative/`
folder (the binary, `takaro.json` (= `native/takaro.json`), `INSTALL.md`, `README.txt`, `THIRD-PARTY.md`, licenses,
`takaro-target.json`, `SHA256SUMS`, and on Linux `ca-certificates.crt`), plus a `.meta.json` beside
it that `takaro-maint artifact validate` reads. Packaging is deterministic: entry modes and
timestamps are normalised and the archive is written by CPython's `zipfile`.

`.github/workflows/conan-exiles.yml` runs the `native` and `native-windows` jobs and then hands the
release to `connector-release.yml` in catalog mode, which builds each target, validates artifact
identity, and publishes the target-qualified zips, `SHA256SUMS` and a compat record.

## Native connector (`native/`)

`libtakaro-conan-native.so` runs inside the Linux server process (`LD_PRELOAD`) and `winmm.dll`
inside the Windows one; both hold the Takaro WebSocket themselves: no sidecar and no RCON. Every
action and event is native; `core/conan/capabilities.json` is the honest per-action state
(`live-supported`, `schema-fallback`, `unsupported`, `not-requested`; the drift test refuses a
`pending` row).

```
native/
  core/            portable, no OS headers
    takaro/        Takaro protocol: ITransport + frame queues + heartbeat, bridge, durable outbox, config
    conan/         the Conan adapter, sendMessage, the coverage registry (+ capabilities.json)
    ue/            UE reflection (names, object walk, property offsets, controllers)
    pins/          startup signature scan + pinned builds (+ pins.json)
  platform/linux/  LD_PRELOAD entry, ProcessEvent detour, /proc/self/maps + build-id, libwebsockets transport,
                   buster Dockerfile.build and build.sh
  platform/windows/ winmm.dll proxy, MinHook ProcessEvent detour, WinHTTP transport (abortive close of a
                   dead link: abortive_close.*), PE scan; build.sh (zig 0.13.0, reproducible), Dockerfile.builder
  tests/           unit tests, pins oracle, drift test, fake Takaro wire tests, real-library test
  tools/           sigderive.py (signatures for ELF and PE)
```

- **Config** (`core/takaro/config.*`, `core/takaro/config_watch.*`). Per field: environment
  (`TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_WS_URL`, `TAKARO_SERVER_NAME`,
  `TAKARO_CA_FILE`, `TAKARO_STATE_DIR`), then `ConanSandbox/Saved/Config/Takaro/takaro.json`
  (`TAKARO_CONAN_CONFIG` moves it; the URL only when not the default), then the connector's saved
  copy `<stateDir>/saved-settings.json`. The release ships the real `takaro.json` (empty token and
  identity; `native/takaro.json`, equal to `kConfigTemplate`, checked by `unit_test`); a missing
  file is created from it at load unless the environment carries both tokens.
  - Identity: env, file, saved copy, the one in use, else a new UUID v4 written into the file and
    the saved copy, with the name `Conan Exiles (<first 8>)` (names are unique per Takaro domain).
    A state dir without a saved copy means an older connector ran here: then no identity is
    generated (that would orphan the Takaro record); a banner asks for the old one.
  - The saved copy holds URL, identity and name, and the registration token only after Takaro
    accepted it; never values from the environment. The token is never written into `takaro.json`.
    Rewrites keep the file's mode and owner (`ReplaceUserFile`).
  - Live reload: the bridge thread re-reads the file every 5 s, applies a changed text when a read
    1 s later matches, and ignores an unparseable one. A change of URL, token, identity or name
    calls `ITransport::Retarget`, which drops the socket (Linux: on the lws service thread, after the
    old socket's own CLOSED callback; Windows: under the transport lock, with a generation check for
    a dial in flight) and redials at once. `caFile`, `stateDir` and the env-only tunables are read
    at startup only.
  - No token, an unreadable or invalid file, a `ws://` URL or a missing identity on a prior install:
    the library still loads (hooks and all) but never dials, and prints a `****` banner to the
    server's stdout naming the file; it repeats after 60 s and then every 15 minutes. Identify
    refusals print a banner too (name conflict / 409 separately); Takaro's error is reduced to
    name, message and HTTP status (`DescribeTakaroError`). Only `TAKARO_CONAN_NATIVE_DISABLE=1` is
    inert. Token values are never logged.
- **Pins.** Only the three raw globals come from fixed knowledge: `ProcessEvent`,
  `GUObjectArray.ObjObjects` and the `FNamePool` block table. At load the library scans the
  server's executable mappings for their signatures (`core/pins/pins.cpp`, derived with
  `tools/sigderive.py`, about 140 ms) and accepts the build only when every signature matches
  exactly once and the GNU build-id is pinned with the same addresses (25792439:
  `ad0c0103…`; 25738716: `8d5382c1…`; 25639945: `3a05a6ef…`). On any other build it installs no hook, still connects and identifies, sends one
  critical notice (a `log` event) and refuses every action with a structured error.
  `TAKARO_CONAN_ALLOW_UNPINNED_BUILD=1` accepts a clean scan of an unpinned build, for re-pin work.
- **Everything else is reflection.** At first use the library walks the object array in
  16384-object slices on the game thread. It finds the chat `UFunction`, `GameStateBase` and the
  live GameState, then reads property offsets by name and type: `PlayerArray`, `Owner`,
  `UserIDFromURLOptions` and `PlayerNamePrivate`. Live cost on 25639945: 1.48M objects scanned in
  47 ms total, spread over about 90 ticks; a send costs about 0.02 ms of game-thread time.
- **ChatRpcData** (0x80 bytes): Timestamp is FILETIME, not FDateTime. userName is at 0x48,
  Channel (`Global`) at 0x58, Message at 0x68 and generated at 0x78, all FStrings.
- **Threads.** The libwebsockets service thread owns the socket; the bridge thread owns protocol
  state and the outbox; action workers run actions. The ProcessEvent detour only drains the
  game-thread queue when a job is pending, at most 4 jobs or 500 µs per drain.
- **Delivery.** Every event goes through a durable outbox (`<Saved>/Takaro/state/event-outbox.json`,
  tmp + fsync + rename) and leaves it only when the pong of a later WebSocket ping confirms it, so a
  Takaro outage or a server restart replays the unconfirmed tail. Reconnect backoff doubles from
  2 s to 60 s and resets after a successful identify. A health snapshot is written to
  `<Saved>/Takaro/state/health.json`.
- **Toolchain.** `platform/linux/Dockerfile.build` is Debian buster pinned by digest, with
  packages from its dated snapshot, and builds OpenSSL 3.5.8 and libwebsockets 4.5.8 statically
  from SHA-256-checked archives. glibc 2.28 is the newest the server binary needs, and `build.sh`
  refuses a library that needs anything newer, exports a symbol, or has an unresolved strong symbol.
- **Tests.** `make -C native test` (or `native/build.sh --tests`) builds the library and runs, in
  the buster container: unit tests; the drift test (`capabilities.json` and `pins.json` equal the
  compiled tables); wire tests of the production Takaro half against a fake Takaro over TLS
  (identify, ping/pong, request correlation, every args shape, events, a forced 20 s outage with
  outbox replay, a killed process replaying from disk, identify rejection, unknown-build refusal);
  and the real library preloaded into a stand-in server executable. With
  `CONAN_SERVER_BINARY=<25639945 ConanSandboxServer-Linux-Shipping>` it also checks that the scan
  reproduces the stage 1 addresses.
- **Windows tests.** `platform/windows/build.sh --tests` also cross-compiles
  `build-windows/tests/abortive_close_test.exe` (the abortive close of a dead link: address match,
  a raw socket pair, a real WinHTTP WebSocket against a loopback server). CI runs it under Wine, whose
  WinHTTP closes an unmarked WebSocket gracefully (the duplicate-delivery case); it also passes on
  Windows 11, where WinHTTP already resets one.

## Portable verification

**Contract level — every build, no game.** `native/build.sh --tests` (Linux) and
`platform/windows/build.sh --tests` plus the Wine run (Windows), as described above. This is what CI
runs: a hosted runner cannot hold a 4.3 GB depot or 10 GB of RAM, so the release claims `contract`
verification and nothing more.

**Startup level — a real pinned server.** On a host that can boot it:

```bash
maintenance/bin/takaro-maint verify --game conan-exiles --target linux-25792439 \
  --artifacts dist --out reports --checks build,startup \
  --startup-timeout 600 --cleanup-orphans
```

The hooks live in `maintenance/src/takaro_maint/games/conan_exiles/verify.py`. The verifier boots
the pinned server from the installed depot bytes **without** the native library, so `startup`
proves only that the server boots in the runtime image and its declared files are still the
ledger's bytes. Every base row that needs a connector to answer (`identify`, `connector-load`,
`heartbeat`, `players`, `catalog-*`, `console`, `shutdown`) is excluded with that reason.

Everything a player sees (chat, Discord, kicks, bans, teleports, items, events) is proven only on
the live runbook with a real client; see the table in [README.md](README.md).

## Takaro-owned Conan mod path

The native connector needs no mod. An earlier Takaro-owned `.pak` was specified, but **no `.pak` is
built or shipped from this repo**: building it requires the Conan Exiles DevKit (Windows, Unreal
cook toolchain), so it cannot be downloaded from the releases page, and build 25639945 refuses both
the last `TakaroConan.pak` and Enhanced Pippi (`Mod is too old and needs to be updated for this
game version`). Specs live in `mod/TakaroConanBridge/`:

- `MOD_SPEC.md` — required minimal `TakaroConan.pak` behaviour.
- `BUILD_ENVIRONMENT.md` — Conan DevKit / cook toolchain gate.
- `INSTALL_RECONNECT_LIVE_TEST.md` — Pippi replacement, client relogin and live validation loop.
- `API_COVERAGE_BOUNDARY.md` — connector-owned actions/events vs wider Takaro MCP tools.
- `COMPLETION_CHECKLIST.md` — final done checklist for the server+client mod goal.
- `DEVKIT_IMPLEMENTATION_NOTES.md` — source-attributed DevKit implementation contract.
- `devkit-handoff/` — build contract and PowerShell build helpers (the blueprint and
  implementation plan live in the private workspace repo).

Read-only local gates (`check-mod-toolchain.sh`, `check-takaro-mod-install.sh`,
`audit-takaro-mod-goal.sh`) were used during the mod campaign; they are **not currently present in
this repo** (only `scripts/build-release.sh` ships here), so re-add them from the campaign workspace
before relying on them:

```bash
bash ../scripts/check-mod-toolchain.sh
bash ../scripts/check-takaro-mod-install.sh
bash ../scripts/audit-takaro-mod-goal.sh
```

The first gate must pass before this machine can build a cooked Conan `.pak`. The second must pass
before claiming the Takaro-owned mod is installed and replacing Pippi. The audit gate runs syntax,
build/tests, safe MCP live verification, toolchain, install and live mod gates, and must pass before
the full Takaro Conan mod goal can be called done.
