# DayZ-Bridge

Sidecar Node.js process that bridges the in-game **`@TakaroIntegration`** mod to **Takaro** at `wss://connect.takaro.io/`.

## Why a sidecar

DayZ's Enforce Script `RestApi`/`RestContext` is HTTP-only — there's no WebSocket support in-engine. Takaro's gameserver protocol is WebSocket-only (the Enshrouded, Palworld, Soulmask bridges all use it).

The bridge translates:

```
DayZ mod (HTTP)  ⇄  DayZ-Bridge  ⇄  Takaro (WSS)
```

The mod stays a clean in-game data source; the bridge handles the protocol gap and shape coercion.

## Run

```bash
npm install
npm run build
# Edit TakaroConfig.txt with your registration token from Takaro dashboard
node dist/index.js
```

## Config

`TakaroConfig.txt` (key=value, one per line):

| Key | Required | Default |
| --- | --- | --- |
| `registrationToken` | yes | — |
| `serverName` | yes | — |
| `identityToken` | no | falls back to `serverName` |
| `takaroWsUrl` | no | `wss://connect.takaro.io/` |
| `localPort` | no | `8088` |

The mod's `profiles/TakaroIntegration/config.json` should have `TakaroApiUrl=http://127.0.0.1:<localPort>` and a
non-empty `IdentityToken` or `RegistrationToken`. With both tokens empty the mod stays idle after one warning line.
The poll and flush keys are `CommandPollIntervalMs` and `EventBatchIntervalMs`.

### Environment variables

Every file key can also be set from the environment, and the environment wins. With the environment complete,
`TakaroConfig.txt` is optional, which suits Docker and panel hosting.

| Variable | Replaces | Notes |
| --- | --- | --- |
| `TAKARO_REGISTRATION_TOKEN` | `registrationToken` | |
| `TAKARO_SERVER_NAME` | `serverName` | |
| `TAKARO_IDENTITY_TOKEN` | `identityToken` | |
| `TAKARO_WS_URL` | `takaroWsUrl` | |
| `BRIDGE_LOCAL_PORT` | `localPort` | |
| `BRIDGE_BIND_ADDR` | `bindAddress` | Default `127.0.0.1`. |
| `BRIDGE_CONFIG` | | Path to the config file. |
| `BRIDGE_POLL_WATCHDOG_SEC` | | Exit if the mod stopped polling for this long, so a supervisor restarts the bridge. `0` disables it. |
| `DAYZ_LOG_DIR` | | Server profiles directory. When set, new RPT and script log lines are sent as Takaro `log` events. |
| `DAYZ_LOG_DENY` | | Comma-separated regexes of log lines to skip. |
| `DAYZ_LOG_MAX_PER_SEC` | | Rate limit for `log` events. Default `20`. |
| `DEBUG` | | `1` logs every WebSocket frame, with tokens redacted. |

## What the bridge does

1. Connects WS to Takaro, sends `identify` with registrationToken + serverName, receives `gameServerId`.
2. Listens on `127.0.0.1:<localPort>` for the mod:
   - `POST /gameserver/register` — returns the cached `{identityToken, gameServerId}` so the mod skips its own registration round-trip.
   - `POST /gameserver/<id>/events` — each event in the batch is forwarded as a Takaro `gameEvent` over WS.
   - `GET /gameserver/<id>/poll` — drains a queue of Takaro requests.
   - `POST /gameserver/<id>/operation/<opId>/result` — relayed to Takaro as a `response` (or `error`) message.
3. Reverse direction: incoming Takaro `request` WS messages → queued for the next mod poll.
4. **Per-action shape coercion** (Enforce Script JSON quirks):
   - Booleans serialized as `0`/`1` are coerced back to `false`/`true` for known fields (`testReachability.connectable`, player `online`).
   - Wrapped lists like `{players:[...]}` are unwrapped to bare arrays for `getPlayers`, `listPlayers`, `listBans`, `listItems`.
5. Auto-reconnects with 5s backoff on WS close.
6. Stamps every forwarded event with the wall-clock UTC time it was received. The mod's own clock is the in-game
   world time.
7. Returns `503` on `POST .../events` while the Takaro WebSocket is not connected and identified. The mod keeps the
   batch and retries, so events are delivered once after a reconnect instead of being dropped.
8. Passes only the player's `gameId` to the mod for player-targeted actions. Takaro sends the whole player record,
   which is long enough that Enforce Script truncates the argument string.
9. `/health` reports `identified`, `wsConnected`, `lastModContactAt`, `lastPollAt`, `lastEventAt`,
   `eventsForwarded`, `eventsDeferred` and `logsForwarded`.

## Linux hosting (Docker)

Tested with the DayZ Linux server 1.29 in one container and the bridge in a second container that shares its network
namespace (`network_mode: service:<dayz>`). The mod then reaches the bridge on `127.0.0.1` and nothing is published.

- Do not mount the `@TakaroIntegration` folder read-only. With a `:ro` mount the server loads nothing from `Addons/`
  and logs no error.
- If the game container is recreated on its own, the bridge keeps the old network namespace and every poll fails.
  Recreate both together, or set `BRIDGE_POLL_WATCHDOG_SEC` with a restart policy.
- Folder and file casing of the mod did not matter on this build. `-servermod=`, `-serverMod=` and `-mod=` all
  loaded it.
- RestApi callback codes: `5` client error (4xx), `6` server error (5xx), `7` app error, `8` timeout. A code `8` means
  the mod loaded and could not reach the bridge in time, which points at networking.

## Verified

Live tested against `wss://connect.takaro.io/` on 2026-04-30:
- WS open + identify + identifyResponse with gameServerId
- Mod registers via local HTTP, polls cleanly (200/poll cycles)
- Inbound Takaro request → mod dispatch → result relay path exercised in the previous run (showed up as Takaro DTO validation errors which drove the shape adapters)

## Limitations

- The bridge is loopback-only (`127.0.0.1`). If you need it reachable from another box, add `host=0.0.0.0` to the local listener and add a firewall rule.
- No persistence: queued operations are in-memory. If the bridge crashes mid-cycle, in-flight operations are lost. Takaro retries reachability checks automatically; other operations would need to be re-issued from the dashboard.
- Chat messages still need a CF/VPP compat addon on the mod side to route in-game chat events. The bridge will forward them once they show up.
