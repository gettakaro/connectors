# Takaro Enshrouded plugin: HTTP API (contract v0.4)

The plugin is `dbghelp.dll`, a proxy DLL loaded by `enshrouded_server.exe`. It serves this API on
`127.0.0.1:18890`, inside the server's network namespace (the container). The sidecar codes against this document.

## Transport and auth
- HTTP/1.1. Every response closes the connection (`Connection: close`). Bodies are JSON (`Content-Type: application/json`) and UTF-8.
- Every request needs the header `Authorization: Bearer <token>`.
  - The token comes from env `TAKARO_PLUGIN_TOKEN`, or from `<server dir>\takaro\plugin.json` as `{"token":"..."}`.
  - A missing or wrong token returns `401 {"error":"unauthorized"}`.
  - If no token is configured at all, every request returns `401 {"error":"plugin token not configured"}`.
- Errors always look like `{"error":"<message>"}`:
  - `400`: bad body or missing field
  - `404`: unknown path, or player not online
  - `405`: known path, wrong method
  - `413`: body larger than 1 MiB
  - `501 {"error":"unimplemented"}`: the capability is not wired yet
  - `503`: the game thread is unavailable
- POST bodies are validated before the 501 check, so a malformed request returns 400 even for unimplemented actions.

## Identity
`gameId` is the player's **SteamID64 as a decimal string**, the same value as `steamId`. `name` is the Steam persona name as the server prints it. It is not the in-game character name, which the server never logs.

## Endpoints

### GET /health
```json
{"status":"ok","version":"0.2.0","gameBuild":"1024233",
 "capabilities":{"logEvents":"ok","players":"ok","gameThread":"ok","teleport":"unimplemented", ...},
 "capabilityDetails":{"players":"log-derived (online peer + login lines)", ...},
 "diagnostics":{"tickCount":2278,"tickThreadId":204,"tickThreadChanges":0,"logLines":290,
                "imageTimestamp":"0x69fdecc9","resolved":[{"name":"logSink","rva":"0x4df6b0","hooked":true,"how":"..."}]}}
```
- `capabilities` values are `"ok"`, `"degraded"` or `"unimplemented"`.
- A capability is `degraded` when its signature did not resolve or its hook failed. The reason is in `capabilityDetails`. The server keeps running either way.
- `gameBuild` is the numeric build from the header of `enshrouded_server.kfc`.
- Current capability names:
  - `logEvents`, `players`, `gameThread`, `kick`, `ban`, `unban`, `listBans`, `shutdown`,
    `playerLocation`, `playerInventory`, `teleport`, `giveItem`, `sendMessage`, `chatEvents`, `deathEvents`, `killEvents`,
    `listItems`, `listEntities`, `listLocations`, `executeCommand` (all implemented in v0.4).
  - `ok` means "resolved and hooked" (world capabilities additionally wait for the first server tick), not "proven";
    live proof is in `context/games/enshrouded/evidence/` and `API_GOAL_MATRIX.json`.
- `capabilityDetails` and `diagnostics` are informational. Do not code against their exact shape.

### GET /players
Returns the players who are online, meaning logged in with their permission block received.
```json
[{"gameId":"7656119XXXXXXXXXX","name":"Limon","steamId":"7656119XXXXXXXXXX","peerId":"0(1)",
  "group":"Friends","permissions":["CanAccessInventories","CanEditBase","CanEditWorld","CanExtendBase","CanReceiveEXP"],
  "connectedAt":"2026-09-13T18:22:37.101Z","online":true}]
```
- `group` is the `userGroups[].name` from `enshrouded_server.json` whose `canKickBan`, `canAccessInventories`, `canEditWorld`, `canEditBase` and `canExtendBase` flags exactly match the logged permissions. It is `"unknown"` when no group matches.

### GET /players/{gameId}
Returns the same object, or `404 {"error":"player not online"}`.

### GET /players/{gameId}/location
Returns `{x,y,z}` in metres (**y is up**), or 404 if the player is not online, 503 if the player has no spawned
entity yet. Read from the player entity's `CurrentTransform` (WorldPosition = 3 x int64 32.32 fixed point).
`{gameId}` may also be the exact player name.

### GET /players/{gameId}/inventory
Returns `[{code,name,amount,inventory,slot,quality?,rarity?}]`:
- `inventory` is the linked inventory category (`equipment`, `generic` = backpack and action bars, `customization`, ...).
- `quality` (item level) and `rarity` are present for unique items (PIDE entities: weapons, armour, tools).
- `code` is the item's `debugName` (see `GET /items`); an unknown item id is returned as `0x%08x`.

### GET /events?since=<seq>[&limit=<n>]
```json
{"seq":328,"latestSeq":330,"truncated":false,
 "events":[{"seq":328,"type":"player-connected","data":{"player":{...player object...}},"ts":"2026-09-13T18:22:37.107Z"}]}
```
- The response (and `/health`) carries `bootId` (plugin >= 0.4.1): a random id per server process. A different `bootId` means the server restarted; reset the cursor to 0 even if `seq` is higher than yours.
- The server keeps a ring buffer of the last 5000 events. `seq` is monotonic, starts at 1, and resets when the server restarts.
- The response returns events with `seq > since`, oldest first, capped at `limit` (default and maximum 5000).
- Pass the response `seq` back as `since` on the next call. It is the seq of the last returned event, or the current latest seq when nothing is new.
- If `since` is greater than `latestSeq`, the server has restarted. Reset to `since=0`.
- `truncated: true` means events between `since` and the oldest buffered event were dropped.
- Event types and `data`:
  - `player-connected` and `player-disconnected`: `{"player": <player object>}`. `player.online` is `false` on disconnect.
  - `log`: `{"msg": "<formatted line without timestamp>", "level": "fatal|error|warning|info|verbose|debug"}`. The server has emitted every line since the hook was installed, including the multi-line permission blocks (`"\t - CanEditWorld"`).
  - `chat-message`, `player-death` and `entity-killed` are reserved and not emitted yet.
- How connect and disconnect are derived:
  - Connect is `[online] Added peer P (steamid:S)`, then `[server] Machine 'M': Player 'H' logged in`, then `[server] Player '<name>' logged in with Permissions:` and its permission lines.
    - The peer is matched first on `M == P`'s parenthesised index, and otherwise to the oldest pending peer.
    - The event fires once the permission block ends: on the next non-permission line, or 750 ms after the last one.
  - Disconnect fires on `[server] Remove Player '<name>'` or `[online] Removed peer P`, whichever comes first. A player only gets one disconnect.

### Moderation (v0.3, live-proven 2026-09-13; admin targets v0.4.2, live-proven 2026-09-14)
- `POST /kick {gameId}` and `POST /ban {gameId}`: player must be online (else `404 player not online`).
  Success is `{"success":true}`, plus `"adminProtectionBypassed":true` when the target's group has `canKickBan` (see below).
  `409` means the game has no player slot for the account (the action would be a no-op); `503` means the moderation hooks or the admin-protection self-check are unavailable.
- **Admin protection (v0.4.2).** The game's handler silently skips Kick and Ban for a player whose slot permission mask has bit 0 (`CanKickBan`) set, i.e. any player who joined with a group that has `canKickBan: true` (default "Admins"): `test byte [slot+0x1cb],1; jne return` in the Kick/Ban branch. Plugins up to 0.4.1 therefore returned success while nothing happened (found 2026-09-14). Since 0.4.2 the plugin finds the target slot (session machine handle of the account -> slot `+0x1c4`), clears bit 0 only for the duration of the synchronous handler call on the moderation thread and restores it. The three layout values (flags offset, machine-handle offset, slot stride) are read from the handler's own code (`F6 87 <off32> 01 0F 85 .. 40 80 FE 01`, `8B 97 <off32> 49 8B 4D 10 E8`, `48 81 C7 <stride32> 48 83 FB 10`, each unique in the first 0x1000 bytes); on mismatch kick/ban refuse with 503 instead of silently doing nothing. `/health` `diagnostics.resolved` lists it as `adminProtect`. `reason` and `expiresAt` are accepted but the game has no field for them (bans are permanent until unbanned).
- `POST /unban {gameId}`: works offline, as long as the plugin has seen the player online at least once (else `404`).
- All three call the game's account-action handler `handleAccountAction(server, accountIdHash, Kick=0|Ban=1|UnBan=2)` from inside a detour on the moderation system that the game itself uses to call it (same thread, live `server` pointer from `r8`). Result: identical to the in-game admin action. The client shows "The host has kicked you from the session." / "The host has banned you from the server. You may not rejoin."; a ban is written immediately to `bannedAccounts` in `enshrouded_server.json`; unban logs `[server] Account <name>/<character> was unbanned.`
- **The game keys accounts by `accountIdHash` (u64), not the SteamID64.** The plugin maps SteamID to hash through the session machine table (`[server+0x10]` manager, active copy `+0x48+[+0x3c]*0x2578`, 64 records of `0x140`: handle `+0x188`, machine handle `+0x190` whose low 7 bits equal the `Machine 'M'` index from the login line, hash `+0x198`). Mappings are cached in `<server>\takaro\accounts.json` (refreshed every 5 s for online players), so unban works after the player left and across restarts.
- `GET /bans` returns `[{gameId, steamId, name, characterName, accountId, bannedAt, reason:"", expiresAt:null}]` read from `enshrouded_server.json`. `gameId`/`steamId` are the SteamID when the hash is in the cache, otherwise the raw `accountId` hash.
- `POST /shutdown` returns `{success:true}` and ~0.5 s later calls the game's own console control handler with `CTRL_C_EVENT` (resolved from the `SetConsoleCtrlHandler` call site). The log shows `[app] Trigger gameflow shutdown, exit: Ctrl_C`, `Start Saving`, `Saved`, `Send Shutdown Message to Player`. Under the mornedhels image, supervisord then restarts the server process.
- Signatures, all anchored and self-checked (the capability goes `degraded` on any mismatch; nothing is hooked):
  - handler: function root of the `[server] Account %s/%s was unbanned.` xref, with prologue check plus `cmp r8b,2` at +0x2f (`0x683510` @1024233)
  - moderation system: unique direct caller of the handler, with prologue check (`cmp qword [r8+38h],0`) and call-site check `mov rcx,r14; mov rdx,[rdx+8]` (`0x69cd40`)
  - ctrl handler: `lea rcx,[rip+X]` before `call [SetConsoleCtrlHandler]`, with X's shape checked (`0x4f5650`)

### World actions (v0.4)
| Endpoint | Body (required in bold) | Response |
|---|---|---|
| POST /teleport | **gameId**, **x**, **y**, **z** (metres, y up) | `{success:true}` once the Teleport component is added |
| POST /give | **gameId**, **code** (item code, case-insensitive, or numeric itemId), **amount**, quality? (ignored) | `{success:true, code, detail}` after the game consumed every create action |
| POST /message | **text**, recipientGameId?, type? (debug, default 0), senderHandle? (debug) | `{success:true}` |
| POST /command | **command** | `{success, output}` |
| POST /kick, /ban, /unban, /shutdown | see Moderation | |

- **teleport**: adds the server-only `Teleport` component (80 B) to the player entity with `World_addComponent` from a
  detour of `Server::updatePlayers` (server main thread). The game's own `teleport` job then moves the player, bumps the
  replicated `TeleportCount` and logs `Player EntityId N teleported From (...) to (...)`. `searchBestSpawnPosition` is
  set, so the game nudges the target to a free voxel position. Orientation/scale are kept. Takaro rounds coordinates to
  integers before sending them. A teleport far outside the playable world is undone by the game (respawn at start).
- **give**: injects `AdminInventoryCreateAction` into the player's `PlayerInput` (server-only) right before the game's
  `inventory_actions` job, and makes `isAdminEntity` return true **only** for that player and **only** for the call from
  that job (return-address check), until the game's `ServerConsumedPlayerInput` shows the action consumed. The game's
  own admin create path then creates the item (stacking, unique item entities, replication). The game ignores the
  action's `count`: each action creates 1, or a full stack with `createFullStack`. `amount` is therefore split into
  full-stack actions plus single-item actions (max 64 actions per request; ~15 ms each). Non-admin players get items
  without being granted anything else. Note for Admin-group players: their client's own admin version counters may
  differ after a give; the plugin never enables the gate for them outside a pending give.
- **message**: calls `ChatSystem::addMessage` on the server thread. Live-tested client behaviour: only chat type 0 with a
  valid player handle is rendered as free text, shown as `<character name>: <text>`; handle 0 is dropped and types 1-3
  become canned notices (e.g. "set a ping"). The plugin therefore uses the recipient's own handle (whisper) or the first
  logged-in player's handle (broadcast). With `recipientGameId`, the queued record is marked as already delivered to
  every other active machine (done bit + pending bytes, mirroring the per-tick flush), so only the recipient receives it.
  With nobody online, a broadcast succeeds without delivering anything.
- **command** (Enshrouded has no server console; this is a plugin-defined set): `help`, `version`, `players`,
  `say <text>`, `whisper <player> <text>`, `location <player>`, `teleport <player> <x> <y> <z>`, `tp <player> <toPlayer>`,
  `inventory <player>`, `give <player> <itemCode> [amount]`, `item <search>`, `kick <player>`, `save-and-shutdown`.
  `<player>` is a SteamID64 or the exact name. Unknown commands return `success:false` with a hint. There is no
  time-of-day command (not reverse engineered).

### GET lists (static data from the server's kfc, build 1024233; regenerate with `tools/gen_gamedata.py`)
- `GET /bans`: see Moderation.
- `GET /items` returns 3609 `[{code, name, description:"<category>, <rarity>", itemId, maxStackSize}]`. Localized names
  do not ship with the dedicated server, so `name` is the code with spaces.
- `GET /entities` returns 979 actor templates `[{code, name, type: hostile|friendly|neutral, description}]`.
- `GET /locations` returns 1031 map markers and spawn points `[{code, name, kind, spawnType?, position:{x,y,z}}]`.

### Events added in v0.4
- `chat-message`: `{msg, channel:"global", chatType, senderName, player?}`. From a detour of `ChatSystem::addMessage`,
  filtered by return address to the player-chat call site in `processIncomingMessages` (join/leave notices and plugin
  messages are not reported). Text is before the game's optional text filter.
- `player-death`: `{entityId, playerName, player?, position?, attacker?|killerEntity?}` from the `EntityDiedEvent` list read
  in a detour of the `combat_experience_source` ECS system, for entities that are in a player slot. Killer comes from a
  killing-blow `HitEvent` seen in the previous 10 s (none for fall/fog deaths). Deduplicated per player for 3 s.
- `entity-killed`: `{player?, entity:<template code>, weapon:"", victimEntityId, templateGuid, killerName, weaponCategory}`
  for `HitEvent`s with `WasKillingBlow` whose root source is a player entity and whose target is not a player. The victim's
  template GUID comes from the game's entity-template lookup and is mapped to a code via the entities table.

### GET /debug/machines, GET /debug/findu64?v=<u64> (diagnostic, not part of the sidecar contract)
`/debug/machines` dumps the session machine table (handle, machine handle, account hash). `/debug/findu64` scans the server and machine-manager objects for a u64 and reports offsets. Both run on the moderation thread.

### GET /debug/slots, GET /debug/nearby?player=<id|name>&radius=<m> (diagnostic)
`/debug/slots` dumps the 16 server player slots (handle, machine handle, login state, entity id, name).
`/debug/nearby` lists live entities with enemy/animal/npc/boss/faction components near a player (entity id, distance, position).

### GET /debug/gamethread (diagnostic, not part of the sidecar contract)
Runs a no-op task on the game thread and returns `{"ranOnThreadId":204,"httpThreadId":332,"latencyMs":16}`. If the tick is not running, it returns 503.

## Implementation notes (for capability owners)
- **Signature resolution** (`src/hooks.cpp`) is anchored on strings. No absolute RVAs are used.
  - **Logger**: take the E8 target that most often follows `lea reg, <fmt>` for six known log format strings. This build resolves it to `0x4ef180` with 12 votes.
  - **Log sink**: the call target inside the logger that matches `40 53 55 56 48 83 EC ?? 49 83 78 08 00`, which is `0x4df6b0`.
    - Signature: `sink(u8 threshold, u8 level, const {char* ptr; u64 len}* text)`. The text is already formatted and ends with `\n`.
    - Lines are emitted only when `threshold >= level`, the same filter the game applies.
  - **Game-thread tick**: take the unique direct caller of the function that references `"-------------- Session ----------------"`. That caller is the session update, `0x8a88f0`, running at about 57 Hz on one constant thread.
- **Hooks**: MinHook (vendored in `third_party/minhook`). Each detour calls the original function first, then does its own work.
- **Game-thread work**: `RunOnGameThread(std::function<std::string()>, result, timeoutMs)` in `hooks.h`. At most 16 tasks are drained per tick.

### World hooks (v0.4, `src/world.cpp`)
All anchored on code shape, never absolute RVAs; each capability degrades on its own if a check fails.
- `Server::updatePlayers` (`0x69f500` @1024233): unique prologue pattern whose caller is the moderation/network-update
  function with `mov rcx, r14`. Slot base/stride come from `imul rsi, r15, <stride>; add rsi, <base>` inside it, the world
  offset from `mov rdx, [r14+<off>]` before its `getComponentRead` call. Tasks queued by the HTTP threads are drained right
  after the original returns (server main thread). Slot name offset `+0x40` was live-checked (`/debug/slots`).
- `World_getComponentRead` (`0x5c92a0`): the one of two shared-prologue functions whose second call is the read core.
- `World_addComponent` (`0x579730`): unique pattern, must be called from `updatePlayers`.
- `isAdminEntity` (`0x11c290`) and `inventory_actions` job (`0x15b0f0`): unique patterns; the gate return address, and the
  PlayerInput/ServerConsumed version offsets (`0x334`/`0x94`) are read from the job's code, with a `lea rsi,[rbx+0x330]` check.
- `ChatSystem::addMessage` (`0x80bbf0`): unique pattern; receive call site = direct call preceded by
  `mov byte [rbp+50h],0; mov byte [rbp+40h],0` inside a callee of the network update; chat offset from `mov rdx,[r13+68h]`.
- `combat_experience_source` (`0xb8df0`): unique pattern including its context size and `mov r15,[rbp+58h]`; the
  EntityDiedEvent view load `mov rdx,[rbp+50h]` is checked; entity template lookup `0x5c9fb0` by unique pattern.
