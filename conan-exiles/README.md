# Conan Exiles Connector

TypeScript sidecar connecting a Conan Exiles dedicated server to Takaro.

## Architecture

The sidecar owns all secrets and server integrations:

- Takaro Generic Connector WebSocket
- Conan RCON commands and player discovery
- optional read-only save-database queries
- dedicated-server log tailing for chat, lifecycle, death, and kill events
- local `/health` diagnostics

Normal player-visible Takaro chat additionally requires the Takaro-owned
`TakaroConan.pak` on both server and clients. The sidecar dispatches:

```text
con <player-index> dc TakaroChat "<sender>" "<encoded-message>"
```

The mod decodes the message and uses a reliable owning-client RPC to render a
normal line in Conan's real chat widgets. Server-wide delivery enumerates all
online players; targeted delivery resolves one stable Steam identity. The pak
contains no Takaro token or RCON credential and performs no HTTP polling.

Inbound chat remains server-owned: the sidecar parses the vanilla
`ChatWindow: Character <name> (uid <id>, player <steam64>) said: <message>` log
format and attaches stable Steam identity before emitting `chat-message`.

The legacy `/mod/poll`, `/mod/result`, and `/mod/event` HTTP helper remains for
diagnostics and compatibility. It is not the production `TakaroConan` render
path. Pippi and Amunet are not required.

## Server setup

Install the Conan dedicated server with SteamCMD app `443030`. A typical Linux
launch is:

```bash
./ConanSandboxServer.sh -log -server -nosteamclient \
  -MULTIHOME=0.0.0.0 -Port=7777 -QueryPort=27015 \
  -RconEnabled=1 -RconPassword=YourRconPassword -RconPort=25575
```

RCON can also be configured in `Game.ini`:

```ini
[RconPlugin]
RconEnabled=1
RconPassword=YourRconPassword
RconPort=25575
```

Conan applies RCON karma limits. Keep `pollIntervalMs` at `10000` or higher for
normal operation. The connector reuses one authenticated RCON connection and
serializes commands through `rconCommandGapMs`.

## Connector setup

```bash
npm ci
cp TakaroConfig.example.txt TakaroConfig.txt
npm run build
npm start
```

Required configuration:

- `registrationToken`
- `serverName`
- `rconHost`, `rconPort`, and `rconPassword`

Important optional configuration:

- `enableTakaroClientModChat=true` enables the installed-mod DataCmd transport.
- `databasePath` enables read-only save-database actions.
- `itemCatalogPath` loads an approved DevKit-exported item catalog.
- `logFiles` selects Conan logs to tail.
- `enableLogEvents` controls generic log emission.
- `requireModSourceAttribution` applies only to the legacy HTTP helper.

The health endpoint is available at `http://127.0.0.1:3010/health`. Its
`clientModChatBridge` object reports configuration, selected targets, last
dispatch acceptance, and the honest `deliveryVerified=false` limitation.
RCON acceptance proves dispatch only; visible client render is verified through
the Conan client log or an in-game screenshot.

## Installing TakaroConan

Install the same validated `TakaroConan.pak` on the dedicated server and every
joining client. Both modlists must contain exactly:

```text
*TakaroConan.pak
```

The current source/build contract is under
[`mod/TakaroConanBridge/`](mod/TakaroConanBridge/). In particular:

- [`README.md`](mod/TakaroConanBridge/README.md) describes ownership and runtime boundaries.
- [`MOD_SPEC.md`](mod/TakaroConanBridge/MOD_SPEC.md) defines the DataCmd/client-RPC behavior.
- [`devkit-handoff/`](mod/TakaroConanBridge/devkit-handoff/) contains build-host templates and source evidence requirements.

Cooked `.pak` files and local credentials are deployment artifacts and are not
committed to this repository.

## Supported Takaro surface

The canonical classification lives in `src/takaro/coverage.ts`.

Live-supported actions include player discovery, reachability, console
commands, bans, shutdown, online-player `giveItem` and `teleportPlayer`, and
`sendMessage`. With `databasePath`, location, inventory, item, entity, and
location-list reads use the Conan save database.

Schema fallbacks are returned where Takaro requires a DTO but Conan cannot
provide live data. `getMapInfo` reports a disabled map. `getMapTile` is
explicitly unsupported.

Events include `player-connected`, `player-disconnected`, `chat-message`,
`player-death`, player-attributed `entity-killed`, and `log`.

Unsupported operations return structured errors instead of timing out.

## Item catalog

The built-in seed catalog keeps common items human-readable while preserving
numeric template IDs. A larger approved DevKit export can be converted with:

```bash
npm run prepare:item-catalog -- /path/to/devkit-export.json /path/to/conan-items.json
```

Set `itemCatalogPath` to the generated file. Do not vendor unlicensed community
item databases.

## Verification

Local verification:

```bash
npm test
npm run build
./scripts/build-release.sh 1.0.0 /tmp/conan-release
```

With a configured live runtime:

```bash
npm run verify:live
```

The strongest chat proof is:

```text
Takaro action
  -> connector DataCmd RCON command
  -> TakaroConan reliable client RPC
  -> client render log and visible chat line
```

Do not claim client delivery from an RCON `Successfully executed` response
alone. Do not run kick, ban, shutdown, teleport, or inventory mutations against
an active player without explicit approval.

## Known limitations

- Targeted negative isolation requires two simultaneous clients to prove that a
  non-target does not receive the line.
- The current protocol has no client render acknowledgement, so successful
  results retain `deliveryVerified=false`.
- `getMapTile` is unsupported and `getMapInfo` is a schema fallback.
