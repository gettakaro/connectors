# Conan Exiles connector — development

Developer, architecture and build notes for the Conan Exiles connector. Operator install steps live
in [README.md](README.md).

## Architecture

The shipped connector is native (`native/`): `libtakaro-conan-native.so` (`LD_PRELOAD`, Linux) and
`winmm.dll` (proxy DLL, Windows) run inside the server process and hold the Takaro WebSocket
themselves. No sidecar, no RCON, no mod. See [Native connector](#native-connector-native).

The Node.js bridge (`bridge/`) is **deprecated and frozen**: it still ships as a legacy zip on the
Linux target for one more release cycle and gets no fixes. The bridge sections below (RCON, log
tailing, the save database, the chat renderer) describe that legacy path only; the native
connector needs none of it and RCON stays off.

Layout:

```
games/conan-exiles/
    native/                     # the native connector (core/, platform/linux, platform/windows, tests/, tools/)
    bridge/                     # the deprecated Node.js sidecar (frozen)
    mod/TakaroConanBridge/      # spec + DevKit handoff for the Takaro-owned .pak (no binary shipped)
    scripts/lib-target.sh       # resolves the catalog target every script builds against
    scripts/build-release.sh    # packages the native zip per target (+ the legacy bridge zip on Linux)
    scripts/templates/          # takaro.json examples and the README.txt of the native zips
    scripts/check-exact-source.mjs  # lockfile and tarball hashes vs the catalog, before npm ci
    INSTALL.md                  # per-platform install, migration from the bridge, rollback (ships in the zips)
    TakaroConfig.example.txt    # the bridge's config example
    version.txt
    CHANGELOG.md

catalog/conan-exiles/
    game.json                     # the Steam watch: app 443030, depot 443032, branch public
    targets/linux-25639945.json   # Linux: pinned server build, images, deps, file hashes
    targets/windows-25639945.json # Windows: the same Steam build, depot 443031, zig toolchain
```

## The pinned server build

The connector is built and verified against one exact server build, declared in
`catalog/conan-exiles/targets/linux-25639945.json`: Steam app `443030`, branch `public`, build
`25639945`, depot `443032` manifest `8611640520811009059` plus the Steamworks redistributable depot
`1006` manifest `4559160656493359681`, with six declared file hashes. Nothing here runs the Steam
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

The rig (`dev-servers/scripts/install.sh conan-exiles`) does all of this from the catalog. By hand,
the native Linux launcher is started like this:

```bash
./ConanSandboxServer.sh -log -server -nosteamclient \
  -MULTIHOME=127.0.0.1 \
  -Port=7777 \
  -QueryPort=27015 \
  -RconEnabled=1 \
  -RconPassword=YourRconPassword \
  -RconPort=25575
```

RCON can also be enabled in `Game.ini`:

```ini
[RconPlugin]
RconEnabled=1
RconPassword=YourRconPassword
RconPort=25575
```

Or with equivalent command-line flags:

```powershell
ConanSandboxServer.exe -RconEnabled=1 -RconPassword=YourRconPassword -RconPort=25575
```

Conan has RCON karma protection. Keep `pollIntervalMs` at the default `10000` or higher unless you
also raise `RconMaxKarma` on a test server. For high-volume live verification on a disposable test
server:

```ini
[RconPlugin]
RconMaxKarma=1000
```

### Observed RCON behaviour

Conan's RCON differs slightly from strict Source RCON expectations: auth success uses packet id `0`,
type `2`, body `Authenticated.`; the command response reuses the auth packet id, matching `mcrcon`
when a single packet id is reused.

## Building and running from source

```bash
cd games/conan-exiles/bridge
npm install
cp ../TakaroConfig.example.txt ../TakaroConfig.txt
npm run build
npm start
```

A release is built per catalog target, through the shared maintenance command:

```bash
maintenance/bin/takaro-maint build --game conan-exiles [--target linux-25639945] \
  --version 1.0.2 --out dist
```

That runs `scripts/build-release.sh <version> <out-dir> [--target <id>]`, which resolves the target
and builds inside the image the target pins — the same image the server runs in, so the host needs
no Node at all. Before `npm ci`, `check-exact-source.mjs` asserts that the lockfile still resolves
every catalog-pinned dependency to the recorded URL and that the tarball there still hashes to the
recorded sha256; neither failure falls back to installing something else.

The result is `takaro-conan-exiles-bridge-linux-25639945-<version>.zip` containing `dist/`,
`scripts/`, `package.json`, `package-lock.json`, `takaro-target.json`, `README.md`,
`TakaroConfig.example.txt` and a generated `README.release.txt`, plus a `.meta.json` beside it that
`takaro-maint artifact validate` reads. The zip contains no `src/`, so every runtime `package.json`
script points at compiled `dist/` output (`npm start` -> `dist/index.js`, `npm run mod-helper` ->
`dist/mod/pollerCli.js`) and runs under `npm ci --omit=dev`; the script fails the build if either
entrypoint is missing from the package. Packaging is deterministic: two builds of one commit are
byte-identical, which CI checks by building twice and comparing.

`npm run mod-helper:dev` is the `tsx` source variant for local development only.
`.github/workflows/conan-exiles.yml` runs the `test` job (`npm ci`, `npm test`, `npm run build`)
and then hands the release to `connector-release.yml` in catalog mode, which builds each target,
validates artifact identity, and publishes the target-qualified zip, the legacy alias, `SHA256SUMS`
and a compat record.

## Configuration reference

Required: `registrationToken`, `serverName`, `rconHost`, `rconPort`, `rconPassword`,
`rconCommandGapMs`.

Optional: `identityToken`, `takaroWsUrl`, `httpPort`, `pollIntervalMs`, `enableLogEvents`,
`logFiles`, `databasePath`, `itemCatalogPath`, `requireModSourceAttribution`.

Useful Conan log paths:

- `ConanSandbox/Saved/Logs/ConanSandbox.log` — player chat and general server logs
- `ConanSandbox/Saved/Logs/ConanSandbox_2.log`
- `ConanSandbox/Saved/Logs/RconCommandLog.log`

## Health and the mod command bridge

```bash
curl http://127.0.0.1:3010/health
```

The same local HTTP server exposes the mod-facing command bridge:

- `GET /mod/poll` returns the next queued command for a Conan-side helper.
- `POST /mod/result` completes a queued command with `{ "requestId": "...", "result": { ... } }`.
- `POST /mod/event` forwards helper-emitted events with `{ "type": "chat-message", "data": { ... } }`.

The helper polls `http://127.0.0.1:3010/mod/poll` from the server host and renders `sendMessage`
commands as normal in-game chat, optionally scoped to the `recipient` Steam64 ID.

Takaro-owned Conan mods should identify the poll source so `/health` can distinguish the real `.pak`
from a host-side helper:

```text
GET /mod/poll?source=TakaroConan
X-Takaro-Mod-Source: TakaroConan
```

Set `requireModSourceAttribution=true` for final TakaroConan validation. In that mode `/mod/poll`,
`/mod/result` and `/mod/event` reject anonymous helper traffic, so a result or chat event cannot be
accepted unless it carries `source=...` or `X-Takaro-Mod-Source`. Ambient User-Agent values are
ignored in strict mode because they are not a durable final-proof source.

`/health` exposes `modBridge.lastPollSource`, `modBridge.lastResultSource`,
`modBridge.lastResultAt`, `modBridge.lastEventSource`, `modBridge.lastEventAt`,
`modBridge.lastEventType`, `modBridge.recentResults`, `modBridge.recentEvents` and
`modBridge.sourceAttributionRequired`. The recent-trace arrays are bounded and are used by final
validation to prove the exact current message markers were handled by `TakaroConan`, not by stale
logs or the Pippi/RCON renderer.

`npm run verify:mod-protocol` is a sidecar-contract diagnostic only: it pauses the host poller,
queues one Takaro `sendMessage` through MCP, handles it through `/mod/poll` and `/mod/result` as
`TakaroConanProtocolProbe/1.0`, posts one `/mod/event`, verifies source attribution, then resumes
the host poller. It proves the HTTP contract the future `.pak` must use; it is not installed-mod
proof and does not satisfy the final `TakaroConan` source gates.

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

- **Config.** Environment first (`TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN`,
  `TAKARO_WS_URL`, `TAKARO_SERVER_NAME`, `TAKARO_CA_FILE`, `TAKARO_STATE_DIR`), then
  `ConanSandbox/Saved/Config/Takaro/takaro.json` (keys `identityToken`, `registrationToken`, `url`,
  `name`, `caFile`, `stateDir`; `TAKARO_CONAN_CONFIG` moves the file). It fails closed: missing
  tokens, a `ws://` URL or a file that does not parse leave the library inert, with the reason in
  `ConanSandbox/Saved/Logs/TakaroConanNative.log`. Token values are never logged.
- **Pins.** Only the three raw globals come from fixed knowledge: `ProcessEvent`,
  `GUObjectArray.ObjObjects` and the `FNamePool` block table. At load the library scans the
  server's executable mappings for their signatures (`core/pins/pins.cpp`, derived with
  `tools/sigderive.py`, about 140 ms) and accepts the build only when every signature matches
  exactly once and the GNU build-id is pinned with the same addresses (25639945:
  `3a05a6ef…`). On any other build it installs no hook, still connects and identifies, sends one
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

## Host-side chat renderer

```bash
TAKARO_CONAN_RENDER_COMMAND="/path/to/render-conan-chat" npm run mod-helper
```

The renderer command receives the queued message as JSON on stdin plus:

- `TAKARO_CONAN_REQUEST_ID`
- `TAKARO_CONAN_MESSAGE`
- `TAKARO_CONAN_RECIPIENT`

The poller refuses to start without a renderer, so Takaro messages are not acknowledged unless
something has accepted responsibility for rendering them. A standalone sidecar process cannot create
normal Conan chat lines by itself.

If the server runs a chat mod exposing RCON commands, the helper can render through it:

```bash
BRIDGE_CONFIG=/path/to/TakaroConfig.txt \
TAKARO_CONAN_CHAT_MOD=pippi \
npm run mod-helper
```

Supported `TAKARO_CONAN_CHAT_MOD` values:

- `pippi` — resolves online character names from `listplayers` and sends Pippi
  `directmessage <sender> <character> <message>` for targeted chat. Server-wide messages use
  Enhanced Pippi's `server <message>` because `globallink` returned OK but did not render as visible
  client chat during live validation.
- `amunet` — sends `ast chat "global" <sender>:<message>`.

Optional overrides: `TAKARO_CONAN_RCON_HOST`, `TAKARO_CONAN_RCON_PORT`,
`TAKARO_CONAN_RCON_PASSWORD`, `TAKARO_CONAN_RCON_TIMEOUT_MS`, `TAKARO_CONAN_SENDER_NAME`.

### Enhanced Pippi notes

Use the Enhanced Pippi workshop item, not the Legacy one:

- Enhanced Pippi workshop ID `3725018456`.
- Legacy Pippi workshop ID `880454836` is opened by the Enhanced Linux server but does not register
  the Pippi mod controller or the `globallink` RCON command.

Server-side layout used during validation:

```text
ConanSandbox/Mods/Pippi.pak
ConanSandbox/Mods/modlist.txt
```

`modlist.txt`:

```text
*Pippi.pak
```

`ConanSandbox/Saved/Config/LinuxServer/ServerSettings.ini`:

```text
ServerModList=modlist.txt
```

## Takaro coverage registry

The code-level coverage registry lives in `bridge/src/takaro/coverage.ts`;
`bridge/src/__tests__/coverage.test.ts` fails if any Takaro action or event type is missing from it.
Each entry is `live-supported`, `schema-fallback` or `unsupported`.

Live-supported actions: `testReachability`, `getPlayers`, `getPlayer`, `getPlayerLocation`,
`getPlayerInventory`, `listItems`, `listEntities`, `listLocations` (the last five DB-backed via
`databasePath`), `giveItem` (`con <player> SpawnItem`), `teleportPlayer`
(`con <player> TeleportPlayer`), `sendMessage`, `executeConsoleCommand`, `kickPlayer`, `banPlayer`,
`unbanPlayer`, `listBans`, `shutdown`.

Schema-valid fallbacks:

- Without `databasePath`, `getPlayerInventory`, `listItems`, `listEntities` and `listLocations`
  return `[]`, and `getPlayerLocation` returns `{ "x": 0, "y": 0, "z": 0 }`.
- `getMapInfo` returns
  `{ "enabled": false, "mapBlockSize": 0, "maxZoom": 0, "mapSizeX": 0, "mapSizeY": 0, "mapSizeZ": 0 }`.

Explicitly unsupported: `getMapTile`. Unsupported actions return structured errors instead of timing
out.

`sendMessage` uses the mod command bridge. If no Conan-side helper is polling `/mod/poll`, the
connector returns a clear failure and does not fall back to vanilla RCON `broadcast`, which Conan
renders as a server-wide popup/overlay rather than a chat line.

If Takaro rejects the connector `identify` payload, the bridge records `takaroIdentifyError` in
`/health` and disables reconnect attempts until the runtime credentials are updated, so a stale
registration token does not loop against `wss://connect.takaro.io/`. The current identify contract
requires a valid `registrationToken`; `identityToken` is optional and is not accepted as a
standalone replacement. Read-only MCP game-server routes can expose the current `identityToken` but
not a usable `registrationToken`.

## Save database reads

With `databasePath` pointing at Conan's `game_0.db`, the bridge reads `characters`, `account`,
`actor_position` and `item_inventory`. `characters.id` joins `actor_position.id` for coordinates,
`characters.playerId` joins `account.id` for Steam/platform identity, and `item_inventory.owner_id`
joins `characters.id` for inventory rows.

## Identity and event confidence

`listplayers` provides the stable identifier; the parser prefers explicit user/platform/Steam IDs and
falls back to the strongest numeric identifier. Player names are display data and must not be used
as durable `gameId` values unless no identifier exists.

Connect/disconnect events are derived from `listplayers` deltas. The connector emits the current
online players on startup after Takaro identification, then later deltas.

Chat parsing: Pippi emits `[Pippi]ChatWindow: Character <name> said: <message>` as the real chat
event and `[Pippi]PippiChat: <name> said in channel [Global]: <message>` for the same message (kept
log-only to avoid duplicates). `ChatWindow` lines carry only the character name, which is resolved
against live `listplayers` output to produce Steam64 `gameId`, display name, `steamId` and
`platformId`. The richer live format
`ChatWindow: Character <name> (uid <id>, player <steam64>) said: <message>` is parsed directly.
Connector-internal `characterName` is stripped from outbound player payloads (it previously leaked
through `getPlayers`). Non-Pippi chat formats remain best effort.

`player-death` and player-attributed `entity-killed` are best-effort log-derived events from Conan
`KillCharacterWithRagdoll_Implementation` lines, enriched from `listplayers` when the character is
online.

## Portable verification

Two lanes, and each one says what it does not cover.

**Contract level — `npm test`, no game, no network.** `src/__tests__/bridgeContract.test.ts` runs a
real bridge between a fake Conan RCON server (with Conan's auth-reply quirk) and a fake Takaro
WebSocket server, in one process. It covers the identify payload, the target stamp on `/health`,
`testReachability` (a real RCON `help`), `getPlayers`, `executeConsoleCommand`, the documented
`sendMessage` refusal, the `getMapTile` unsupported error, reconnect after a `1001` close, and a
clean stop. This is what CI runs: a hosted runner cannot hold a 4.3 GB depot or 10 GB of RAM, so
the release claims `contract` verification and nothing more.

**Startup level — a real pinned server.** On a host that can boot it:

```bash
maintenance/bin/takaro-maint verify --game conan-exiles --target linux-25639945 \
  --artifacts dist --out reports \
  --checks build,startup,bridge-identify,bridge-reachability,bridge-players,bridge-console,bridge-reconnect,shutdown,bridge-stop \
  --startup-timeout 600 --cleanup-orphans
```

The hooks live in `maintenance/src/takaro_maint/games/conan_exiles/verify.py`. They write a per-run
RCON password into `ConanSandbox/Saved/Config/LinuxServer/Game.ini` before the server starts (never
on the command line, which is logged), boot the pinned server from the installed depot bytes, then
start the *deployed artifact* as a second container the way the release README says to
(`npm ci --omit=dev`, `node dist/index.js`) and drive the checks through it:

| Check | What it proves |
|---|---|
| `startup` | The pinned server boots and its declared files are still the ledger's bytes. |
| `bridge-identify` | The sidecar identified with Takaro, and logged the catalog target it was built for. This is Conan's equivalent of the base `identify` + `connector-load` rows. |
| `bridge-reachability` | The server opened RCON and the sidecar reached it — `RconCommandLog.log` shows the `help` it says it ran. |
| `bridge-players` | An empty server answers with an empty list, produced by a logged `listplayers`. |
| `bridge-console` | A console command runs over RCON; `sendMessage` without the chat helper returns the documented structured refusal, recorded as a coverage statement rather than a pass. |
| `bridge-reconnect` | Takaro closes the socket with `1001`; the sidecar comes back and is usable again. |
| `shutdown` | Takaro's `shutdown` reaches the server over RCON and the container exits 0. |
| `bridge-stop` | The sidecar stops cleanly on SIGTERM and the pinned inputs are unchanged afterwards. |

The base `identify`, `connector-load`, `heartbeat`, `players`, `catalog-*` and `console` rows are
not selected for this game — they read other games' log lines or run before the sidecar exists — so
a Conan report reaches level `startup` and never claims `protocol`.

**Not covered by either lane**, and not claimed anywhere: chat delivery (needs Enhanced Pippi and
the `mod-helper` process), anything the Takaro-owned `TakaroConan.pak` would add (it has no build
host — see the DevKit gate below), `player-connected` / `player-disconnected` / `player-death` /
`entity-killed` events, save-database reads, and every gameplay-level effect (an item actually
landing in an inventory, a teleport moving a client). Those stay on the live-check evidence in
[README.md](README.md) and on the human lanes.

### Findings from the 25356024 rig lane (2026-09-21)

The rig was run end to end against hosted Takaro on the pinned build: install from the depot
manifests, `takaro-maint deploy` of the release zip, both containers up from the resolved target.

- The engine banner on this build is `LogInit: Build: ++exiles+release-CL-376069` /
  `LogInit: Engine Version: 5.8.2-376069+++exiles+release` — the connector's runtime-identity
  parser reads both, and `Compatible Engine Version` is deliberately not matched.
- `LoadMap` took 49 s and the process settled around 8.5 GB resident, which is why the verifier's
  container hook asks for more memory than the runner's 3 GB default.
- The bridge logged its target stamp, identified with hosted Takaro, and `/health` reported
  `target.target = linux-25356024`.
- Through the Takaro API: `testReachability` → `connectable: true`, `getPlayers` → `[]`,
  `executeConsoleCommand help` → the server's full RCON command list. `RconCommandLog.log`
  recorded the matching `help` and `listplayers` lines.
- The WebSocket was closed three times with `1006` during startup, while the log tailer was
  emitting the boot log as `log` events, and the bridge reconnected and identified on its own.
  Reconnect works; the event flood at startup is worth a look of its own.
- `shutdown` works but is **slow**. The server logged
  `LogRcon: Warning: Received Rcon: shutdown` and `LogNet: World NetDriver shutdown` at once, then
  ran for about four and a half more minutes — at full CPU, logging nothing — before
  `LogExit: Exiting.` and a clean exit 0. Any stop timeout around a Conan shutdown has to be
  minutes, not seconds; the verifier's `shutdown` row needs a budget of at least 360 s here.
- `docker stop -t 30` on the sidecar exits 0: `index.ts` handles SIGTERM, and the pinned inputs
  still hash as the ledger recorded them afterwards.

`takaro-maint verify --game conan-exiles` is not yet runnable end to end, for two reasons in the
shared verifier:

1. The runner boots `containerRef` with the image's own entrypoint and a fixed memory cap. This
   game needs to name a command and its own options instead. The adapter already ships
   `container_command` and `container_options` (unit-tested directly); they take effect once that
   runner seam lands.
2. The base `shutdown` check waits 120 s for the container to exit, and this server takes about
   four and a half minutes. That budget has to come from the game before the row can be selected
   here.

Until both land, the rig lane above is this target's startup-level evidence.

### Findings from the 25639945 real-client lane (2026-10-02)

The exact target was installed with `takaro-maint install` into an isolated directory, run in the
pinned Node image with no mods, and joined from a Conan Exiles Enhanced client (revision
378,132) on the Windows gamer PC. Everything was driven through the Takaro MCP against hosted
Takaro; evidence lives under the runner's `.runner-reports/349/e2e/linux-25639945/`.

- **No chat mod loads.** The server logs `SetCompatibleDevkitVersions: [1002]` and refuses both
  Enhanced Pippi (last Workshop update 2026-06-11) and the July `TakaroConan.pak` with `Mod is
  too old and needs to be updated for this game version`, then exits. Takaro chat delivery
  (`sendMessage`, Discord → game) is therefore unavailable on this build; the bridge refuses it
  with `Conan chat bridge is not connected` and never falls back to `broadcast`.
- **Inbound chat works without a mod.** Vanilla `ChatWindow: Character … said:` lines became
  `chat-message` events, and an `@`-prefixed module command typed in game ran.
- **Bans need an online player.** `banplayer platformid <id>` (and `userid <id>`) for an offline
  player answers `No player with platform ID <id>.` and bans nothing. The bridge used to report
  that as success; it now returns a failed action. Banning an online player kicks them, writes
  `Saved/blacklist.txt` and refuses the rejoin with `PreLogin failure: UserBanned`.
  `unbanplayer <steam64>` works offline.
- **`listbans` prints bare Steam IDs**, one per line, with no reason.
- **RCON karma and connection churn.** The bridge used to open one RCON connection per command.
  At the default 10 s poll plus Takaro's reachability checks, Conan's karma denied every
  connection after about 70 minutes (`Rcon connection … triggered karma system and has been
  denied`), for ten minutes at a time, so a Takaro shutdown in that window failed with
  `write EPIPE`. Conan serves several commands on one connection and answers them in order (its
  reply ids lag one request behind), so the bridge now keeps one authenticated socket and
  reconnects only after it drops.
- `shutdown` → `LogExit: Exiting.` and exit 0 took about three minutes on this build.

## Live verification

```bash
npm run build
npm run verify:live
```

The script checks the bridge health endpoint, fresh Takaro validation errors in the bridge log
(`logs/conan-exiles-takaro.log`), and Conan save DB reads when `databasePath` is configured. It
initializes a Takaro MCP session and runs non-destructive game-server checks against the
`gameServerId` from `/health`: reachability, players, bans, map info fallback, map tile unsupported
response and `executeCommand help`.

MCP calls are paced by `TAKARO_CONAN_MCP_ACTION_GAP_MS` (default 6 s) to avoid tripping Conan RCON
karma. Do not run multiple live verifiers concurrently against the same test server.

The direct RCON `help` sweep is off by default because karma can deny repeated localhost probes:

```bash
TAKARO_CONAN_RUN_RCON_PROBES=1 TAKARO_CONAN_VERIFY_SEND_MESSAGE=0 npm run verify:live
```

Keep `TAKARO_CONAN_VERIFY_SEND_MESSAGE=0` when the verifier should not emit a chat line through the
installed chat bridge.

Do not run destructive checks (live kick, ban, shutdown, teleport, inventory mutation) against an
active player without explicit approval. For mutation smoke tests use a known online test player and
a harmless item/coordinate.

## Live findings (2026-06-20 / 2026-06-21)

- `testReachability`, `getPlayers` and `listBans` succeeded against real Conan RCON.
- `sendMessage` was originally RCON `broadcast`; live review showed it renders as an overlay, so the
  mod command bridge replaced it.
- Enhanced Pippi `server <message>` was confirmed visible in client chat; `globallink` returned OK
  but rendered nothing.
- `opts.senderNameOverride` is propagated and embedded in the message body (Pippi `server` has no
  sender parameter). Without an override the default MCP send path renders as `Takaro: ...`.
- `directmessage <sender> <characterName> <message>` returned `Sent message ... to player "..."`;
  the Steam display name does not match, the character name does.
- Live player chat from `werwerwer` / `76561198000735875` advanced Takaro `chat-message` analytics
  with no validation errors.
- No usable vanilla command for normal chat delivery exists: `SendAzureTextChatMessage`,
  `GlobalChat`, `ServerMessage`, `ChatWindow`, `DumpConsoleCommands` and similar `con 0 ...` probes
  were rejected as unknown.
- No top-level RCON/Pippi commands exist for `teleport`, `teleportplayer`, `teleporttoplayer`,
  `summonplayer`, `setplayerpos`, `getplayerpos`, `giveitem`, `spawnitem` or `listitems`.
- The `con <id> <command> <args>` relay does expose online-player client commands:
  `con 0 SpawnItem 1 1` succeeds; `con 0 Teleport <x> <y> <z>` succeeds but does not move the
  player, while `con 0 TeleportPlayer <x> <y> <z>` triggers Conan teleport streaming.
- Enhanced Pippi exposes `kill <playerName>`, but this bridge has no `killPlayer` action.
- `listbans` parsing has been validated against empty and populated output on the test server;
  capabilities data still flags populated-server output as the weaker case.

## Takaro-owned Conan mod path

The visible chat bridge is still Enhanced Pippi backed. The Takaro-owned replacement is specified,
but **no `.pak` is built or shipped from this repo** — building it requires the Conan Exiles DevKit
(Windows, Unreal cook toolchain), so it cannot be downloaded from the releases page. Specs live in
`mod/TakaroConanBridge/`:

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
