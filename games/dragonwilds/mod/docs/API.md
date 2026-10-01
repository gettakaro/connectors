# Takaro Dragonwilds plugin: native connector contract

The plugin is `libtakaro-dragonwilds.so`, loaded into `RSDragonwildsServer-Linux-Shipping` through
`LD_PRELOAD`. Since the native generation it **is** the connector: it holds the Takaro WebSocket
itself (outbound `wss://`, no inbound port) and there is no sidecar. The loopback HTTP API that the
0.2.x sidecar used is kept as an optional, authenticated diagnostics surface.

```
game hooks ──► event ring ──► bridge worker ──► durable outbox ──► transport worker ──► Takaro
                                   ▲  │                                    │
             log tail (raw lines) ─┘  └──► action worker ──► GameThread jobs ◄─ requests
```

Wire shapes are the 0.2.x sidecar's (`sidecar/src/dragonwilds/{adapter,mapping}.ts` was the parity
reference; `tests/native_behavior_parity.cpp` and `tests/native_full_bridge.py` pin them). State
files: `native-state.md`. Game-thread rules: `gamethread-policy.md`.

## Configuration

Environment of the game process; the `plugin.json` column is `<data dir>/plugin.json`.

| env | `plugin.json` | default | meaning |
|---|---|---|---|
| `TAKARO_WS_URL` | — | `wss://connect.takaro.io/` | Takaro WebSocket; must be `wss://` |
| `TAKARO_IDENTITY_TOKEN` | — | `dragonwilds` | identity token (server identity in Takaro) |
| `TAKARO_REGISTRATION_TOKEN` | — | — | registration token, sent with `identify` when set (trimmed) |
| `TAKARO_SERVER_NAME` | `serverName` | `Dragonwilds` | name sent with `identify` (env only); last chat-sender fallback |
| `TAKARO_SENDER_NAME` | `senderName` | — | chat sender name (`TAKARO_SERVER_CHAT_NAME` / `serverChatName` is the alias) |
| `TAKARO_CA_FILE` | — | `/etc/ssl/certs/ca-certificates.crt` | CA bundle; chain and host name are always verified |
| `TAKARO_RECONNECT_BASE_MS` / `TAKARO_RECONNECT_MAX_MS` | — | `2000` / `60000` | reconnect backoff (2000..60000) |
| `TAKARO_REQUEST_DEADLINE_MS` | — | `8500` | every Takaro request is answered within this (1000..9500); Takaro gives up at 10 s |
| `TAKARO_STATE_DIR` | — | `<data dir>` | connector state, see `native-state.md` |
| `TAKARO_CURSOR_FILE`, `TAKARO_ONLINE_FILE`, `TAKARO_BAN_FILE`, `TAKARO_KNOWN_PLAYERS_FILE` | — | in the state dir | 0.2.x file locations, see `native-state.md` |
| `DRAGONWILDS_LOG_TAIL` | — | `auto` | `auto`: the log tail takes over join/leave while the `players` capability is degraded; `always`; `never` |
| `DRAGONWILDS_LOG_EVENTS` | — | `filtered` | `log` events: `filtered` drops UE/EOS noise; `all`; `none` |
| `DRAGONWILDS_LOG_FILE` | — | — | server log path; honoured only if it exists in the game process (it was the sidecar's mount path) |
| `TAKARO_LOG_PATH` | `logPath` | `<game>/Saved/Logs/RSDragonwilds.log` | server log path |
| `TAKARO_TICK_BUDGET_US` | `tickBudgetUs` | `500` | game-thread job budget per tick (50..33000) |
| `TAKARO_PLUGIN_IDLE` | `idle` | off | `1`: load, log one line, do nothing else (A/B arm) |
| `TAKARO_NATIVE_DISABLE` | — | off | `1`: no Takaro connection (hooks and pump still run) |
| `TAKARO_PLUGIN_TOKEN` | `token` | — | enables the diagnostic HTTP listener; not needed for Takaro |
| `TAKARO_PLUGIN_PORT` | `port` | `18890` | diagnostic listener port (loopback only) |
| `TAKARO_PLUGIN_DEBUG` | `debug` | off | enables `/debug/*` and per-request logging |
| `TAKARO_SYM_PATH` | `symPath` | `<exe>.sym` | symbol database |
| `TAKARO_PLUGIN_DATA_DIR` | — | `<exe dir>/takaro` | `plugin.log`, `symcache.json`, `plugin.json`, `bans.json` |

`TAKARO_NATIVE_GATE=1` is a test-only transport mode (memory outbox, three actions); never use it in
an installation. `plugin.log` lines are redacted (`*Password`, `*Token`, `Ticket` values).

## Takaro protocol behaviour

- `identify` with the identity token (+ registration token, + name) on every new socket.
  `identifyResponse` is the moment the connector is live; a rejected identity closes the socket and
  reconnects with backoff. Takaro `ping` frames get `pong`; Takaro `error` frames are counted
  (`/health.native.protocolErrorCount`) and the last one is kept in `lastError` with tokens redacted.
- Requests: `payload.args` may be `[]`, `{}`, an object, `null` or a JSON-encoded string; nesting
  deeper than 64 is refused. Every request is answered once, by `requestId`, within
  `TAKARO_REQUEST_DEADLINE_MS` (a request still queued then is dropped unexecuted and answered with an
  error; a running one is answered with an error, finishes, and its late response is suppressed).
  Unknown action → error frame `Unknown Takaro action '<x>'`. Request ids over 128 bytes close the
  socket; duplicates are refused.
- Events: every outgoing `gameEvent` is built by one whitelist (`NativeBehavior::MapEventData`) with
  exactly the keys Takaro's DTOs accept, plus `timestamp` (the moment the game produced it). Events
  are admitted durably, sent only on an OPEN and identified socket, and confirmed only by a pong that
  follows the write (F10, F20); unconfirmed events are re-sent after the next identify, also after a
  crash.
- Shutdown (`shutdown`, or `executeConsoleCommand shutdown`): the response is written and pong-
  confirmed first (bounded, 2 s), then the plugin saves (`CanSave` → `RequestSaveGame`) and sends
  itself `SIGTERM`. A response that cannot be written cancels the shutdown. Not gated, as in 0.2.x.
- Timed bans: Takaro never sends an unban for a timed ban. The connector lifts it at `expiresAt`
  (checked every second, across restarts), through the same unban path with a revision guard so a
  newer permanent ban is never lifted by an older schedule. Every ban mutation is journaled in
  `ban-intent.json` first and verified against both lists after a crash.
- Online reconcile: at identify and every 30 s, players Takaro was told are online but the game no
  longer lists get a `player-disconnected` (server crash with players connected). A connect admitted
  within the last 30 s is never undone by a reconcile read taken before it.

### Actions

| action | behaviour |
|---|---|
| `testReachability` | `{connectable:true, reason}`; `reason` is `null` when healthy, else `Dragonwilds plugin degraded` and/or `capabilities not ok: a=degraded, …` (`unimplemented` is not news) |
| `getPlayers` | online players only, IGamePlayer |
| `getPlayer` | online → the player; seen before → last-known record with `online:false`; never seen → `{gameId,name:gameId,epicOnlineServicesId,platformId,online:false}` built from the id (F7). Never `{}`, `null` or an error for a valid id |
| `getPlayerLocation` | `{x,y,z}`; within 60 s of a connect/disconnect event a failed lookup answers the last position or the origin (Takaro stores the event only when this succeeds) |
| `getPlayerInventory` | `[{code,name,amount}]` |
| `giveItem` | `item` / `itemCode` / `code` / `item.code` / `item.name` / `name`; `amount` (or `quantity`, default 1, must be > 0); `quality` ignored |
| `listItems` / `listEntities` / `listLocations` | bare arrays with display names |
| `executeConsoleCommand` | `CommandOutput`; a command the plugin refuses (unknown verb, bad arguments, `cheat`) is `{success:false, rawResult:"", errorMessage}` (F1), never an error frame |
| `sendMessage` | no recipient = everyone; `opts.recipient.{gameId,epicOnlineServicesId,steamId,platformId}` = that player; sender `opts.senderNameOverride` → `TAKARO_SENDER_NAME` → `TAKARO_SERVER_NAME` → `Server`. Rendered under the receiving player's name as `[sender] text` |
| `teleportPlayer` | numeric `x,y,z` (numbers or numeric strings) or a named `target`; `yaw` optional; `dimension` ignored |
| `kickPlayer` | `reason` optional |
| `banPlayer` / `unbanPlayer` | plugin `bans.json` + `KnownPlayerList[].bIsBanned` + `PerformConfigSave` + `BanPlayer` when online, PostLogin kick on rejoin; `expiresAt` (ISO string or epoch ms; `null`/`0`/garbage = permanent) |
| `listBans` | bare array `[{player, reason, expiresAt}]`; a scheduled timed ban the game no longer lists is still reported until it expires |
| `shutdown` | see above |

Every optional argument accepts absent, `null` or a wrong type (`""`, `0`, `false`, `[]`, `{}`); a
player is `{gameId}`, `{epicOnlineServicesId}`, `{steamId}`, `{platformId:"epic:…"}` or nested
`{player:{…}}` / `{playerRef:{…}}` (the full pog row).

### Events (wire)

| type | data |
|---|---|
| `player-connected` / `player-disconnected` | `{player}` |
| `chat-message` | `{msg, channel, player?}` (`channel` global/team/friends/whisper) |
| `player-death` | `{player, position?, attacker?, msg?}` (`msg` = `<name> was killed by <creature>`) |
| `entity-killed` | `{player, entity, weapon}` (`weapon` is a string, `""` when unresolved) |
| `log` | `{msg}` redacted: `*Password` values, `?p=<base64>`, `*Token`/`Ticket` values; a line mentioning a password in free text becomes `[redacted: line mentions a password]` |

IGamePlayer: `{gameId:<puid>, name:<character name>, epicOnlineServicesId:<puid>, platformId:"epic:<puid>", steamId?, ip?, ping?}`.

## Diagnostic HTTP surface (optional)

Off unless `TAKARO_PLUGIN_TOKEN` (or `token` in `plugin.json`) is set. Loopback only.
- HTTP/1.1, one short-lived thread per connection, every response closes it (`Connection: close`).
  Bodies are JSON (`Content-Type: application/json`), UTF-8.
- Every request needs `Authorization: Bearer <token>`; wrong or missing → `401 {"error":"unauthorized"}`.
- Errors are always `{"error":"<message>"}`:
  - `400` bad body or missing field · `404` unknown path or player not online · `405` known path,
    wrong method · `409` the game refused the action · `413` body over 1 MiB ·
    `501 {"error":"unimplemented", "capability":"..."}` · `503` the game thread is unavailable.
- The listener binds loopback only and retries every 5 s if the port is busy.
- The action endpoints below call the same handlers the bridge uses; ban changes made through them
  are picked up by the connector's expiry schedule within 2 s.

## Identity
`gameId` is the player's **EOS ProductUserId** (32 hex chars, lower case, shown at the bottom of the
in-game Settings screen). `platformId` is `epic:<puid>`; `steamId` only when a real SteamID64 is
known. `name` is the in-game character name (from the server's `PlayerChar entered world` log line;
`ADominionPlayerState::GetCharacterDisplayName` is never called - it crashes the server).

## GET /health
```json
{"status":"ok","version":"0.2.0","bootId":"c07f7763cfbf47c9","pid":48,
 "gameBuild":"++dominion+staging-CL-240163","engineVersion":"5.6.1-240163",
 "buildId":"3b4ce30aed886594","uptimeMs":26709,
 "capabilities":{"gameThread":"ok","reflection":"ok","players":"unimplemented", ...},
 "capabilityDetails":{"players":"lane L3 not wired yet", ...},
 "symCache":{"path":"...","symPath":"...","buildId":"...","hit":true,"loadMs":0},
 "native":{"connection":{"state":"connected","identified":true,"epoch":1},
           "queues":{"inbound":0,"actions":0,"runningActions":0,"outbound":0,"outbox":0,"rawLogs":0,…},
           "deliveryLosses":0,"persistenceErrors":0,"banRecoveryPending":false,"banMetadataError":"",
           "outboxDurabilityPending":false,"rawLogLosses":0,"overloads":0,"prunedEvents":0,
           "confirmedSeq":330,"scanSeq":330,"lastError":"","requestCount":12,"lastRequestAction":"getPlayers",
           "protocolErrorCount":0,"deadlineAnswers":0,"stateVersion":1,"persistenceLastError":"",
           "behavior":{"knownPlayers":3,"onlinePlayers":1,"timedBans":0,"banPersistenceError":"",
                       "logTailOwnsConnections":false,"logParserError":"","lastError":""},
           "gate":{"experimental":false,"durableOutbox":true}},
 "diagnostics":{
   "sym":{"resolved":74,"wanted":74,"processEventSlot":77},
   "selfChecks":[{"check":"_init: sym=0xdc2dd90 elfSection=0xdc2dd90","ok":true}, ...],
   "reflect":{"objName":24,"classDefaultObject":272, ...,"validations":[{"check":"findFunctionNative","ok":true,"detail":"..."}]},
   "gameThread":{"installed":true,"how":"...","alive":true,"tickCount":698,"tickThreadId":47,
                 "tickThreadChanges":0,"approxHz":28.8,"jobsRun":2,"jobsTimedOut":0,"maxJobMs":9,"tickBudgetUs":500},
   "perf":{"windowMs":60000,"tick":{"count":1800,"avgUs":2.6,"p50Us":1.1,"p99Us":40,"maxUs":900,"hz":30,"budgetHits":0,…},
           "jobs":{…},"gameThreadEntries":{"count":12,"perSecond":0.2},
           "processEventFilter":{"calls":90000,"avgNs":60,"maxNs":4000,"hits":3,"cacheHits":89950,…},
           "eventHandlers":{…},"sweeps":{"housekeep.sweep":{"calls":12,"avgUs":800,…},"GET /players":{…}}},
   "pluginLogDropped":0,
   "http":{"port":18890,"requests":12,"unauthorized":2,"handlerErrors":0},
   "events":{"buffered":0,"latestSeq":0},
   "hooksInstalled":3,
   "resolved":[{"name":"UObject::ProcessEvent","rva":"0x4e3cf50","addr":"0x503cf50","how":"symcache",
                "hooked":false,"fired":0,"signature":"UObject::ProcessEvent(UFunction*, void*)",
                "records":37,"contiguous":true}, ...]}}
```
- `status` is `"ok"` only when every self-check passed, `reflection` is `ok`, the bridge is identified
  with Takaro and no persistence / ban-recovery / log-grammar error is pending; otherwise `"degraded"`.
  A degraded plugin keeps serving; the server is never affected.
- `capabilities` values are `"ok"`, `"degraded"` or `"unimplemented"`; the reason for a degrade is in
  `capabilityDetails`. `"ok"` means resolved/hooked, **not** proven — live proof lives in
  `context/games/dragonwilds/evidence/`.
- `bootId` is random per server process.
- `native` is the bridge's own health (`protocolErrorCount` counts Takaro error frames,
  `deadlineAnswers` requests answered by the deadline); `diagnostics` is informational. Do not code
  against their exact shape.

## GET /events?since=\<seq\>[&limit=\<n\>] (diagnostics; the bridge reads the same ring in-process)
```json
{"bootId":"c07f7763cfbf47c9","seq":328,"latestSeq":330,"truncated":false,
 "events":[{"seq":328,"type":"player-connected","data":{...},"ts":"2026-09-16T18:22:37.107Z"}]}
```
- Ring buffer of the last 5000 events. `seq` is monotonic, starts at 1, resets on restart.
- Returns events with `seq > since`, oldest first, capped at `limit` (default and maximum 5000).
- Pass the response `seq` back as the next `since`: it is the last returned event, or the current
  latest when nothing is new.
- `since > latestSeq` means the server restarted; reset to `0`.
- `truncated: true` means events between `since` and the oldest buffered event were dropped.
- Types: `player-connected`, `player-disconnected`, `chat-message`, `player-death`, `entity-killed`,
  `log`. All are wired (lane L2). `entity-killed` carries a readable `entity` name
  (`ADominionAICharacter::AIName`) with the Blueprint class in `entityCode`/`entityClass`, and the
  weapon that made the kill in `weapon`/`weaponCode`; `weaponSource` names the route that resolved
  it (the fatal damage event's source item, the killer's equipped main/off hand, the loadout scan,
  or `unresolved`). `weapon` is empty for a bare-handed kill and for an unresolved one alike -
  `weaponSource` is what tells those apart. Only `weapon` is forwarded to Takaro; the other keys are
  plugin-side, because Takaro's EventEntityKilled schema does not carry them.

### Event payloads (real output, build `++dominion+staging-CL-240163`)
The `player` object is `{gameId, name, characterName?, platformName?, characterGuid?, steamId?, platformId}`;
`gameId` is the bare 32-hex EOS ProductUserId.
```json
{"type":"player-connected","data":{"player":{"gameId":"0123456789ab…","name":"takarotester","characterName":"takarotester","characterGuid":"41C4B04F…","platformId":"epic:0123456789ab…"}}}
{"type":"player-disconnected","data":{"player":{…}}}
{"type":"chat-message","data":{"msg":"hello","channel":"global","player":{…}}}
{"type":"player-death","data":{"player":{…},"position":{"x":0,"y":0,"z":0},"attacker":{…}?,"killerEntity":"FallDamageActor","source":"telemetry|health-edge"}}
{"type":"entity-killed","data":{"entity":"Cow","entityCode":"BP_AI_Cow_Character_C",
  "entityClass":"BP_AI_Cow_Character_C","weapon":"Rune Sword","weaponCode":"…",
  "weaponSource":"the killer's equipped main hand (ELoadoutSlot::HeldRight)",
  "source":"BP_OnDeath","attribution":"the fatal damage event's instigator","player":{…}}}
{"type":"log","data":{"msg":"[2026.09.16-17.46.33:165][355]LogNet: Login request: ?p=<redacted>…"}}
```
- `position` is omitted when the game did not fill the victim location; `attacker` only appears for a
  player killer, a creature/environment killer goes into `killerEntity`.
- `log` ring events exist only while the native bridge is not running (`TAKARO_NATIVE_DISABLE=1`):
  redacted, UE noise dropped, at most 120 per 2 s cycle. With the bridge running, every raw log line
  goes to the bridge, which redacts, filters (`DRAGONWILDS_LOG_EVENTS`) and admits `log` events to the
  durable outbox itself.
- Event sources and their hook state are in `/health.diagnostics.eventSources`
  (`{source, hooked, fired, emitted, note}`, plus `processEventVTables`, `trackedConnections`, `logPath`).

## Debug endpoints (need the token *and* `TAKARO_PLUGIN_DEBUG=1`; otherwise `404`)
| endpoint | returns |
|---|---|
| `GET /debug/gamethread` | `{"ranOnThreadId":47,"httpThreadId":123,"latencyMs":31,"stats":{...}}`. Runs a no-op job on the game thread. `503` if the pump never ticked. |
| `GET /debug/symbols` | the ELF facts, the symcache block and the full resolved table. |
| `GET /debug/object?path=/Script/Pkg.Name` or `?ptr=0x...` | the object's UPROPERTY tree up the class chain: `{name, type, offset, value}` per property, values decoded for primitives, `FString`, `FName` and object pointers. When the object is itself a `UClass`/`UScriptStruct` the properties it *declares* are listed under `declaredProperties`. `ptr` is refused unless it is inside a readable mapping. |
| `GET /debug/perf[?reset=1]` | the game-thread counters (same as `/health.diagnostics.perf`); `reset=1` starts a new window after answering. |
| `POST /debug/kill-nearest {gameId?, radius?}` | kills the AI nearest to a player through the game's own damage pipeline, with that player as instigator, so `entity-killed` can be proven without a human at the PC. |
| `GET /debug/structs?name=X` | property table of a `UClass`/`UScriptStruct`. `X` is a full path (`/Script/JagexChatBackend.ChatMessageData`) or a plain name, which is resolved by scanning the live object array (so the owning module does not have to be known). |

Example (real output, build `++dominion+staging-CL-240163`):
```
GET /debug/structs?name=ChatMessageData
{"package":"/Script/JagexChatBackend","name":"ChatMessageData","class":"ScriptStruct",
 "propertiesSize":136,"super":"",
 "properties":[{"name":"SenderData","type":"StructProperty","offset":0},
               {"name":"MessageBody","type":"StrProperty","offset":120}]}
```

## Diagnostic action endpoints (lane L3) — real shapes

Identity: `gameId` is the bare 32-hex EOS ProductUserId, lower-cased (a `RedpointEOS:` prefix is
stripped). `GET /players/{id}`, `/give`, `/teleport`, `/kick`, `/ban`, `/unban` also accept the
character name or the platform name for convenience.

| endpoint | body (required in bold) | response |
|---|---|---|
| `GET /players` | — | `[{gameId,name,characterName,platformName,epicOnlineServicesId,platformId:"epic:<puid>",steamId?,ping,spawned,online:true,connectedAt}]` |
| `GET /players/{id}` | — | one player object, else `404 {"error":"player not online"}` |
| `GET /players/{id}/location` | — | `{x,y,z,yaw,pitch}` (UE cm, doubles); `503` when the player has no pawn yet |
| `GET /players/{id}/inventory` | — | `[{code,name,amount,inventory,slot}]` — one entry per item across every `UInventoryComponent` under the pawn and the controller (`inventory` is the component name, e.g. `BP_Components_Inventory`, `BP_Components_Loadout`) |
| `GET /items[?search=]` | — | `[{code,name,description,category}]`, 1536 entries on this build; `search` matches code or name, case-insensitive |
| `GET /entities` | — | `[{code,name,type:"hostile",description}]` — the AI character classes and `UAIDataAsset` assets currently loaded (AI content streams in on demand, so this is not the full bestiary) |
| `GET /locations` | — | `[{code,name,position:{x,y,z}}]` — lodestone actors currently streamed in |
| `GET /bans` | — | `[{gameId,name,reason,expiresAt,createdAt,enforcedBy,inGameList,inPluginList}]` — the union of the game's own `KnownPlayerList` (`bIsBanned=True`) and the plugin ban list in `<serverdir>/takaro/bans.json`. The game stores neither a reason nor an expiry, so those come from the plugin list; the native bridge lifts timed bans. `enforcedBy` is `"plugin"` when the `PreLogin` hook is installed and the entry is in the plugin list (the rejoin is refused immediately) and `"game"` for an entry only a restarted server would honour |
| `POST /message` | **text**, `recipientGameId?`, `senderName?` | `{success:true,delivered:<n>}`. Without a recipient the message goes to everyone. The client renders it under the *receiving* player's name, so the sender is prefixed: `[<senderName>] <text>`. `senderName` defaults to `TAKARO_SENDER_NAME`, else `Server` |
| `POST /teleport` | **gameId**, **x**,**y**,**z** *or* **target**, `yaw?` | `{success:true,position:{x,y,z}}`; `404` unknown target, `409` when the game refuses the destination |
| `POST /give` | **gameId**, **code**, `amount` (default 1) | `{success:true,code,name,amount}`; `404` unknown/ambiguous code, `409` when the game refuses (full inventory) |
| `POST /kick` | **gameId**, `reason?` | `{success:true,gameId,online:true}`; `404` when the player is not online |
| `POST /ban` | **gameId**, `reason?`, `expiresAt?` (ISO-8601 UTC, stored in `bans.json`) | `{success:true,gameId,online,persisted,pluginList,enforcedBy,detail}`. Three things happen: the id goes into the plugin ban list (`takaro/bans.json`), which the `PreLogin` hook refuses a rejoin with **immediately, with no restart** (the client sees the game's own `PLogBanned` refusal); an online player is disconnected through `ADominionGameSession::BanPlayer`; and `KnownPlayerList[…].bIsBanned` is set and saved to `DedicatedServer.ini` so the game itself keeps refusing the login after a restart. `persisted:false` with `pluginList:true` means the server has never seen that player, so only the plugin list carries the ban — which is enough |
| `POST /unban` | **gameId** | `{success:true,gameId,pluginList,persisted,detail}`; removes the id from the plugin ban list and clears `bIsBanned` in the game's list, then saves. The player can rejoin at once. An id neither list holds and the server never saw is already unbanned (200) |
| `POST /command` | **command** | `{success,output}` — `output` is a string (JSON text for the list commands) |
| `POST /shutdown` | — | `{success:true,"detail":"saving, then SIGTERM"}`, then `CanSave` → `RequestSaveGame` → `SIGTERM` to our own pid; the container restarts under its compose policy |

### `POST /command` command set
```
help | players | bans | items [query] | entities | locations
say <msg> | whisper <gameId> <msg>
give <gameId> <code> [amount] | tp <gameId> <x> <y> <z>
kick <gameId> [reason] | ban <gameId> [reason] | unban <gameId>
save | shutdown
raw <console command>        # UEngine::Exec with our own FOutputDevice; output is captured
cheat <gameId> <dom command> # 501: the Shipping dedicated server creates no CheatManager
```
`raw` only reaches commands that survive into a Shipping build (`log list` works; `obj list`,
`stat unit` and `memreport` answer `(command not recognised by the engine)`).

## Implementation notes (for capability owners)
- **Symbols** (`src/sym.cpp`): every address comes from the depot's `<exe>.sym` at runtime; no RVA is
  ever hard-coded. Format: `u32 N`, `N` x 20-byte `{u64 rva, u32 line, u32 fileOff, u32 nameOff}`
  sorted by rva, then a `\n`-separated name table shared by source paths and demangled signatures.
  A function's address is the **lowest rva of its record run**; `vaddr = rva + the first PT_LOAD
  p_vaddr` read from the ELF (`0x200000` on this build). A wanted entry either pins one exact
  signature or matches by base name (lowest rva across overloads). Results are cached in
  `<data dir>/symcache.json` keyed on `.note.gnu.build-id`, so a game update invalidates it.
  Addresses outside an executable segment, or starting with `0x00`/`0xCC`, are rejected.
- **Boot self-checks** (`/health.diagnostics.selfChecks`): `_init`/`_fini` from `.sym` must equal the
  ELF section addresses; `UObject::ProcessEvent` must appear **exactly once** in
  `dlsym("_ZTV7UObject")` (that match is the ProcessEvent slot index, 77 on this build); at least 40
  names must resolve.
- **Reflection** (`src/reflect.cpp`): UE 5.6 offsets are declared in one `Layout` struct and
  *validated against the live process*; on mismatch they are re-discovered by scanning, and on
  failure the capability degrades. Confirmed on this build: `UObject::NamePrivate 0x18`,
  `UClass::ClassDefaultObject 0x110`, `UFunction::Func 0xD8`, `UStruct::ChildProperties 0x50`,
  `FField::Next 0x18`, `FField::NamePrivate 0x20`, `FProperty::Offset_Internal 0x44`.
  The decisive check is `FindFunction(CDO PlayerChatComponent,"Server_SendChatMessage")->Func ==
  sym("UPlayerChatComponent::execServer_SendChatMessage")`.
  **Never stringify an unvalidated `FName`**: `FName::ToString` indexes the engine name pool and
  segfaults on a bogus index. Discovery compares raw `FName` values (via `FindPropertyByName`) and
  only converts to text once the offset is confirmed.
  All game-struct property offsets come from `FindPropertyByName` at runtime, never from constants.
- **Game thread** (`src/gamethread.cpp`): the engine `Tick` is hooked by vtable slot swap. Hooking
  the declaring class is not enough — a derived class has its own vtable holding the *same* inherited
  pointer. The plugin therefore sweeps every exported `_ZTV*` symbol (44 800 of them) and swaps every
  slot holding the target. On this build the live engine is `UDomGameEngine`, whose slot 98 holds
  `UJgxGameEngine::Tick`; hooking only `_ZTV14UJgxGameEngine` never fires. The detour calls the
  original first, then drains at most 16 queued jobs within `TAKARO_TICK_BUDGET_US`. Workers only
  enqueue owned jobs and wait (5 s → 503). See `gamethread-policy.md`.
- **Hooks** (`src/hooks.cpp`): vtable slot swaps only, with `mprotect` + restore on unload. No inline
  detours. `/health.diagnostics.resolved` reports `hooked` and `fired` per name.
- **Native bridge** (`src/native_*.cpp`): `native_transport` (libwebsockets + OpenSSL, heartbeat,
  bounded queues), `native_bridge` (frames, requests, deadline, outbox, ring/raw-log intake,
  expiry/recovery/reconcile scheduling), `native_behavior` (Takaro shapes, ban journal),
  `native_persistence` (atomic state files), `native_log` (PCRE2 log grammar, redaction, noise).
  Statically linked, pinned dependencies in `third_party/README.md`; nothing is exported from the
  `.so` (`exports.map`).
- **Actions** (`src/actions.cpp`): every UObject touch runs inside `GameThread::Run`. The world is
  found through `GetObjectsOfClass(UWorld)`; players come from `GameState.PlayerArray`; the EOS id is
  found by scanning the first eight words of `APlayerState::UniqueID` for a pointer whose vtable is
  `_ZTV15FUniqueNetIdEOS` (never assume the `TSharedPtr` offset, and never stringify an unvalidated
  `FName`). `UTeleportationSubsystem::GetTargetLocationNames()` must **not** be called: its return
  convention does not match a by-value `TArray<FName>` here and it crashed the server once.
  Moderation state is `UDedicatedServerSettings::KnownPlayerList[…].bIsBanned` + `PerformConfigSave`,
  not `BannedUserList`/`SetBannedUsers`, which the game ignores on this build.
- **Safety**: every handler is wrapped in `try/catch(...)`; every game pointer is checked against a
  lock-free `/proc/self/maps` snapshot before dereference; a failed capability degrades with a reason and the
  server keeps running.
