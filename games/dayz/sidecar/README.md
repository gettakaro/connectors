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

The mod's `profiles/TakaroIntegration/config.json` should have `TakaroApiUrl=http://localhost:<localPort>`.

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

## Verified

Live tested against `wss://connect.takaro.io/` on 2026-04-30:
- WS open + identify + identifyResponse with gameServerId
- Mod registers via local HTTP, polls cleanly (200/poll cycles)
- Inbound Takaro request → mod dispatch → result relay path exercised in the previous run (showed up as Takaro DTO validation errors which drove the shape adapters)

## Limitations

- The bridge is loopback-only (`127.0.0.1`). If you need it reachable from another box, add `host=0.0.0.0` to the local listener and add a firewall rule.
- No persistence: queued operations are in-memory. If the bridge crashes mid-cycle, in-flight operations are lost. Takaro retries reachability checks automatically; other operations would need to be re-issued from the dashboard.
- Chat messages still need a CF/VPP compat addon on the mod side to route in-game chat events. The bridge will forward them once they show up.
