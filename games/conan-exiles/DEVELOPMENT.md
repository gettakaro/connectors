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
    scripts/build-release.sh    # packages takaro-conan-exiles-bridge.zip
    TakaroConfig.example.txt
    version.txt
    CHANGELOG.md
```

## Conan server setup for development

Install the dedicated server with SteamCMD app `443030`. On Linux the current dedicated server
install includes a native launcher:

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

`scripts/build-release.sh <version> <out-dir>` produces `takaro-conan-exiles-bridge.zip`
containing `dist/`, `scripts/`, `package.json`, `package-lock.json`, `README.md`,
`TakaroConfig.example.txt` and a generated `README.release.txt`. The zip contains no `src/`, so
every runtime `package.json` script points at compiled `dist/` output (`npm start` ->
`dist/index.js`, `npm run mod-helper` -> `dist/mod/pollerCli.js`) and runs under
`npm ci --omit=dev`; the script fails the build if either entrypoint is missing from the package.
`npm run mod-helper:dev` is the `tsx` source variant for local development only. CI runs it from
`.github/workflows/conan-exiles.yml` (`test` job: `npm ci`, `npm test`, `npm run build`;
`package` job: build + publish stable / rolling `conan-exiles-dev` / per-PR assets).

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
- `devkit-handoff/` — blueprint, implementation plan, build contract and PowerShell build helpers.

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
