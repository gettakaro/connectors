# Conan Exiles connector — development

Developer, architecture and build notes for the Conan Exiles connector. Operator install steps live
in [README.md](README.md).

## Architecture

The connector is a TypeScript sidecar (`bridge/`) plus an optional Conan-side chat renderer.

- Takaro outbound WebSocket for the connector protocol (`wss://connect.takaro.io/`).
- Conan Exiles RCON for commands, player lists and moderation.
- Optional log tailing for `log` events and chat parsing.
- Player polling (`listplayers` deltas) for `player-connected` / `player-disconnected`.
- A local HTTP command bridge that an in-game mod or host-side helper polls to render real chat.
- Optional read-only reads of the Conan save database (`game_0.db`) for location, inventory,
  item/entity/location catalogues.

Layout:

```
games/conan-exiles/
    bridge/                     # the Node.js sidecar (source of truth for behaviour)
    mod/TakaroConanBridge/      # spec + DevKit handoff for the Takaro-owned .pak (no binary shipped)
    scripts/lib-target.sh       # resolves the catalog target every script builds against
    scripts/build-release.sh    # packages the target-qualified bridge zip
    scripts/check-exact-source.mjs  # lockfile and tarball hashes vs the catalog, before npm ci
    TakaroConfig.example.txt
    version.txt
    CHANGELOG.md

catalog/conan-exiles/
    game.json                   # the Steam watch: app 443030, depot 443032, branch public
    targets/linux-25639945.json # the pinned server build, image, deps and file hashes
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
