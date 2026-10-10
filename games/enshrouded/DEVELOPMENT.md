# Takaro Enshrouded Connector — development

Developer, architecture and build notes. Operator install instructions live in [README.md](README.md).

## Architecture

Enshrouded has no modding API, RCON or scripting. The connector is one in-process plugin:

```
enshrouded_server.exe (Wine/Proton, container)
  └─ dbghelp.dll  ← mod/ (C++17, MinHook). Proxy DLL loaded by the server.
       ├─ resolves game functions by string-xref + prologue signatures (never fixed RVAs)
       ├─ tails game state (players, chat, deaths, kills) into an event ring buffer
       ├─ native connector (src/native/): WSS to Takaro, 17 actions, event outbox, timed bans
       └─ diagnostics on 127.0.0.1:18890 (Bearer token): /health, /events, /debug/perf; see mod/docs/API.md
sidecar/ (Node 22, TypeScript) — RETIRED from releases in 0.6.0. Kept in the tree for one release as the
  source of the parity fixtures and the reference for a rollback to the 0.5.0 assets (INSTALL.md); a 0.5.0
  sidecar needs TAKARO_LEGACY_HTTP=1 (action routes over HTTP) and TAKARO_NATIVE_DISABLE=1 in the game container
```

The Takaro protocol lives in the plugin, in `src/native/`: nothing runs next to the game server. The sidecar was a
habit from the first C++ connector, not a constraint (no inbound port is needed for an outbound WebSocket, and
WinHTTP gives TLS without shipping a TLS library).

### Native connector

`src/native/` is game-independent: it never includes `world.h` or `hooks.h` and reaches the game only through
`native/game_api.h`, which `src/native_glue.cpp` implements by calling the plugin's own contract router
in-process (`PluginCall`). Threads:

- **transport** (`transport_winhttp.cpp`): WinHTTP in **async** mode, the only mode that passed the G0 probe under
  GE-Proton10-30 (sync receives cannot be cancelled and sync sends wedge under Takaro's protocol pings). Per
  connection ("epoch") one supervisor, one send thread (one send outstanding, buffer alive until WRITE_COMPLETE) and
  one receive thread. Reconnect backoff 2 s doubling to 60 s, reset on identify. Application heartbeat: Takaro
  answers `{"type":"ping"}` with `{"type":"pong"}` in order; a ping every 5 s (plus one 1 s after an event burst),
  and 20 s without any message closes the link (WinHTTP's receive timeout does not apply to WebSockets under Wine).
  TLS: system trust, or only the root in `TAKARO_CA_FILE`/`caFile` (pinned root, hostname and dates checked);
  an unusable CA file fails closed.
- **bridge** (`bridge.cpp`): owns all protocol state and the state files. Drains transport notices 512 at a time.
  Requests: `requestId` <= 128 bytes, JSON nesting <= 64, duplicates and overload (128 pending) answered with an
  error. Outbound priority: control > bridge errors > responses > events.
- **action workers** (4, `adapter.cpp`): the sidecar's `adapter.ts`/`mapping.ts` ported line for line. They may wait
  on the game's queues (bounded, see hooks.h); a request still running after 30 s is answered with an error and its
  late result is dropped, but its state changes (a ban) are recorded. Queued game jobs that never started are
  cancelled.

Events: the bridge reads the plugin ring (`/events`, 512 at a time), maps and filters them (the sidecar's
`NOISY_LOG` filter, `ENSHROUDED_LOG_EVENTS`), and appends them to a durable outbox (at most 5000 events / 32 MiB;
when full, the oldest `log` events go first, counted as losses). An event leaves the outbox only when the pong of a
**later** ping arrives on the same connection; after a reconnect everything unconfirmed is sent again (at least
once). Kept from the sidecar: the 60 s location fallback after a connect/disconnect, online reconciliation at start,
on a new plugin bootId and every 30 s (two misses in a row), and the log-tail fallback while `logEvents`/`players`
are degraded.

State lives in `<server dir>\takaro\connector-state\` (`TAKARO_STATE_DIR`): `event-outbox.json` (scan and
confirmed cursors per bootId, pending events), `online-players.json` (the sidecar's format; `TAKARO_ONLINE_FILE`),
`known-players.json`, `timed-bans.json`, `ban-intent.json`. Writes are tmp file, `FlushFileBuffers`, then
`MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`. A file that exists but does not parse fences its area (never
overwritten, never read as empty) and turns `connectorState` degraded. On the first start the sidecar's
`event-cursor.json` (`TAKARO_CURSOR_FILE`) is imported once.

Timed bans (new over the sidecar, which banned permanently): `banPlayer` with `expiresAt` bans in the game and
records the expiry; every 5 s the bridge lifts expired bans itself (`/unban`, retried with backoff) and `listBans`
reports the expiry. The intent is journalled before the game acts, so a crash mid-ban is recovered at the next start.

- `mod/`: `src/` source, `third_party/minhook` (vendored, see VENDORED.txt), `tools/gen_gamedata.py`
  (item/entity/location tables from the server kfc), `tests/` (host-side correlator test).
- `sidecar/`: the retired 0.5.0 Takaro bridge, its unit tests (vitest), the mock plugin and
  `npm run parity-fixtures`, which writes the fixtures the native host tests replay. Not packaged.
- `scripts/validate-module-proof.mjs`: validates a module live-proof JSON file.

The plugin is a dbghelp proxy because `enshrouded_server.exe` imports only `MiniDumpWriteDump` from
`dbghelp.dll`; that export is forwarded to the system copy and the plugin starts on its own thread
(no work under the loader lock). It writes `<server dir>\takaro\plugin.log` and caches resolved
account hashes in `<server dir>\takaro\accounts.json` so offline unban survives restarts.

## The catalog target

Everything below resolves one record: `catalog/enshrouded/targets/proton-1024233.json`. It
names the Steam app, branch and build id, both depot manifests with the sha256 of the files
that matter, the game image by tag **and** digest, the Proton the image carries, the builder
image, the zig tarball by URL and sha256, and the artifact name. No script, compose
file or workflow here hard-codes any of those; they ask for them:

```bash
maintenance/bin/takaro-maint targets resolve --game enshrouded --format env --prefix ENSHROUDED
```

| Key | What it is |
|---|---|
| `ENSHROUDED_TARGET`, `ENSHROUDED_FINGERPRINT`, `ENSHROUDED_FP16` | the target id and the hash of everything pinned in it |
| `ENSHROUDED_REVISION`, `ENSHROUDED_GAME_BUILD` | the game's own build id, `1024233` — what the server prints and the plugin pins against |
| `ENSHROUDED_HOOKS_PROVEN_BUILD` | the build the signatures were derived on; `verify` fails when the running server reports another one |
| `ENSHROUDED_IMAGE`, `ENSHROUDED_PROTON` | the game image by digest and the Proton inside it (`GE-Proton10-30`) |
| `ENSHROUDED_STEAM_APP`, `_STEAM_BRANCH`, `_STEAM_BUILDID`, `_STEAM_DEPOTS` | the exact Steam pin |
| `ENSHROUDED_TOOLCHAIN`, `ENSHROUDED_DEP_ZIG_URL`, `ENSHROUDED_DEP_ZIG_SHA256` | the builder image and the zig it installs |
| `ENSHROUDED_ARTIFACT_SERVER_PLUGIN` | the artifact name pattern |
| `ENSHROUDED_SERVER_EXE_SHA256` | the sha256 of the `enshrouded_server.exe` this plugin was built for |

### Install the exact server

```bash
maintenance/bin/takaro-maint install --game enshrouded --dest <dest>/server
maintenance/bin/takaro-maint ledger check --game enshrouded --dest <dest>/server
```

DepotDownloader fetches exactly the pinned manifests of depot 2278521 (the game) and depot
1004 (the Steamworks SDK redistributable). A manifest Steam no longer serves is an error
(exit 4), never a silent fall back to the branch head; a declared file whose hash does not
match leaves the existing install untouched (exit 5). The install lays down
`takaro/plugin/`, `savegame/`, `logs/`, `backups/`,
`steamapps/compatdata/2278520/` and a `takaro/PINNED.txt` that says what this directory is.
Everything in the target's `preserve[]` — the config, the save game, the compat prefix and
`takaro/` — survives a re-pinned install, and `--rollback` restores the `.previous` tree.

### Keep the game where it is

The image runs `/usr/local/etc/enshrouded/enshrouded-updater` at every container start,
which compares the installed build with the branch head and runs `steamcmd +app_update`
over the install when they differ. Both compose files bind-mount
[`server/enshrouded-updater`](server/enshrouded-updater) read-only over it; that script starts the
server and nothing else, and a test asserts it contains no update path at all. `verify`
greps the boot log for one and fails the run if it finds one.

### Moving to a new game build

1. `takaro-maint steam pin --game enshrouded --metadata` to see what moved, then
   `--record-files … --write` to re-pin the record (id and `revision` follow the new game
   build, so the target is a new file).
2. Re-derive the plugin's signatures against the new `enshrouded_server.exe` (the
   enshrouded-engineer workflow; `mod/tests/run.sh` with `ENSHROUDED_EXE=` pointed at the
   installed binary parses the anchors out of the exact binary).
3. `takaro-maint verify --game enshrouded …` must report `plugin-health` **pass** — every
   capability `ok`, `gameBuild` equal to the new revision.
4. Open the PR with the new target and the evidence. Until all of that is done, the old
   target stays the default and the readiness note in `catalog/enshrouded/game.json` says
   why a moved head is not ready.

## Build

```bash
# the connector, packaged exactly as a release ships it
maintenance/bin/takaro-maint build --game enshrouded --version <v> --out <dir>
```

That runs `scripts/build-release.sh <version> <out> --target proton-1024233`, which builds
`Dockerfile.builder` (the pinned Node base plus zig by hashed tarball), cross-compiles
`dbghelp.dll` inside it and packages `takaro-enshrouded-plugin-proton-1024233-<v>.zip` with
`pkg_zip` (fixed order, fixed timestamps from `SOURCE_DATE_EPOCH`), a `.meta.json` beside it
carrying the target, the fingerprint and the source revision, and an outer `SHA256SUMS`.
Building twice gives identical bytes; CI checks that, and so does
`takaro-maint artifact validate --game enshrouded <zip>`. The zip holds one folder whose
contents go next to `enshrouded_server.exe`:

```
TakaroEnshrouded/
    dbghelp.dll
    takaro/plugin.json             scripts/templates/plugin.json (empty tokens: paste and start)
    README.txt                     scripts/templates/plugin-README.txt, version stamped
    INSTALL.md                     install, upgrade from 0.5.0, rollback
    THIRD-PARTY.md                 mod/third_party/README.md
    licenses/                      MinHook, zig compiler-rt, libc++/libc++abi/libunwind, mingw-w64
    SHA256SUMS                     every file above, by path
```

For a quick loop without the packaging step:

```bash
# plugin: cross-compile dbghelp.dll with zig 0.13 (set ZIG=/path/to/zig if not on PATH)
./mod/build.sh                 # -> mod/build/dbghelp.dll
./mod/tests/run.sh             # host-side plugin tests (needs docker; pinned gcc:14 by digest)
NATIVE_ONLY=1 ./mod/tests/run.sh   # just the native connector tests
./mod/tests/wine/run.sh        # the real DLL under the pinned Proton image against tests/fake_takaro_ws.py

# parity fixtures: the sidecar produces them, the native host tests replay them
cd sidecar && npm run parity-fixtures   # after an intentional behaviour change only

# the retired sidecar (parity source; CI still runs its suite)
cd sidecar && npm ci && npm run typecheck && npm test
```

`DEBUG_CORRUPT_SIG=<signature name> ./mod/build.sh` builds a debug DLL into `mod/build-debug/` with one
signature corrupted, to exercise the degrade self-check. That is exactly what
`takaro-maint verify --negative` builds, so the compatibility claim is falsifiable.

## Deploy

```bash
maintenance/bin/takaro-maint deploy --game enshrouded --dest <dest>/server \
    --from <dir>/build-manifest.json
```

`deploy` places `dbghelp.dll` at `<dest>/server/takaro/plugin/dbghelp.dll` (atomically —
the game loads that file). A zip that holds anything outside its one top-level folder is
refused and nothing is extracted, and a component role other than `server-plugin` is refused.
A `takaro/sidecar/` folder an earlier 0.5.0 deploy left behind is not touched. Stop the game
container first: a running server holds the DLL open. The configuration
(`takaro/plugin.json` or the environment) is the operator's, never written by `deploy`; the
plugin itself only adds a missing `identityToken`/`name` to it.

## Verify

```bash
maintenance/bin/takaro-maint verify --game enshrouded --artifacts <dir> --out <reports> --negative
```

With no `--checks`, the run selects `build` plus the target record's
`verification.separate` -- `startup`, `plugin-health`, `native-identify`,
`native-players`, `native-catalog`, `native-console`, `action`, `reconnect`, `event`,
`stop`, `negative-degraded-hooks` -- and the run prints that list. The generic runner's
`connector-load`, `identify`, `heartbeat`, `players`, `catalog-*`, `console` and `shutdown`
checks wait for lines and answers another connector gives (a target-check line, a line on
the container's stdout, Minecraft ids, an exit code), so they are left out (the report
records them as skipped) and each `native-*` check says which one it replaces.

`--checks` narrows that set further and is taken literally, so naming one of the left-out
ids selects it and it will fail. `negative-degraded-hooks` is selected by default but still
needs `--negative`: the flag decides whether the degraded boot runs, `--checks` decides
whether the check is selected at all.

`--label tm.run=...` and `--label tm.ttl=...` are refused: the harness sets both itself
(`tm.run` from `--run-id`). Pass only your own keys.

It boots the pinned image on the exact install with the deployed DLL and the updater
override. The plugin only accepts `wss://`, so the fake Takaro serves TLS from a CA made for
the run (`GameHooks.takaro_tls`, `verify/tls.py`); `takaro/plugin.json` (mode 0600) carries
the fake's URL, both throwaway tokens, `caFile` and the diagnostics secret, and no Takaro
value reaches a docker command line. `/health` is read with `curl` inside the game
container, the bearer token on stdin. It reports:

| Check | What it proves |
|---|---|
| `plugin-health` | the compatibility claim: `/health` `status: ok`, `gameBuild == 1024233`, no capability `degraded`, the plugin version this run built, and the container's Proton equal to the target's |
| `native-identify` | the game process identified over TLS with both tokens, `plugin.log` says `native: identified with Takaro`, `/health` `diagnostics.native.state == identified`, its application ping got a pong and it answered Takaro's empty RFC 6455 ping (Enshrouded's `identify` and `heartbeat`) |
| `native-players` | `testReachability` connectable with a **null** reason, `getPlayers` empty |
| `native-catalog` | `listItems`/`listEntities`/`listLocations` non-empty, every entry with a `code` and a readable, distinct `name` |
| `native-console` | `executeConsoleCommand version` answers with the game build |
| `action` | `sendMessage` resolves end to end (nobody is online, so this proves the path, not delivery) |
| `reconnect` | the fake closes with 1001, the plugin logs it, reconnects by itself, identifies again and answers |
| `event` | a plugin log event reaches Takaro as a `gameEvent` |
| `stop` | the server saves, supervisord respawns it, the container stops 0, no update path ran and every pinned file still hashes the same |
| `negative-degraded-hooks` | with `--negative`: a build with one corrupted signature identifies on its own, is reported `degraded`, **fails** the claim and names the capability in the reachability reason, while the server stays alive |

A full run with `--negative` passed every selected check on 2026-09-29 against the pinned
install and image (the plugin had identified over TLS by the time the server reached
HostOnline, 24.5 s after the boot; it re-identified 2.2 s after a
1001 close, and the corrupted `addComponent` build reported `teleport=degraded` in the
reachability reason). It proves the release artifact against a fake Takaro; the live proof
with a real client and real Takaro is separate.

`--negative` needs zig on the host (`TAKARO_MAINT_ZIG=/path/to/zig`); without it the check
is skipped with that reason rather than passed.

`mod/deploy.sh` is gone; it printed the two commands above and exits 2.

## Run it locally (Docker)

`docker-compose.example.yml` runs the image the target pins, by tag **and** digest, with
`WINEDLLOVERRIDES=dbghelp=n,b`, the plugin bind-mounted read-only over
`/opt/enshrouded/server/dbghelp.dll`, `server/enshrouded-updater` bind-mounted over the
image's updater program, and the Takaro settings in the game container's environment. One
service; the plugin's diagnostics port is never exposed on the host.

```bash
cd games/enshrouded
cp .env.example .env              # fill in tokens and role passwords
../../maintenance/bin/takaro-maint install --game enshrouded --dest "$PWD/data/enshrouded/server"
../../maintenance/bin/takaro-maint build  --game enshrouded --version dev --out /tmp/ens-dist
../../maintenance/bin/takaro-maint deploy --game enshrouded --dest "$PWD/data/enshrouded/server" \
    --from /tmp/ens-dist/build-manifest.json
docker compose -f docker-compose.example.yml --env-file .env up -d
docker compose -f docker-compose.example.yml logs -f
```

To update the plugin later: stop the game container (the server holds the DLL open), run
`build` and `deploy` again, start it.

UDP 15637 is published for clients. Check `GET /health` from inside the container for per-capability status.

The dev rig's `dev-servers/compose/enshrouded.yml` runs the same image and the same two
mounts (one service, the native connector configured by environment; `ENSHROUDED_NATIVE_DISABLE`
and `ENSHROUDED_LEGACY_HTTP` exist only for a rollback to the 0.5.0 pair), reading `ENSHROUDED_IMAGE` from the resolved target when the rig has resolved one.
It has no `lib/games/enshrouded.sh` install/deploy step yet -- `dev-servers/scripts/install.sh
enshrouded` answers "unknown game" -- so its data directory is laid down with the same
`takaro-maint install`/`build`/`deploy` commands as above, pointed at
`dev-servers/_data/enshrouded/server`. Both game-tree binds set `create_host_path: false`,
so a `docker compose up` before that install stops with "bind source path does not exist"
instead of creating an empty tree and a directory named `dbghelp.dll`.

## Environment

| Variable | Where | Purpose |
|---|---|---|
| `TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN` | game container (plugin) | native connector; or `identityToken` / `registrationToken` in `takaro\plugin.json`. Without a registration token the connection stays off, the console shows a banner and `/health` says why; a missing identity is generated (see Configuration) |
| `TAKARO_SERVER_NAME`, `TAKARO_WS_URL`, `TAKARO_CA_FILE` | game container (plugin) | or `name`, `url`, `caFile` in `plugin.json`; defaults: see Configuration, `wss://connect.takaro.io/`, system trust. Relative `caFile` resolves against the server dir |
| `TAKARO_RECONNECT_BASE_MS`, `TAKARO_RECONNECT_MAX_MS` | game container (plugin) | reconnect backoff, default 2000 / 60000 |
| `TAKARO_ACTION_TIMEOUT_MS`, `TAKARO_ACTION_WORKERS`, `TAKARO_POLL_INTERVAL_MS` | game container (plugin) | default 30000 / 4 / 250 |
| `TAKARO_STATE_DIR`, `TAKARO_ONLINE_FILE`, `TAKARO_CURSOR_FILE` | game container (plugin) | connector state (default `takaro\connector-state`), online-player file, sidecar cursor to import once |
| `TAKARO_NATIVE_DISABLE=1`, `TAKARO_LEGACY_HTTP=1` | game container (plugin) | turn the native connection off; serve the action routes over HTTP for the legacy sidecar |
| `TAKARO_CONFIG_POLL_MS` | game container (plugin) | how often `plugin.json` is re-read (default 5000, min 200; tests) |
| `TAKARO_LOG_FRAMES=1` | game container (plugin) | debug: write every outbound gameEvent and response frame to `plugin.log` (cut at ~2 KB; the identify frame is never logged). Use it to check player identity fields on the wire |
| `ENSHROUDED_LOG_TAIL`, `ENSHROUDED_LOG_FILE`, `ENSHROUDED_LOG_EVENTS` | game container (plugin) | log-tail fallback (`auto`), log path (default `logs\enshrouded_server.log`), `log` events (`filtered`) |
| `TAKARO_PLUGIN_TOKEN` (`TAKARO_ENSHROUDED_PLUGIN_TOKEN` in .env) | game container (plugin) | optional bearer secret for the diagnostics endpoint; or `token` in `plugin.json`. Without one the diagnostics answer 401 |
| `ENSHROUDED_ADMIN_PASSWORD` / `_PLAYER_` / `_GUEST_` | .env | Server role passwords |

## Configuration

`takaro\plugin.json` next to `enshrouded_server.exe` holds the same keys as JSON (`registrationToken`,
`identityToken`, `name`, `url`, `caFile`, `token`); an environment variable, when set and not blank, wins. The
plugin logs where each value came from, never the value. The release ships the real `plugin.json` with empty
tokens.

- **Live settings** (`url`, `registrationToken`, `identityToken`, `name`; `src/native/config_file.*`):
  `ConfigWatcher` runs on its own thread (never the game thread) and re-reads the file every 5 s, comparing
  the text. A changed text applies only when the next read 1 s later is the same and it parses (a BOM is
  fine); otherwise the running settings stay and the console says the file is broken. A change calls
  `Bridge::Reconfigure`, which on the bridge thread swaps the settings and calls `ITransport::Retarget`:
  the WinHTTP supervisor closes the current epoch and connects at once (no backoff), or idles while there
  is no registration token. An epoch that was being opened during a Retarget is dropped before its Open
  notice. Without a registration token at startup no bridge, transport or state is created; the watcher
  starts them when a token appears. `caFile`, `token` and every other setting are read at startup.
- **Precedence per field:** environment, then `plugin.json`, then the saved copy
  `<state dir>\saved-settings.json` (an upgrade that copies the whole zip replaces `plugin.json` but never
  the state folder). `url` from `plugin.json` counts only when it is not the default. The saved copy holds
  the url, identity and name in use (not values from the environment) and the registration token only after
  Takaro accepted it; an environment-only install writes none.
- **Identity:** environment, `plugin.json`, saved copy, then `my-enshrouded-server` when the state folder
  existed before this start without a saved copy (an install from an older release whose file lost its
  identity), else a new UUID. A resolved identity that `plugin.json` lacks is written into it (and the file
  is created when only the DLL was copied and nothing comes from the environment); the identity an install
  already uses never changes.
- **Name:** environment, `plugin.json`, saved copy, then `Takaro Dev Enshrouded` (the old default) when the
  identity came from the operator or a legacy install, else `<"name" from enshrouded_server.json, or
  Enshrouded> (<first 8 identity characters>)`, written back like the identity. Takaro keeps game-server
  names unique per domain and answers a taken name with 409, which gets its own banner.
- **Console:** banners and the `connecting` / `connected` lines go to the process's stdout (one `WriteFile`
  per message), which supervisord passes to `docker logs` and a panel console, and to `plugin.log`. A
  problem is printed once until the settings change. An identify error is reduced to its name, message and
  HTTP status (Takaro's error can carry its internal request, `x-takaro-token` included); long base64url
  runs are redacted.

## Capabilities

The user-facing table in [README.md](README.md) is the one kept current; `capabilities.json` is the
machine-readable copy. The native connector was proven live on 2026-09-29 (game build 1024233, a real client,
Takaro as the oracle): 48 of 52 acceptance rows proven; the rest are partial by design (`listLocations`, `log`)
or partial on catalogue names (`listItems`, `listEntities`).
Evidence is kept in the El-Limon workspace, not in this repository.

## Known limitations

- **Player name is the Steam persona name**, not the in-game character name (the server never exposes it). `gameId` = SteamID64.
- **Kick/ban need the player online**; unban needs the player to have been seen online once (account-hash cache). The game stores no ban reason and no expiry: the connector keeps both (`timed-bans.json`) and lifts a timed ban itself at expiry.
- **Whisper isolation is unproven**: whispers reach the recipient, but exclusion of other players was not verified (only one player account; accepted limitation).
- **Admins cannot be kicked/banned by the game itself**: Enshrouded ignores kick/ban for players whose group has `canKickBan`. Plugin >= 0.4.2 lifts that check for each Takaro kick/ban; older plugins return success while nothing happens.
- **listLocations** is implemented (1031 locations) but Takaro never calls it, so it is not observable end-to-end.
- **Log events are not stored** by Takaro as searchable events; they only surface via rate-limit records.
- **Signatures are pinned to game build 1024233 (Steam build 23178631).** Every hook is anchored on code shape and self-checked at load; on a game update a mismatching capability reports `degraded` in `/health` (and reachability reason) while everything else keeps working and the server keeps running. Re-derive signatures after updates — see "Moving to a new game build" above.
- **Catalogue names are the server's own codes with spaces**, not localised display names: no localisation ships with the dedicated server, so `Block_Roof_T5_Granite_Ornamented` becomes `Block Roof T5 Granite Ornamented`. `verify`'s `native-catalog` check enforces `name != code` and no underscores; it cannot enforce what the server does not have.
- Takaro rounds teleport coordinates to integers; the game nudges to a free voxel. `giveItem` stacks are split (game ignores count, max 64).
- **Discord echo**: Takaro core stores its own server messages as player-less chat, and the built-in `chatBridge` relays them back to Discord. Use the `chatBridgeNoEcho` fork (GameToDiscord skips chat without a player).

## Releases

Releases are cut by release-please (`release-please-config.json`, package `games/enshrouded`,
component `enshrouded`, tags `enshrouded-vX.Y.Z`). Merging a conventional commit that touches
`games/enshrouded/**` opens/updates a `chore(main): release enshrouded X.Y.Z` PR; merging that PR
tags the release and bumps both `games/enshrouded/version.txt` and the annotated
`#define TAKARO_PLUGIN_VERSION "X.Y.Z" // x-release-please-version` line in `mod/src/common.h`,
so the plugin's `takaro enshrouded plugin <version> starting` log line always matches the tag.

`.github/workflows/enshrouded.yml` runs `mod/tests/run.sh` (host tests, native connector
included), the retired sidecar's typecheck and tests (it still produces the parity fixtures)
and `mod/tests/wine/run.sh` (`native-wine`: the real DLL under the pinned Proton image
against a fake Takaro over WSS, zig taken from the catalog target) on every PR/push, and
then hands the packaging to the shared `connector-release.yml`, which resolves the target,
builds it twice and compares the bytes, and publishes:

- `takaro-enshrouded-plugin-proton-1024233-<v>.zip` — the `TakaroEnshrouded/` folder above
- its `.meta.json`, `SHA256SUMS`, and the legacy name `takaro-enshrouded-plugin.zip` as an
  alias of exactly those bytes. The catalog keeps the `takaro-enshrouded-sidecar.zip` alias
  for old links; with no sidecar role it is skipped (`aliasesSkipped`) and nothing is
  published under that name
- `takaro-enshrouded-<v>.compat.json` — what this release was built against: the Steam pin
  with both depot manifests, the image digest, the fingerprint, and
  `verification.required: "contract"` with `executed: null`

`runtime: false`: the server is a Windows binary under Proton on top of an 8.8 GB depot set,
which a hosted runner cannot boot, so CI claims contract verification only and the runtime
proof comes from `takaro-maint verify --game enshrouded` run locally against the exact
target. Onboarding that to CI is tracked separately.

Where a release goes depends on the trigger (`scripts/release-params.sh`): a PR gets a
disposable `pr-<n>-enshrouded` pre-release plus a sticky PR comment, a push to main refreshes
the rolling `enshrouded-dev` pre-release, and a release-please release calls this workflow
through `release-please.yml`'s `release-enshrouded` job to attach the assets to the real tag.
