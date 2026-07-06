# Conan Exiles Takaro Bridge

TypeScript sidecar that connects Conan Exiles dedicated servers to Takaro.

## What It Uses

- Takaro outbound WebSocket for connector protocol.
- Conan Exiles RCON for commands and player lists.
- Optional log tailing for `log` and best-effort chat events.
- Player polling for `player-connected` and `player-disconnected` events.
- Optional Conan-side helper/mod polling the sidecar for real in-game chat messages.

## Conan Server Setup

Install the dedicated server with SteamCMD app `443030`. On Linux, the current dedicated server install includes a native launcher:

```bash
./ConanSandboxServer.sh -log -server -nosteamclient \
  -MULTIHOME=127.0.0.1 \
  -Port=7777 \
  -QueryPort=27015 \
  -RconEnabled=1 \
  -RconPassword=YourRconPassword \
  -RconPort=25575
```

You can also enable RCON in the Conan dedicated server `Game.ini`:

```ini
[RconPlugin]
RconEnabled=1
RconPassword=YourRconPassword
RconPort=25575
```

Conan also accepts equivalent command-line flags:

```powershell
ConanSandboxServer.exe -RconEnabled=1 -RconPassword=YourRconPassword -RconPort=25575
```

Conan has RCON karma protection. Keep `pollIntervalMs` at the default `10000` or higher for normal operation unless you also raise `RconMaxKarma` on a test server.

For high-volume live verification on a disposable test server, add the karma limit to `Game.ini` and restart Conan:

```ini
[RconPlugin]
RconMaxKarma=1000
```

## Bridge Setup

```bash
npm install
cp TakaroConfig.example.txt TakaroConfig.txt
npm run build
npm start
```

Edit `TakaroConfig.txt` before starting.

Required values:

- `registrationToken`
- `serverName`
- `rconHost`
- `rconPort`
- `rconPassword`
- `rconCommandGapMs`

Optional values:

- `identityToken`
- `takaroWsUrl`
- `httpPort`
- `pollIntervalMs`
- `enableLogEvents`
- `logFiles`
- `requireModSourceAttribution`

Useful Conan log paths:

- `ConanSandbox/Saved/Logs/ConanSandbox.log` for player chat and general server logs.
- `ConanSandbox/Saved/Logs/RconCommandLog.log`

## Health

The bridge exposes a local readiness endpoint:

```bash
curl http://127.0.0.1:3010/health
```

The same local HTTP server exposes a mod-facing command bridge:

- `GET /mod/poll` returns the next queued command for a Conan-side helper.
- `POST /mod/result` completes a queued command with `{ "requestId": "...", "result": { ... } }`.
- `POST /mod/event` forwards helper-emitted events to Takaro with `{ "type": "chat-message", "data": { ... } }`.

The helper should poll `http://127.0.0.1:3010/mod/poll` from the server host and render `sendMessage` commands as normal in-game chat, optionally scoped to the `recipient` Steam64 ID.

Takaro-owned Conan mods should identify the poll source so `/health` can distinguish the real `.pak` from a host-side helper:

```text
GET /mod/poll?source=TakaroConan
X-Takaro-Mod-Source: TakaroConan
```

Set `requireModSourceAttribution=true` for final TakaroConan validation. In
that mode `/mod/poll`, `/mod/result`, and `/mod/event` reject anonymous helper
traffic, so a result or chat event cannot be accepted unless it carries
`source=...` or `X-Takaro-Mod-Source`. Ambient User-Agent values are ignored in
strict mode because they are not a durable final-proof source.

`/health` exposes `modBridge.lastPollSource`, `modBridge.lastResultSource`,
`modBridge.lastResultAt`, `modBridge.lastEventSource`,
`modBridge.lastEventAt`, `modBridge.lastEventType`,
`modBridge.recentResults`, `modBridge.recentEvents`, and
`modBridge.sourceAttributionRequired` for this attribution. The recent trace
arrays are bounded and are used by final validation to prove the exact current
message markers were handled by `TakaroConan`, not by stale logs or the old
Pippi/RCON renderer.

For a sidecar-contract diagnostic only, `npm run verify:mod-protocol` temporarily pauses the host poller, queues one Takaro `sendMessage` through MCP, handles it through `/mod/poll` and `/mod/result` as `TakaroConanProtocolProbe/1.0`, posts one `/mod/event`, verifies source attribution, then resumes the host poller. This proves the HTTP contract the future `.pak` must use; it is not installed-mod proof and does not satisfy the final `TakaroConan` source gates.

This package also ships a host-side poller for renderer integration:

```bash
TAKARO_CONAN_RENDER_COMMAND="/path/to/render-conan-chat" npm run mod-helper
```

The renderer command receives the queued message as JSON on stdin and these environment variables:

- `TAKARO_CONAN_REQUEST_ID`
- `TAKARO_CONAN_MESSAGE`
- `TAKARO_CONAN_RECIPIENT`

The poller refuses to start without a renderer so Takaro messages are not acknowledged unless something has actually accepted responsibility for rendering them. On a production Conan server, that renderer must be an in-engine Conan mod/helper, a chat mod with an RCON command API, or an equivalent integration that can call Conan's chat UI/server messaging API. A standalone sidecar process cannot create normal Conan chat lines by itself.

If the server runs a chat mod that exposes RCON commands, the helper can render directly through that mod instead of an external command:

```bash
BRIDGE_CONFIG=/path/to/TakaroConfig.txt \
TAKARO_CONAN_CHAT_MOD=pippi \
npm run mod-helper
```

For Conan Exiles Enhanced, use the Enhanced Pippi workshop item, not the Legacy one:

- Enhanced Pippi workshop ID: `3725018456`
- Legacy Pippi workshop ID `880454836` is opened by the Enhanced Linux server but does not register the Pippi mod controller or `globallink` RCON command.

Server-side Pippi setup used during validation:

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

Supported `TAKARO_CONAN_CHAT_MOD` values:

- `pippi` resolves online character names from `listplayers` and sends Pippi `directmessage <sender> <character> <message>` commands for targeted chat. Server-wide Takaro messages use Enhanced Pippi's `server <message>` command because `globallink` returned OK but did not render as visible client chat during live validation.
- `amunet` sends `ast chat "global" <sender>:<message>` for Amunet Server Transfer style chat.

Optional overrides:

- `TAKARO_CONAN_RCON_HOST`, `TAKARO_CONAN_RCON_PORT`, `TAKARO_CONAN_RCON_PASSWORD`, `TAKARO_CONAN_RCON_TIMEOUT_MS`
- `TAKARO_CONAN_SENDER_NAME`

The helper still refuses to acknowledge queued Takaro messages unless one of these renderers is configured.

## Supported Takaro Functions

The code-level coverage registry lives in `src/takaro/coverage.ts`, and `src/__tests__/coverage.test.ts` fails if any Takaro action or event type is missing from that registry.

Live-supported actions:

- `testReachability`
- `getPlayers`
- `getPlayer`
- `getPlayerLocation` with `databasePath`
- `getPlayerInventory` with `databasePath`
- `listItems` with `databasePath`
- `listEntities` with `databasePath`
- `listLocations` with `databasePath`
- `giveItem` for online players through Conan `con <player> SpawnItem`
- `teleportPlayer` for online players through Conan `con <player> TeleportPlayer`
- `sendMessage`
- `executeConsoleCommand`
- `kickPlayer`
- `banPlayer`
- `unbanPlayer`
- `listBans`
- `shutdown` through RCON. Conan accepts the command and exits asynchronously after a delay.

Takaro schema-valid fallbacks:

- Without `databasePath`, `getPlayerInventory`, `listItems`, `listEntities`, and `listLocations` return `[]`.
- Without `databasePath`, `getPlayerLocation` returns `{ "x": 0, "y": 0, "z": 0 }`.
- `getMapInfo` returns `{ "enabled": false, "mapBlockSize": 0, "maxZoom": 0, "mapSizeX": 0, "mapSizeY": 0, "mapSizeZ": 0 }`.

Explicit unsupported actions:

- `getMapTile`

Unsupported functions return structured errors instead of timing out.

`sendMessage` uses the mod-facing command bridge. If no Conan-side helper is polling `/mod/poll`, the connector returns a clear failure and does not fall back to vanilla RCON `broadcast`. Conan renders `broadcast` as a server-wide popup/overlay, not as a normal chat-feed line.

If Takaro rejects the connector `identify` payload, the bridge records
`takaroIdentifyError` in `/health` and disables reconnect attempts until the
runtime credentials are updated. This prevents a stale registration token from
looping indefinitely against `wss://connect.takaro.io/`.
The current Takaro connector identify contract requires a valid
`registrationToken`; `identityToken` is optional and is not accepted as a
standalone replacement by the live service.
Read-only MCP game-server routes can expose the current `identityToken`, but
they do not expose a usable `registrationToken` for sidecar identify.

## Live Verification

Build first, then run the local live verifier from this directory:

```bash
npm run build
npm run verify:live
```

The script checks the bridge health endpoint, fresh Takaro validation errors in the bridge log, and Conan save DB reads when `databasePath` is configured.
It initializes a Takaro MCP session and runs non-destructive game-server checks against the `gameServerId` from `/health`: reachability, players, bans, map info fallback, map tile unsupported response, and `executeCommand help`.
MCP calls are paced by `TAKARO_CONAN_MCP_ACTION_GAP_MS` and default to a 6 second gap to avoid tripping Conan RCON karma through back-to-back Takaro requests. Do not run multiple live verifiers concurrently against the same Conan test server.

By default, the verifier does not run the direct RCON `help` sweep because Conan's RCON karma system can deny repeated localhost probes. To include non-destructive RCON `help` probes for teleport, position, inventory, items, bans, Pippi `server`, and Pippi `directmessage`, use:

```bash
TAKARO_CONAN_RUN_RCON_PROBES=1 TAKARO_CONAN_VERIFY_SEND_MESSAGE=0 npm run verify:live
```

Keep `TAKARO_CONAN_VERIFY_SEND_MESSAGE=0` when you do not want the verifier to emit a chat line through the currently installed chat bridge.

Run these Takaro MCP checks with the `gameServerId` reported by `/health`:

- `gameserverGetPlayers`
- `gameserverListBans`
- `gameserverSendMessage`
- `gameserverExecuteCommand` with `help`

Do not run destructive checks such as live kick, ban, shutdown, teleport, or inventory mutation against an active player without explicit approval. For mutation smoke tests, use a known online test player and a harmless item/coordinate.

## Known Gaps

Conan RCON output is not consistently documented across server versions. The bridge includes tolerant parsers; current live QA has captured populated `listplayers` and both populated and empty `listbans` output for this server.

Incoming player chat is parsed from configured logs. The parser supports the live `ChatWindow: Character <name> (uid <id>, player <steam64>) said: <message>` format seen on a real server, plus simpler best-effort chat formats. A Conan-side helper can also forward richer chat events through `/mod/event`.

`databasePath` enables read-only Conan save DB state for location, inventory, discovered item templates, discovered actor classes, and player location rows. `giveItem` and `teleportPlayer` require an online player because they use Conan's `con <online player> <client command>` relay. `player-death` and player-attributed `entity-killed` are best-effort log-derived events from Conan `KillCharacterWithRagdoll_Implementation` lines.

## Takaro-Owned Conan Mod Path

The current visible chat bridge is still Enhanced Pippi backed. The Takaro-owned replacement path is tracked in `../takaro-mod/`:

- `MOD_SPEC.md` defines the required minimal `TakaroConan.pak` behavior.
- `BUILD_ENVIRONMENT.md` defines the Conan DevKit/cook toolchain gate.
- `INSTALL_RECONNECT_LIVE_TEST.md` defines the Pippi replacement, client relogin, and live validation loop.
- `API_COVERAGE_BOUNDARY.md` separates connector-owned Takaro actions/events from wider Takaro MCP platform tools.
- `COMPLETION_CHECKLIST.md` is the final done checklist for the server+client mod goal.
- `DEVKIT_IMPLEMENTATION_NOTES.md` captures the source-attributed DevKit implementation contract.

Read-only local gates:

```bash
bash ../scripts/check-mod-toolchain.sh
bash ../scripts/check-takaro-mod-install.sh
bash ../scripts/audit-takaro-mod-goal.sh
```

The first gate must pass before this machine can build a cooked Conan `.pak`. The second gate must pass before claiming the Takaro-owned mod is installed and replacing Pippi.
The audit gate runs syntax, build/tests, safe MCP live verification, toolchain, install, and live mod gates. It must pass before the full Takaro Conan mod goal can be called done.
