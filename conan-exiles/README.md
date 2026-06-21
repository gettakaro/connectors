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

Optional values:

- `identityToken`
- `takaroWsUrl`
- `httpPort`
- `pollIntervalMs`
- `enableLogEvents`
- `logFiles`

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

- `pippi` resolves online character names from `listplayers` and sends Pippi `directmessage <sender> <character> <message>` commands. Server-wide Takaro messages are sent once to every online character because Enhanced Pippi's `globallink` RCON command is internal and did not render as visible client chat during live validation.
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
- `shutdown`

Takaro schema-valid fallbacks:

- Without `databasePath`, `getPlayerInventory`, `listItems`, `listEntities`, and `listLocations` return `[]`.
- Without `databasePath`, `getPlayerLocation` returns `{ "x": 0, "y": 0, "z": 0 }`.
- `getMapInfo` returns `{ "enabled": false, "mapBlockSize": 0, "maxZoom": 0, "mapSizeX": 0, "mapSizeY": 0, "mapSizeZ": 0 }`.

Explicit unsupported actions:

- `getMapTile`

Unsupported functions return structured errors instead of timing out.

`sendMessage` uses the mod-facing command bridge. If no Conan-side helper is polling `/mod/poll`, the connector returns a clear failure and does not fall back to vanilla RCON `broadcast`. Conan renders `broadcast` as a server-wide popup/overlay, not as a normal chat-feed line.

## Live Verification

Build first, then run the local live verifier from this directory:

```bash
npm run build
npm run verify:live
```

The script checks the bridge health endpoint, non-destructive RCON `help` probes for teleport, position, inventory, items, bans, Pippi `server`, and Pippi `directmessage`, and fresh Takaro validation errors in the bridge log.

Run these Takaro MCP checks with the `gameServerId` reported by `/health`:

- `gameserverGetPlayers`
- `gameserverListBans`
- `gameserverSendMessage`
- `gameserverExecuteCommand` with `help`

Do not run destructive checks such as live kick, ban, shutdown, teleport, or inventory mutation against an active player without explicit approval. For mutation smoke tests, use a known online test player and a harmless item/coordinate.

## Known Gaps

Conan RCON output is not consistently documented across server versions. The bridge includes tolerant parsers, but real `listplayers` and `listbans` output from populated servers should be captured during first server testing.

Incoming player chat is parsed from configured logs. The parser supports the live `ChatWindow: Character <name> (uid <id>, player <steam64>) said: <message>` format seen on a real server, plus simpler best-effort chat formats. A Conan-side helper can also forward richer chat events through `/mod/event`.

`databasePath` enables read-only Conan save DB state for location, inventory, discovered item templates, discovered actor classes, and player location rows. `itemCatalogPath` enables readable labels and aliases for item grants, inventory reads, and item picker lists. `giveItem` and `teleportPlayer` require an online player because they use Conan's `con <online player> <client command>` relay.
