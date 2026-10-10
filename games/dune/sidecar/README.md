# Takaro connector for Dune: Awakening (sidecar)

## Install

1. Download `takaro-dune-sidecar-<version>.tar.gz` from the latest release and unpack it, or pull the image.
2. Copy `.env.example` to `.env` and fill in:
   - `TAKARO_REGISTRATION_TOKEN` — from Takaro, for a **Generic** game server.
   - `DUNE_PG_URL` — the battlegroup's Postgres. Read-only; the connector writes nothing to the database. The
     database **name** differs between builds (`dune`, `dune_sb_1_4_0_0`, …), so look it up with `\l`.
   - `DUNE_RMQ_URL` — the **game** RabbitMQ (AMQP over TLS, `DUNE_RMQ_TLS_INSECURE=true` for the shipped
     self-signed certificate).
   - `DUNE_GM_AUTH_TOKEN` — the same value as the map process's `ServerCommandsAuthToken`.
   - `DUNE_GM_PUBLISHER` — `amqp` if the sidecar can log in to the game broker as the `fls` user, otherwise
     `docker-exec` (`DUNE_RMQ_CONTAINER`) or `kubectl-exec` (`DUNE_K8S_NAMESPACE`/`DUNE_K8S_POD`).
3. Every map process must be started with **both** gates, or no server command has any effect:
   - `-ini:engine:[ConsoleVariables]:server.NotificationSystem.Enabled=true`
   - `-ini:engine:[FuncomLiveServices]:ServerCommandsAuthToken=<DUNE_GM_AUTH_TOKEN>`
4. Run it on the battlegroup's network, with a persistent volume on `/data` (bans and cursors live there):

   ```bash
   docker run -d --name takaro-dune --env-file .env \
     -v "$PWD:/takaro-config:ro" -e TAKARO_CONFIG_FILE=/takaro-config/.env \
     --network <battlegroup-network> -v takaro-dune-data:/data \
     ghcr.io/gettakaro/takaro-dune-sidecar:<version>
   ```

   A compose fragment is in `docker-compose.example.yml`.
5. Check `http://127.0.0.1:18891/health`: `capabilities` names the active GM publisher, whether the chat consumer is
   bound, and whether the optional native plugin is present. No secrets appear in it.

No native plugin is required. Installing it (`DUNE_PLUGIN_URL`) only adds the rows marked "plugin only" below.

## What works

Nothing in this table has been verified against a live Dune server yet — the connector was built and tested entirely
against an in-memory battlegroup (124 unit tests, `npm test`). **Every row is therefore `untested`**, and the table will
be filled in from the hard-test run, not from unit tests.

| Capability | Status | Notes |
|---|---|---|
| testReachability | untested | `farm_state` alive ∧ ready, plus RabbitMQ and GM-token state in the reason |
| getPlayers | untested | `player_state.online_status = 'Online'` |
| getPlayer | untested | answers a real IGamePlayer for online, offline and never-seen players |
| getPlayerLocation | untested | plugin (live) → last chat `m_OriginLocation` (live) → last **saved** pawn transform |
| getPlayerInventory | untested | `inventories` type 0 + gear types ⋈ `items`, stacks aggregated |
| giveItem | untested | GM `AddItemToInventory`, read back from the DB; `verified:false` when not observed in time |
| listItems | untested | placeholder catalogue only; the generator has not run yet |
| listEntities | untested | placeholder catalogue only |
| listLocations | untested | map partitions + static markers |
| executeConsoleCommand | untested | connector command set + `gm <ServerCommand> <json>`; Dune has no console |
| sendMessage (global) | untested | `chat.map` fan-out, optionally/also a `ServiceBroadcast` |
| sendMessage (whisper) | untested | `chat.whispers` with a temporary bind of `<FLS>_queue` |
| teleportPlayer | untested | GM `TeleportToExact`; read-back needs the plugin |
| kickPlayer | untested | GM `KickPlayer`, verified by the player leaving the online set |
| banPlayer / unbanPlayer / listBans | untested | **connector-enforced**: Dune has no native ban, so a banned player is kicked on sight and timed bans are lifted by the connector |
| shutdown | untested | GM `ServerShutdown` notice, then `DUNE_SHUTDOWN_CMD`; refuses without a hook |
| player-connected / player-disconnected | untested | roster diff with a map-transfer grace window |
| chat-message | untested | `chat.intercept` consumer |
| player-death | untested | `life_state` edge; killer/weapon attribution needs the plugin |
| entity-killed | untested | **plugin only** — no out-of-process source exists |
| log | untested | optional tail of the map-server log, redacted |

Known limits by design, independent of testing:

- `ip` and `ping` are never reported: nothing outside the game process exposes them.
- The saved pawn position can be far from where a player actually is. Without the plugin, a live position is only
  known for players who have chatted recently.
- Events are produced by polling, so they are up to one `DUNE_PRESENCE_INTERVAL_MS` late.
