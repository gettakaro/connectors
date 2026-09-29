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
sidecar/ (Node 22, TypeScript) — LEGACY, kept one release as the rollback path and the parity reference;
  needs TAKARO_LEGACY_HTTP=1 (action routes over HTTP) and TAKARO_NATIVE_DISABLE=1 in the game container
```

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
- `sidecar/`: the Takaro bridge, unit tests (vitest) and a mock plugin (`npm run mock-plugin`).
- `scripts/validate-module-proof.mjs`: validates a module live-proof JSON file.

The plugin is a dbghelp proxy because `enshrouded_server.exe` imports only `MiniDumpWriteDump` from
`dbghelp.dll`; that export is forwarded to the system copy and the plugin starts on its own thread
(no work under the loader lock). It writes `<server dir>\takaro\plugin.log` and caches resolved
account hashes in `<server dir>\takaro\accounts.json` so offline unban survives restarts.

## The catalog target

Everything below resolves one record: `catalog/enshrouded/targets/proton-1024233.json`. It
names the Steam app, branch and build id, both depot manifests with the sha256 of the files
that matter, the game image by tag **and** digest, the Proton the image carries, the builder
image, the zig tarball by URL and sha256, and the two artifact names. No script, compose
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
| `ENSHROUDED_SIDECAR_RUNTIME` | the `node:22-alpine@sha256:…` both sidecar Dockerfiles build FROM |
| `ENSHROUDED_ARTIFACT_SERVER_PLUGIN`, `_ARTIFACT_SIDECAR` | the two artifact name patterns |
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
`takaro/{plugin,sidecar}/`, `savegame/`, `logs/`, `backups/`,
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
# both components, packaged exactly as a release ships them
maintenance/bin/takaro-maint build --game enshrouded --version <v> --out <dir>
```

That runs `scripts/build-release.sh <version> <out> --target proton-1024233`, which builds
`Dockerfile.builder` (the pinned Node base plus zig by hashed tarball), then cross-compiles
`dbghelp.dll` and compiles the sidecar inside it, and packages
`takaro-enshrouded-plugin-proton-1024233-<v>.zip` and
`takaro-enshrouded-sidecar-proton-1024233-<v>.zip` with `pkg_zip` (fixed order, fixed
timestamps from `SOURCE_DATE_EPOCH`), plus a `.meta.json` beside each carrying the target,
the fingerprint and the source revision. Building twice gives identical bytes; CI checks
that, and so does `takaro-maint artifact validate --game enshrouded <zip>`.

The sidecar zip ships `sidecar/Dockerfile.release` **as** `Dockerfile`, so the folder an
operator unpacks builds on its own from the packaged `dist/`. The source `sidecar/Dockerfile`
compiles from `src/` and is what the dev compose builds.

For a quick loop without the packaging step:

```bash
# plugin: cross-compile dbghelp.dll with zig 0.13 (set ZIG=/path/to/zig if not on PATH)
./mod/build.sh                 # -> mod/build/dbghelp.dll
./mod/tests/run.sh             # host-side plugin tests (needs docker; pinned gcc:14 by digest)
NATIVE_ONLY=1 ./mod/tests/run.sh   # just the native connector tests
./mod/tests/wine/run.sh        # the real DLL under the pinned Proton image against tests/fake_takaro_ws.py

# parity fixtures: the sidecar produces them, the native host tests replay them
cd sidecar && npm run parity-fixtures   # after an intentional behaviour change only

# sidecar
cd sidecar && npm ci && npm run typecheck && npm test && npm run build
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
the game loads that file) and unpacks `TakaroEnshroudedSidecar/` to
`<dest>/server/takaro/sidecar/`, which is what the compose files build the sidecar image
from. A zip that holds anything outside its one top-level folder is refused and nothing is
extracted. Stop the game container first: a running server holds the DLL open.

Known gap: `deploy` records only the **last** component in the ledger's `artifact`, so
`ledger check` proves the sidecar zip's hash and not the plugin's. The plugin's own hash is
proven by `build` (which validates every artifact against the target) and by the
`takaro enshrouded plugin <version> starting` line `verify` requires in `plugin.log`.

## Verify

```bash
maintenance/bin/takaro-maint verify --game enshrouded --artifacts <dir> --out <reports> --negative
```

With no `--checks`, the run selects `build` plus the target record's
`verification.separate` -- `startup`, `plugin-health`, `sidecar-identify`,
`sidecar-players`, `sidecar-catalog`, `sidecar-console`, `action`, `reconnect`, `event`,
`stop`, `negative-degraded-hooks` -- and the run prints that list. The generic runner's
`connector-load`, `identify`, `heartbeat`, `players`, `catalog-*`, `console` and `shutdown`
checks watch the *game* server, which in Enshrouded never speaks to Takaro, and they run
before the sidecar exists, so they are left out (the report records them as skipped) and
each `sidecar-*` check says which one it replaces.

`--checks` narrows that set further and is taken literally, so naming one of the left-out
ids selects it and it will fail. `negative-degraded-hooks` is selected by default but still
needs `--negative`: the flag decides whether the degraded boot runs, `--checks` decides
whether the check is selected at all.

`--label tm.run=...` and `--label tm.ttl=...` are refused: the harness sets both itself
(`tm.run` from `--run-id`). Pass only your own keys.

It boots the pinned image on the exact install with the deployed DLL and the updater
override, starts a second container built from the **shipped** sidecar zip in the game
container's network namespace, and reports:

| Check | What it proves |
|---|---|
| `plugin-health` | the compatibility claim: `/health` `status: ok`, `gameBuild == 1024233`, no capability `degraded`, the plugin version this run built, and the container's Proton equal to the target's |
| `sidecar-identify` | the sidecar reached Takaro and identified (Enshrouded's `identify`) |
| `sidecar-players` | `testReachability` connectable with a **null** reason, `getPlayers` empty |
| `sidecar-catalog` | `listItems`/`listEntities` non-empty, every entry with a `code` and a `name` |
| `sidecar-console` | `executeConsoleCommand version` answers with the plugin version and game build |
| `action` | `sendMessage` resolves end to end (nobody is online, so this proves the path, not delivery) |
| `reconnect` | Takaro drops the socket with 1001 and the sidecar comes back and is usable |
| `event` | a plugin log event reaches Takaro as a `gameEvent` |
| `stop` | the server saves, supervisord respawns it, the container stops 0, no update path ran and every pinned file still hashes the same |
| `negative-degraded-hooks` | with `--negative`: a build with one corrupted signature is reported `degraded` and **fails** the claim while the server stays alive |

The base `identify`/`heartbeat`/`players`/`catalog-*`/`console`/`shutdown` checks are the
generic runner's, and they watch the *game* server — which here never speaks to Takaro. The
`sidecar-*` checks replace them and each says so in its detail, so an Enshrouded report
reaches `startup` and never claims `protocol`.

`--negative` needs zig on the host (`TAKARO_MAINT_ZIG=/path/to/zig`); without it the check
is skipped with that reason rather than passed.

`mod/deploy.sh` is gone; it printed the two commands above and exits 2.

## Run it locally (Docker)

`docker-compose.example.yml` runs the image the target pins, by tag **and** digest, with
`WINEDLLOVERRIDES=dbghelp=n,b`, the plugin bind-mounted read-only over
`/opt/enshrouded/server/dbghelp.dll`, `server/enshrouded-updater` bind-mounted over the
image's updater program, and the sidecar built from `./sidecar` with
`network_mode: service:enshrouded` (the plugin API is never exposed on the host).

```bash
cd games/enshrouded
cp .env.example .env              # fill in tokens and role passwords
../../maintenance/bin/takaro-maint install --game enshrouded --dest "$PWD/data/enshrouded/server"
../../maintenance/bin/takaro-maint build  --game enshrouded --version dev --out /tmp/ens-dist
../../maintenance/bin/takaro-maint deploy --game enshrouded --dest "$PWD/data/enshrouded/server" \
    --from /tmp/ens-dist/build-manifest.json
docker compose -f docker-compose.example.yml --env-file .env up -d --build
docker compose -f docker-compose.example.yml logs -f
```

To update the plugin later: stop the game container (the server holds the DLL open), run
`build` and `deploy` again, start it.

UDP 15637 is published for clients. Check `GET /health` from inside the container for per-capability status.

The dev rig's `dev-servers/compose/enshrouded.yml` runs the same image and the same two
mounts, reading `ENSHROUDED_IMAGE` from the resolved target when the rig has resolved one.
It has no `lib/games/enshrouded.sh` install/deploy step yet -- `dev-servers/scripts/install.sh
enshrouded` answers "unknown game" -- so its data directory is laid down with the same
`takaro-maint install`/`build`/`deploy` commands as above, pointed at
`dev-servers/_data/enshrouded/server`. Both game-tree binds set `create_host_path: false`,
so a `docker compose up` before that install stops with "bind source path does not exist"
instead of creating an empty tree and a directory named `dbghelp.dll`.

## Environment

| Variable | Where | Purpose |
|---|---|---|
| `TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN` | game container (plugin) | native connector; or `identityToken` / `registrationToken` in `takaro\plugin.json`. Without both the connection stays off and `/health` says why |
| `TAKARO_SERVER_NAME`, `TAKARO_WS_URL`, `TAKARO_CA_FILE` | game container (plugin) | or `name`, `url`, `caFile` in `plugin.json`; defaults `Takaro Dev Enshrouded`, `wss://connect.takaro.io/`, system trust. Relative `caFile` resolves against the server dir |
| `TAKARO_RECONNECT_BASE_MS`, `TAKARO_RECONNECT_MAX_MS` | game container (plugin) | reconnect backoff, default 2000 / 60000 |
| `TAKARO_ACTION_TIMEOUT_MS`, `TAKARO_ACTION_WORKERS`, `TAKARO_POLL_INTERVAL_MS` | game container (plugin) | default 30000 / 4 / 250 |
| `TAKARO_STATE_DIR`, `TAKARO_ONLINE_FILE`, `TAKARO_CURSOR_FILE` | game container (plugin) | connector state (default `takaro\connector-state`), online-player file, sidecar cursor to import once |
| `TAKARO_NATIVE_DISABLE=1`, `TAKARO_LEGACY_HTTP=1` | game container (plugin) | turn the native connection off; serve the action routes over HTTP for the legacy sidecar |
| `ENSHROUDED_LOG_TAIL`, `ENSHROUDED_LOG_FILE`, `ENSHROUDED_LOG_EVENTS` | game container (plugin) and sidecar | log-tail fallback (`auto`), log path (default `logs\enshrouded_server.log`), `log` events (`filtered`) |
| `TAKARO_ENSHROUDED_PLUGIN_TOKEN` | .env | Shared secret; passed as `TAKARO_PLUGIN_TOKEN` to both the game container (plugin) and the sidecar |
| `ENSHROUDED_ADMIN_PASSWORD` / `_PLAYER_` / `_GUEST_` | .env | Server role passwords |
| `TAKARO_REGISTRATION_TOKEN` | .env | Takaro registration token |
| `TAKARO_WS_URL` | sidecar | default `wss://connect.takaro.io/` |
| `TAKARO_IDENTITY_TOKEN` (`TAKARO_IDENTITY_ENSHROUDED` in compose) | sidecar | default `takaro-dev-enshrouded` |
| `TAKARO_SERVER_NAME` | sidecar | server name shown in Takaro |
| `TAKARO_PLUGIN_URL` | sidecar | default `http://127.0.0.1:18890` |
| `TAKARO_PLUGIN_TIMEOUT_MS`, `TAKARO_POLL_INTERVAL_MS` | sidecar | default 10000 / 1000 |
| `TAKARO_CURSOR_FILE`, `TAKARO_ONLINE_FILE` | sidecar | persisted event cursor and online-player set |
| `ENSHROUDED_LOG_TAIL`, `ENSHROUDED_LOG_FILE`, `ENSHROUDED_LOG_EVENTS` | sidecar | server log tailing (`auto`, path, `filtered`) |
| `SIDECAR_HEALTH_PORT`, `SIDECAR_HEALTH_HOST` | sidecar | health endpoint, default 127.0.0.1:18891 |
| `SIDECAR_EXIT_AFTER_UNREACHABLE_MS` | sidecar | self-exit after 180 s without plugin so compose restarts it into the new netns |

The plugin also reads the token from `<server dir>\takaro\plugin.json` (`{"token":"..."}`) when the env var is unset.

## Capabilities

Status from live testing against Takaro (oracle: Takaro MCP), last updated 2026-09-14; machine-readable copy in
`capabilities.json`, evidence in the El-Limon workspace. `proven` = observed end-to-end through Takaro plus a game-side check.

| Area | Capability | Status | Note |
|---|---|---|---|
| actions | `testReachability` | proven |  |
| actions | `getPlayers` | proven |  |
| actions | `getPlayer` | proven | via playerongameserverSearch/playerGetOne |
| actions | `getPlayerLocation` | proven | MCP playerongameserverSearch position + trackingGetPlayerMovementHistory; matches start spawn point within 0.2 m and follows teleports |
| actions | `getPlayerInventory` | proven | MCP trackingGetPlayerInventoryHistory matches in-game backpack (Ward_T1 level 5, arrows 26, club); Takaro history records changes only, so the unchanged starter legs (equipment) never appear there; plugin returns them |
| actions | `giveItem` | proven | non-admin player; game ignores count, plugin splits into full-stack + single actions (max 64); Admin-group player: give x5 + x4 arrived in client, a later client-driven backpack move was accepted server-side, no crash. Persistence across restart not verified (server was hard-killed before save and the character rolled back) |
| actions | `listItems` | proven | 3609 items via syncItems job, MCP itemSearch |
| actions | `listEntities` | proven | 979 templates, Takaro stored 977 (2 duplicate codes), MCP entitySearch; static data, live entity list only as /debug/nearby |
| actions | `listLocations` | partial | 1031 locations via plugin and sidecar adapter; Takaro app-api never calls listLocations and MCP has no location tool, so not observable via MCP |
| actions | `executeConsoleCommand` | proven | plugin-defined command set (help, players, say, whisper, location, teleport, tp, inventory, give, item, kick, save-and-shutdown); no settime |
| actions | `sendMessage` | partial | broadcast proven (renders as "<character name of an online player>: text"). Whisper reaches the recipient, but exclusion of other players is NOT verified: only one player account is available. Accepted limitation (user decision 2026-09-14) |
| actions | `teleportPlayer` | proven | Takaro rounds coordinates to integers; game nudges to a free voxel spot |
| actions | `kickPlayer` | proven | Friends group (0.3.0, 0.4.1, 0.4.2) and Admins group (0.4.2) proven via MCP + client 'The host has kicked you from the session.'. Plugin <=0.4.1 silently did nothing for Admins/canKickBan players: the game's own handler skips them; 0.4.2 lifts that check for the one call. MCP returns {} for success and for plugin errors alike |
| actions | `banPlayer` | proven | online players only; reason/expiresAt not supported by the game. Re-proven 2026-09-14 on 0.4.2 with an Admins-group player (ban dialog, bannedAccounts entry, rejoin refused) |
| actions | `unbanPlayer` | proven | requires the player to have been seen online once (hash cache); re-proven 2026-09-14 on 0.4.2 (unban log line, list empty, rejoin succeeded) |
| actions | `listBans` | proven |  |
| actions | `shutdown` | proven |  |
| events | `player-connected` | proven | stored records 7f2fa9b2, 4038ab40 from real joins; playerOnboarding hook fired from them |
| events | `player-disconnected` | proven | stored 7757b852 from a real leave; 9b618661 from sidecar reconciliation after the server was killed with the player online |
| events | `log` | partial | not a stored/searchable Takaro event; arrival only shown by event-rate-limited records (697530ba, limitedEventType log, droppedCount 231) |
| events | `chat-message` | proven |  |
| events | `player-death` | proven | fall death and death by enemy proven earlier. Non-player killer is now forwarded in the base msg field ('<name> was killed by <code>'; Takaro schema has no killerEntity and forbids unknown fields). Unit-tested only; not re-proven live after the change |
| events | `entity-killed` | proven | weapon is empty (weapon category not mapped); props/voxel destructibles are filtered out |
| modules | `command` | proven | real in-game chat with '@' prefix (utils ping, custom ensproof); '/', '!' also pass through |
| modules | `hook` | proven | real chat-message, real player-connected (playerOnboarding) and MCP hookTrigger |
| modules | `cronjob` | proven | MCP cronjobTrigger (message seen in client) and natural scheduled runs 20:05, 20:10 |
| modules | `teleports` | proven | @settp/@tp/@tplist/@deletetp; position change confirmed by plugin, server log, MCP pog and screenshots |
| modules | `serverMessages` | proven |  |
| modules | `playerOnboarding` | proven | join-time welcome is sent while the client is still loading and was not seen on screen; the same hook run via hookTrigger in game was seen |
| modules | `chatBridge` | proven | built-in chatBridge: D1, D4 and a human D2 proven. Replaced on this server by chatBridgeNoEcho (5ca72768) because of the Takaro-core echo |
| modules | `chatBridgeNoEcho` | proven | fork of chatBridge. GameToDiscord skips player-less chat. No echo proven, and player chat still relayed |
| resilience | `containerRestartReconnect` | proven | reachability false during restart, sidecar self-exit after 180 s, reconnected, connectable:true |
| resilience | `sidecarRestartNoReplay` | proven | cursor 693 persisted; no Takaro records created by the restart; new chat after restart forwarded once |
| resilience | `serverRestartOnlineReconcile` | proven |  |
| resilience | `pluginRestartDetection` | proven | seq-reset path proven live (0.4.0); plugin 0.4.1 adds bootId (cursor now stores it, used live after redeploy); bootId-only case unit-tested |
| resilience | `signatureSelfCheckDegrade` | proven | debug build with corrupted addComponent pattern: teleport=degraded, all other capabilities ok, server kept running, MCP reachability reason 'capabilities not ok: teleport=degraded'; release 0.4.1 restored with all ok. Degraded teleport request not exercised with a player online |

## Known limitations

- **Player name is the Steam persona name**, not the in-game character name (the server never exposes it). `gameId` = SteamID64.
- **Kick/ban need the player online**; unban needs the player to have been seen online once (account-hash cache). Ban reason and expiry are not supported by the game.
- **Whisper isolation is unproven**: whispers reach the recipient, but exclusion of other players was not verified (only one player account; accepted limitation).
- **Admins cannot be kicked/banned by the game itself**: Enshrouded ignores kick/ban for players whose group has `canKickBan`. Plugin >= 0.4.2 lifts that check for each Takaro kick/ban; older plugins return success while nothing happens.
- **listLocations** is implemented (1031 locations) but Takaro never calls it, so it is not observable end-to-end.
- **Log events are not stored** by Takaro as searchable events; they only surface via rate-limit records.
- **Signatures are pinned to game build 1024233 (Steam build 23178631).** Every hook is anchored on code shape and self-checked at load; on a game update a mismatching capability reports `degraded` in `/health` (and reachability reason) while everything else keeps working and the server keeps running. Re-derive signatures after updates — see "Moving to a new game build" above.
- **Catalogue names are the server's own codes with spaces**, not localised display names: no localisation ships with the dedicated server, so `Block_Roof_T5_Granite_Ornamented` becomes `Block Roof T5 Granite Ornamented`. `verify`'s `sidecar-catalog` check enforces `name != code` and no underscores; it cannot enforce what the server does not have.
- Takaro rounds teleport coordinates to integers; the game nudges to a free voxel. `giveItem` stacks are split (game ignores count, max 64).
- **Discord echo**: Takaro core stores its own server messages as player-less chat, and the built-in `chatBridge` relays them back to Discord. Use the `chatBridgeNoEcho` fork (GameToDiscord skips chat without a player).

## Releases

Releases are cut by release-please (`release-please-config.json`, package `games/enshrouded`,
component `enshrouded`, tags `enshrouded-vX.Y.Z`). Merging a conventional commit that touches
`games/enshrouded/**` opens/updates a `chore(main): release enshrouded X.Y.Z` PR; merging that PR
tags the release and bumps both `games/enshrouded/version.txt` and the annotated
`#define TAKARO_PLUGIN_VERSION "X.Y.Z" // x-release-please-version` line in `mod/src/common.h`,
so the plugin's `takaro enshrouded plugin <version> starting` log line always matches the tag.

`.github/workflows/enshrouded.yml` runs the sidecar typecheck and tests plus
`mod/tests/run.sh` on every PR/push, and then hands the packaging to the shared
`connector-release.yml`, which resolves the target, builds it twice and compares the bytes,
and publishes:

- `takaro-enshrouded-plugin-proton-1024233-<v>.zip` — `TakaroEnshrouded/dbghelp.dll` + `README.txt`
- `takaro-enshrouded-sidecar-proton-1024233-<v>.zip` — `TakaroEnshroudedSidecar/` with `dist/`,
  `package.json`, `package-lock.json`, `Dockerfile` (the release one), `.dockerignore`,
  `.env.example`, `README.release.txt`
- a `.meta.json` beside each, `SHA256SUMS`, and the legacy names
  `takaro-enshrouded-plugin.zip` / `takaro-enshrouded-sidecar.zip` as aliases of exactly
  those bytes
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
