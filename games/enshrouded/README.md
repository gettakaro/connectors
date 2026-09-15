# Takaro Enshrouded Connector

Connects an Enshrouded dedicated server to Takaro via the Generic Connector Protocol.

## What works / what doesn't

Plain summary for server owners. ✅ = tested live end-to-end through Takaro and checked in the game; ⚠️ = works only partly or could not be fully checked; ❌ = does not work. Tested on game build 1024233 with plugin 0.4.2 (details: capabilities table below).

| Feature | Status | Note |
|---|---|---|
| Server shows as online (reachability) | ✅ Works | |
| List online players | ✅ Works | Player name is the Steam name, not the character name. |
| Look up a single player | ✅ Works | |
| Player position | ✅ Works | |
| Player inventory | ✅ Works | |
| Give items | ✅ Works | Tested on a normal player and an Admin; items survive a server save and restart. |
| Item list | ✅ Works | 3609 items. |
| Entity list | ✅ Works | 979 creature/NPC types (fixed list, not live creatures). |
| Location list | ⚠️ Partial | The connector has 1031 map locations, but Takaro never asks for them, so they cannot be tested. |
| Console commands | ✅ Works | Enshrouded has no console; the connector adds its own commands (`help`, `say`, `teleport`, `give`, `kick`, …). No time-of-day command. |
| Send chat message (everyone) | ✅ Works | Shows in chat under the name of an online player (the game has no "server" sender). |
| Private message (whisper) | ⚠️ Partial | Arrives at the right player. It could not be checked that other players do not see it (only one test account). |
| Teleport player | ✅ Works | Coordinates are rounded; the game moves the player to the nearest free spot. |
| Kick player | ✅ Works | Also works on players in an admin group. The game itself refuses to kick admins, so the connector lifts that for the one kick (fixed in plugin 0.4.2). |
| Ban player | ✅ Works | Player must be online. Bans are permanent until unbanned: the game has no ban reason or end date. |
| Unban player | ✅ Works | The player must have joined at least once while the connector was running. |
| List bans | ✅ Works | |
| Shut down server | ✅ Works | Saves first, then quits (the container restarts it). |
| Event: player joined | ✅ Works | |
| Event: player left | ✅ Works | Also sent when the server crashed with players online. |
| Event: chat message | ✅ Works | |
| Event: player died | ✅ Works | The killer's name for deaths by creatures is new and not re-tested live. |
| Event: creature killed | ✅ Works | Weapon name is not filled in. |
| Event: server log lines | ⚠️ Partial | Sent to Takaro, but Takaro does not store log lines, so they cannot be looked up. |
| Modules: chat commands | ✅ Works | Commands with `@`, `/` or `!` in game chat. |
| Modules: hooks | ✅ Works | |
| Modules: cronjobs | ✅ Works | |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ Works | |
| Shop: buy in game (`@shop`) | ✅ Works | Currency is taken and the items appear in the inventory. |
| Shop: order via Takaro + claim | ✅ Works | Orders placed while offline stay paid and can be claimed once the player is online. |
| Shop: not enough currency | ✅ Works | Purchase refused, nothing taken. In game the message is a generic "Oops, something went wrong". |
| Economy: balance / top list | ✅ Works | `@balance`, `@topcurrency`. |
| Economy: transfer to another player | ⚠️ Partial | Only tested sending to yourself (one test account). |
| Shop: `@claim` in game | ⚠️ Partial | Needs the player's account linked to Takaro; claiming via Takaro works. |
| Discord: game chat → Discord | ✅ Works | Seen in the Discord channel. |
| Discord: Discord → game chat | ✅ Works | A real Discord message showed up in game chat. |
| Discord: join/leave notices in Discord | ✅ Works | Seen in the Discord channel. |
| Discord: no echo of server messages | ✅ Works (with `chatBridgeNoEcho`) | The built-in `chatBridge` posts server messages back to Discord ("Non-player: …"). That is a Takaro bug affecting every game. Install the `chatBridgeNoEcho` module instead of `chatBridge`: tested, no echo, player chat still goes through. |
| Reconnects after a server/container restart | ✅ Works | Takes up to ~3 minutes. |
| No duplicate events after a connector restart | ✅ Works | |
| Keeps running after a game update breaks a feature | ✅ Works | Only the affected feature is switched off and reported; everything else keeps working. |

## Architecture

Enshrouded has no modding API, RCON or scripting. The connector has two parts:

```
enshrouded_server.exe (Wine/Proton, container)
  └─ dbghelp.dll  ← mod/ (C++17, MinHook). Proxy DLL loaded by the server.
       ├─ resolves game functions by string-xref + prologue signatures (never fixed RVAs)
       ├─ tails game state (players, chat, deaths, kills) into an event ring buffer
       └─ HTTP API on 127.0.0.1:18890 (Bearer token), see mod/docs/API.md
sidecar/ (Node 22, TypeScript) — runs in the game container's network namespace
  ├─ polls plugin events (persisted cursor + bootId, no replay on restart)
  ├─ reconciles online players (emits disconnects after a server crash)
  └─ outbound WebSocket to Takaro (identify, action requests, events)
```

- `mod/`: `src/` source, `third_party/minhook` (vendored, see VENDORED.txt), `tools/gen_gamedata.py` (item/entity/location tables from the server kfc), `tests/` (host-side correlator test).
- `sidecar/`: the Takaro bridge, unit tests (vitest) and a mock plugin (`npm run mock-plugin`).
- `scripts/validate-module-proof.mjs`: validates a module live-proof JSON file.

## Build

```bash
# plugin: cross-compile dbghelp.dll with zig 0.13 (set ZIG=/path/to/zig if not on PATH)
./mod/build.sh                 # -> mod/build/dbghelp.dll
./mod/tests/run.sh             # host-side plugin tests (needs docker)

# sidecar
cd sidecar && npm ci && npm run typecheck && npm test && npm run build
```

`DEBUG_CORRUPT_SIG=<signature name> ./mod/build.sh` builds a debug DLL into `mod/build-debug/` with one
signature corrupted, to exercise the degrade self-check.

## Deploy (Docker)

`docker-compose.example.yml` runs `mornedhels/enshrouded-server` with `WINEDLLOVERRIDES=dbghelp=n,b` and the
plugin bind-mounted read-only (SteamCMD updates cannot overwrite it), plus the sidecar built from `sidecar/`
with `network_mode: service:enshrouded` (the plugin API is never exposed on the host).

```bash
cd enshrouded
cp .env.example .env              # fill in tokens and role passwords
./mod/build.sh
mkdir -p data/enshrouded-plugin && cp mod/build/dbghelp.dll data/enshrouded-plugin/
docker compose -f docker-compose.example.yml --env-file .env up -d --build
docker compose -f docker-compose.example.yml logs -f
```

To update the plugin later: rebuild, stop the game container, replace `data/enshrouded-plugin/dbghelp.dll`,
start it again (the server holds the DLL open while running).

UDP 15637 is published for clients. Check `GET /health` from inside the container for per-capability status.

## Environment

| Variable | Where | Purpose |
|---|---|---|
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
- **Signatures are pinned to game build 1024233 (Steam build 23178631).** Every hook is anchored on code shape and self-checked at load; on a game update a mismatching capability reports `degraded` in `/health` (and reachability reason) while everything else keeps working and the server keeps running. Re-derive signatures after updates.
- Takaro rounds teleport coordinates to integers; the game nudges to a free voxel. `giveItem` stacks are split (game ignores count, max 64).
- **Discord echo**: Takaro core stores its own server messages as player-less chat, and the built-in `chatBridge` relays them back to Discord. Use the `chatBridgeNoEcho` fork (GameToDiscord skips chat without a player).
