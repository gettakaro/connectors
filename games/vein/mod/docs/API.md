# Takaro VEIN plugin: optional diagnostic HTTP API

The native connector calls its game actions and reads its event ring directly. HTTP is an optional operator diagnostic interface, not its Takaro transport. It listens only on loopback port 18890 when `TAKARO_PLUGIN_TOKEN` is configured; without that token the listener is disabled. The historical endpoint shapes below remain useful for testing and migration, but the old sidecar no longer owns the protocol.

The plugin is `libtakaro-vein.so`, loaded into `VeinServer-Linux-Test` through
`LD_PRELOAD`. With diagnostics enabled, it serves this API on `127.0.0.1:18890` inside the server's network namespace.

Contract shape, status codes and the `/events` cursor semantics are the Takaro Enshrouded plugin's
`API.md` v0.4; identity, endpoints and diagnostics are adapted to Vein.

## Status of this document

This document describes the diagnostic contract and response shapes. Its original lane/M0 planning status is historical: actions and events have since run in a real VEIN server. Live coverage remains specific to each tested connector build; endpoint availability alone does not prove real-client effects or Takaro receipt. See the connector README for the compatibility reference and the candidate's validation results for current coverage.

Mechanism details are in `docs/actions-design.md` and `docs/events-design.md`. Examples marked *(shape)* describe the wire format, not evidence of a particular live run. Symbol health, installed hooks and HTTP success do not replace real-client and Takaro verification.

## Transport and auth
- HTTP/1.1, one short-lived thread per connection, every response closes it (`Connection: close`).
  Bodies are JSON (`Content-Type: application/json`), UTF-8.
- Every request needs `Authorization: Bearer <token>`.
  - Token from env `TAKARO_PLUGIN_TOKEN`, else `<server binary dir>/takaro/plugin.json` `{"token":"..."}`.
  - Wrong or missing request token: `401 {"error":"unauthorized"}`. When no token is
    configured, the HTTP listener is disabled. Direct native Takaro communication does
    not require this diagnostics token.
- Errors are always `{"error":"<message>"}`:
  - `400` bad body or missing field · `404` unknown path or player not online · `405` known path,
    wrong method · `409` the game refused the action · `413` body over 1 MiB ·
    `501 {"error":"unimplemented", "capability":"..."}` · `503` the game thread is unavailable.
- POST bodies are parsed **before** the 501 check, so a malformed body is a `400` even for an
  action that is not wired yet.
- The listener binds loopback only and retries every 5 s if the port is busy.

## Configuration
| env | `plugin.json` key | default | meaning |
|---|---|---|---|
| `TAKARO_PLUGIN_TOKEN` | `token` | — | bearer token; without it HTTP diagnostics are disabled |
| `TAKARO_PLUGIN_PORT` | `port` | `18890` | loopback port |
| `TAKARO_PLUGIN_DEBUG` | `debug` | off | enables `/debug/*` and per-request logging |
| `TAKARO_SYM_PATH` | `symPath` | `<exe>.sym` | depot symbol database for the `depotsym` strategy |
| `TAKARO_PLUGIN_DATA_DIR` | — | `<exe dir>/takaro` | `plugin.log`, `symcache.json`, `plugin.json` |
| `TAKARO_ADMIN_STEAMIDS` | `adminSteamIds` | — | comma/semicolon/whitespace-separated SteamID64s that the plugin grants in-game admin (see **Admin grants** below). `steam:<id>` is accepted; anything that is not a SteamID64 is dropped and listed in `/health.diagnostics.admins.rejected` |
| `TAKARO_SUPERADMIN_STEAMIDS` | `superAdminSteamIds` | — | parsed and reported, but **never applied**: this build ships no super-admin setter. See **Admin grants** |
| `TAKARO_TICK_BUDGET_US` | `tickBudgetUs` | `500` | *(lane L9)* how many microseconds of one engine tick the queued-job pump may use. Leftover jobs wait for the next tick; one job always runs, so a slow job cannot starve the queue. Clamped to 50…33000 |
| `TAKARO_SNAPSHOT_TTL_MS` | `snapshotTtlMs` | `500` | *(lane L9)* how long the player snapshot (`/players`, `/players/{id}`, `/players/{id}/location`) may be served from cache before the game thread is entered again. The refresh is lazy: with no requests there are no entries. Clamped to 1…30000 |

Logs go to `<data dir>/plugin.log`. Every line is passed through the redactor, which masks
`WorldPassword`, `AdminPassword`, `ServerPassword`, `Password=` and `*Token` values in ini,
JSON and CLI form.

## Identity
`gameId` is the player's **SteamID64** (17 decimal digits). VEIN is a Steam-only title and the
server authenticates through `OnlineSubsystemSteam`, so `steamId` equals `gameId` and `platformId`
is `steam:<gameId>`. `name` is the in-game player name.

The plugin reads the id in three steps, cheapest and safest first: a replicated id **string**
property on `AVeinPlayerState` (`OnlineID`, which `AVeinPlayerState::OnRep_OnlineID` replicates);
else the stock `APlayerState::UniqueID`, whose `FUniqueNetIdSteam` holds the id as a raw `uint64`,
found by scanning the struct's first words for the SteamID64 bit pattern
(`(v >> 32) == 0x01100001`) - no virtual call and no `FName` stringification; else
`AVeinPlayerState::GetPlayerUniqueID()`, the game's own getter, last because it is the only route
that runs game code. *(Not proven yet.)*

## GET /health

The additional top-level `native` object reports `connection` (state, identified,
epoch), queue counts, completion bytes, delivery losses, overloads, persistence
errors, and capture/confirmation sequences. During the opt-in
[native transport gate](native-gate.md), `native.gate.experimental` is true and
`native.gate.durableOutbox` is false. Those fields explicitly distinguish the
experimental transport from a completed persistent migration.

```json
{"status":"ok","version":"0.1.0","bootId":"c07f7763cfbf47c9","pid":48,
 "gameBuild":"++vein+staging-CL-240163","engineVersion":"5.6.1-240163",
 "buildId":"3b4ce30aed886594","uptimeMs":26709,
 "capabilities":{"gameThread":"ok","reflection":"ok","players":"unimplemented", ...},
 "capabilityDetails":{"players":"lane L3 not wired yet", ...},
 "symCache":{"path":"...","symPath":"...","buildId":"...","hit":true,"loadMs":0},
 "diagnostics":{
   "resolve":{"resolved":61,"wanted":66,"required":9,"requiredResolved":9,"processEventSlot":77,
              "processEventSlotHow":"_ZTV7UObject",
              "strategies":[{"name":"symtab","available":true,"resolved":61,"ms":812,"detail":"..."},
                            {"name":"depotsym","available":false,"resolved":0,"ms":0,"detail":"..."},
                            {"name":"dynsym","available":true,"resolved":0,"ms":0,"detail":"..."},
                            {"name":"signature","available":true,"resolved":0,"ms":0,"detail":"..."}]},
   "selfChecks":[{"check":"_init: sym=0xdc2dd90 elfSection+slide=0xdc2dd90","ok":true}, ...],
   "reflect":{"objName":24,"classDefaultObject":272, ...,
              "reference":{"source":"UE4SS-Vein MemberVariableLayout.ini (Vein 5.6.1)","mismatches":[]},
              "validations":[{"check":"findFunctionNative","ok":true,"detail":"..."}]},
   "gameThread":{"installed":true,"how":"...","alive":true,"tickCount":698,"tickThreadId":47,
                 "tickThreadChanges":0,"approxHz":28.8,"jobsRun":2,"jobsTimedOut":0,"maxJobMs":9},
   "http":{"port":18890,"requests":12,"unauthorized":2,"handlerErrors":0},
   "admins":{"configured":["<your-steamid64>"],"applied":["<your-steamid64>"],
             "sessionArrayNum":1,"sessionArray":["<your-steamid64>"],"status":"ok","lastError":"",
             "rejected":[],"passes":14,"grants":1,
             "superAdmins":{"configured":[],"applied":[],"sessionArrayNum":0,"rejected":[],"reason":"not configured"},
             "via":"AVeinGameSession::SetAdmin(FString,bool) through UObject::ProcessEvent; ..."},
   "events":{"buffered":0,"latestSeq":0},
   "hooksInstalled":3,
   "resolved":[{"name":"UObject::ProcessEvent","rva":"0x4e3cf50","addr":"0x503cf50","how":"symtab",
                "hooked":false,"fired":0,"signature":"UObject::ProcessEvent(UFunction*, void*)",
                "records":1,"required":true,"contiguous":true}, ...]}}
```
- `how` names the strategy that produced the address: `symtab`, `depotsym`, `dynsym`, `signature`,
  `symcache:<strategy>` (cached from a previous boot of the same build id) or `rejected`.
- `status` is `"ok"` only when every self-check passed and `reflection` is `ok`; otherwise `"degraded"`.
  A degraded plugin keeps serving; the server is never affected.
- `capabilities` values are `"ok"`, `"degraded"` or `"unimplemented"`; the reason for a degrade is in
  `capabilityDetails`. `"ok"` means resolved/hooked, **not** proven — live proof lives in
  `context/games/vein/evidence/`.
- `bootId` is random per server process. A different `bootId` means the server restarted: reset the
  event cursor to `0` even if `seq` is higher than yours.
- `diagnostics` is informational. Do not code against its exact shape.

## GET /events?since=\<seq\>[&limit=\<n\>]
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
  `log`. All are wired (lane L2). `entity-killed` carries the victim's display name as `entity`,
  its Blueprint class as `entityCode`/`entityClass`, the spawned actor's instance name as
  `entityInstance`, and `attribution` naming how the killer was determined. Only `entity`, `weapon` and `player` are
  forwarded to Takaro; the rest is plugin-side evidence, because Takaro's `EventEntityKilled` schema
  does not carry it.

### Event payloads (lane L2)
The `player` object is `{gameId, name, steamId, platformId, characterName?, platformName?}`.
`gameId` **is** the SteamID64 (`AVeinPlayerState::OnlineID`), `steamId` equals it and `platformId` is
`steam:<gameId>`. `name` is the VEIN character name once one is known, else the Steam persona; both
are also carried separately as `characterName` and `platformName`, because VEIN really does have two
names per player and Takaro only has room for one. The character name is not on the player state —
it lives on the pawn and reaches the plugin through the server log's
`Player <persona> selected character <id> (aka <name>)` line — so a join event may legitimately
carry only the persona.

```json
{"type":"player-connected","data":{"player":{"gameId":"<your-steamid64>","name":"Takaro Tester","characterName":"Takaro Tester","platformName":"Limon","steamId":"<your-steamid64>","platformId":"steam:<your-steamid64>"}}}
{"type":"player-disconnected","data":{"player":{…}}}
{"type":"chat-message","data":{"msg":"hello","channel":"local","chatSegment":"Local","player":{…},"source":"processevent"}}
{"type":"player-death","data":{"player":{…},"position":{"x":0,"y":0,"z":0},"attacker":{…}?,"killerEntity":"BP_Zombie_Male_C"?,"cause":"DeathCause:3"?,"source":"NetMulticast_OnDeath"}}
{"type":"entity-killed","data":{"entity":"Zombie","entityInstance":"BP_Zombie_C_2147462244","entityCode":"BP_Zombie_C","entityClass":"BP_Zombie_C","weapon":"Baseball Bat"?,"position":{…}?,"source":"NetMulticast_OnDeath","attribution":"the death event's instigator controller","player":{…}}}
{"type":"log","data":{"msg":"[2026.09.17-07.10.00:001][123]LogNet: Login request: ?Password=<redacted>?Name=Limon??ID=<your-steamid64>?Ticket=<redacted>"}}
```

- **`chat-message`** — `channel` is the `EChatSegment` the chat RPC actually carried
  (`All=0, Local=1, Global=2, Radio=3`, confirmed from the depot's DWARF), lower-cased:
  `"all"` | `"local"` | `"global"` | `"radio"`. **Changed by lane L3e**: it used to be a constant
  `"global"`, which L6b caught making a message typed on the Local channel indistinguishable from a
  global one — so Takaro's `onlyGlobalChat` relayed local chat too. A call that carries no segment at
  all, or a value outside the enum, still reports `"global"` (the old default); the segment's display
  **name** continues to ride along as `chatSegment`.
  `source` is `processevent` (the `Server_Say` / `NetMulticast_SendChat` hook) or `log` (the
  `LogVeinChat:` line); the same message seen by both is emitted once (8 s dedupe). Messages the
  plugin injected through `POST /message` are never re-emitted.
- **`player-death`** — `position` is omitted when neither the death event nor the pawn gave one.
  `attacker` only appears for a *player* killer **other than the victim**; a creature goes into
  `killerEntity` under its display name (`Zombie`, `Wolf`), the same name `GET /entities` uses.
  **Lane L2c**: VEIN passes the victim's own pawn as `DamageCauser` and his own controller as
  `DamageInstigator` for a fall, drowning or the cold, and the plugin used to copy that into
  `attacker` — every environmental death arrived in Takaro as a death by the dead player's own hand
  and scored as PvP. A candidate that is the victim is now skipped: such a death carries no
  `attacker` and no `killerEntity`, and `cause` says what is known instead (VEIN's own
  `DeathCause`/`DeathReason`, else the humanised damage-type class, else `environment`); the
  sidecar renders it as `msg: "<name> died (<cause>)"`. A genuinely self-inflicted death is
  indistinguishable from a fall on this build — the wire carries the same three pointers — so it,
  too, reports as environmental. `cause` is VEIN's own `DeathCause`/`DeathReason` when the property
  is readable. `source` is the UFunction that fired, or `health-edge` for the fallback that watches
  `Dead`/`Health` per cycle. Dedupe: 3 s per `gameId`.
- **`entity-killed`** — VEIN routes *every* death through one event, so the dying actor's class is
  what makes this an entity kill rather than a player death. **Lane L2c** settled the three fields:
  - `entity` is the **victim** — the actor that owns the health component that fired the death
    event — named exactly as `GET /entities` names it: the animal's `UsableName` (`Wolf`, `Boar`)
    and otherwise the humanised class name (`BP_Zombie_C` → `Zombie`). The raw class stays in
    `entityCode`/`entityClass` and the spawned actor in `entityInstance`.
  - `player` is the **killer**, resolved only from the death event's own `DamageInstigator` /
    `DamageCauser` or an instigator property on the victim. When none of those names a player the
    event is **not emitted at all** (Takaro's `entity-killed` is a player's kill) and the death is
    counted in `/health.diagnostics.eventSources.aiKillsWithoutPlayer`.
  - `weapon` is the **killer's weapon**: the display name of the `UItem` behind the `AEquippedItem`
    the death event names as `DamageCauser` (`Baseball Bat`), else the humanised class of a
    weapon-ish causer (a projectile, a bullet). When the causer names no weapon it is `"debug"`
    (this actor was killed by `POST /debug/kill-nearest` — nothing was swung) or `"unknown"`. It is
    never a pawn class and never the victim's. It is deliberately never `"unarmed"`: a punch and an
    unnamed weapon are identical on the wire on this build, so that would be a claim, not a reading.
    The field is always present because Takaro's `EventEntityKilled` **requires** it — measured
    2026-09-17, an event without it fails validation (`property weapon … isString`) and is discarded
    whole.

  `attribution` remains a diagnostic field naming the route that produced the killer (the death
  event's instigator controller, its damage causer, or an instigator property on the AI). Only
  `entity`, `weapon` and `player` reach Takaro, the rest is plugin-side evidence. Deaths of doors,
  built actors and item instances share the same game event and are counted
  (`nonPlayerDeathsIgnored`) and dropped rather than reported as kills.
- **`log`** — lines are redacted before they leave the process: the join URL carries the join
  password **and** a Steam auth session ticket in cleartext
  (`LogNet: Login request: ?Password=…?Name=…??ID=…?Ticket=…`), so `Password`, `Ticket`,
  `AuthTicket`, `Token`, `Secret` and `?p=` are masked, while the SteamID64 and the display name are
  kept (they are not secrets and the join fallback needs them). UE noise categories are dropped and
  at most 120 lines are emitted per 2 s cycle; `logDropped` counts the rest.
- Event sources and their hook state are in `/health.diagnostics.eventSources`:
  `{source, hooked, fired, emitted, note}` per type, plus `processEventVTables`,
  `trackedConnections`, `nonPlayerDeathsIgnored`, `functionCandidates` (which chat/death UFunction
  names exist in the engine name pool and how often each was seen), `liveFunctions` (which ones
  actually fired), `classesHooked`, `logPath`, `logDropped`, `logChatLines` and `logJoinsEmitted`.
  A capability is `ok` only when a hook is **installed**; `"; not yet observed firing"` in the detail
  string means exactly that.

### `POST /debug/kill-nearest` *(debug-gated: token **and** `TAKARO_PLUGIN_DEBUG=1`)*
| body | response |
|---|---|
| `gameId?` (default: the only announced player), `radius?` (cm, default 5000) | `{success:true,entityClass,entityInstance?,distance,damageApplied,instigator,detail}` |

Kills the AI nearest to that player by driving `UGameplayStatics::ApplyDamage` **by reflection** —
the UFunction is found on the `UGameplayStatics` CDO and called through `ProcessEvent` with a
parameter block laid out from `FindPropertyByName`, with a real looked-up `UDamageType` class (never
a null `TSubclassOf`; without one the endpoint answers `501` instead of risking the server). It
exists so `entity-killed` can be proven without a human swinging a weapon, and it deliberately does
**not** emit the event itself: the kill has to come out of the normal hook or it proves nothing.
`404` when no player is online or no AI is within the radius, `503` when the game thread is busy.

## Admin grants (lane L3b)

**`[/Script/Vein.VeinGameSession] +AdminSteamIDs=` can never work on this build.** Lane L1 proved it
on the live class default object: the scalars in that same ini section load fine (`ServerName`,
`bPublic`, `HTTPPort`, `Password`) while `AdminSteamIDs` and `SuperAdminSteamIDs` are `arrayNum: 0`
on every boot regardless of `+Key=` syntax — the two `TArray<FString>` properties are simply not
`UPROPERTY(Config)`. So the file, the section and the syntax are all right and nothing loads, and
without an entry in that array the in-game admin panel never opens.

The route that does exist is the UFUNCTION **`AVeinGameSession::SetAdmin(FString, bool)`**
(`execSetAdmin` is in the depot `.sym` at `0x907d83a`; the function itself at `0x91e46be`). Set
`TAKARO_ADMIN_STEAMIDS` and the plugin:

1. waits for the pump to tick and for a live (non-CDO) `AVeinGameSession` to exist;
2. reads `AdminSteamIDs` **by reflection** (`UStruct::FindPropertyByName`, never the `0x340`
   constant L1 measured) and calls `SetAdmin(id, true)` through `UObject::ProcessEvent` for every
   configured id the array does not already list;
3. re-grants for a configured player who has (re)joined since the last grant, and additionally calls
   `AVeinPlayerState::SetAdmin(true)` on their live player state, because the client-side panel keys
   off the replicated player-state flag which is set from the array at login;
4. reads the array back and reports it in `/health.diagnostics.admins`.

The pass runs every 2 s from the housekeeping thread and is idempotent — with nothing to do it costs
one property read.

`/health.diagnostics.admins`:

| field | meaning |
|---|---|
| `configured` | the validated ids from `TAKARO_ADMIN_STEAMIDS` |
| `applied` | ids `SetAdmin` has been called for successfully this boot |
| `sessionArrayNum` | `AdminSteamIDs.Num()` on the live session, read back by reflection (`-1` = property not found / no session yet) |
| `sessionArray` | the array's contents, so `applied` can be checked against what the game actually holds |
| `status` | `idle` \| `no-session` \| `ok` \| `error`; `lastError` carries the reason |
| `rejected` | tokens that were not SteamID64s |
| `superAdmins` | `{configured, applied: [], sessionArrayNum, rejected, reason}` |

**There is no super-admin setter.** The depot `.sym` for build `f0653d6e815bb2c3` has
`AVeinGameSession::SetAdmin`, `AVeinPlayerState::SetAdmin`, `AVeinPlayerState::SetAdminPermissionsFromID`
and `UAdminComponent::Server_SetAdmin`, plus `IsAdmin()`/`IsSuperAdmin()` getters on
`AVeinPlayerController`, `AVeinPlayerState` and `UAdminComponent` — and **no `SetSuperAdmin` of any
spelling**. Populating `SuperAdminSteamIDs` would mean writing the `TArray<FString>` ourselves with
the game's allocator, which this lane deliberately does not do. `TAKARO_SUPERADMIN_STEAMIDS` is
therefore parsed, validated and reported with `applied: []` and a reason.

**UNVERIFIED:** whether a granted id actually opens the in-game admin panel, and whether the nine
`UAdminComponent` RPCs behind `giveItem`/`kick`/`ban`/`teleport`/`message`/`executeCommand`/`shutdown`
gate on `IsAdmin()` server-side. A 0x400-byte disassembly of `Server_GiveItem_Implementation`,
`Server_KickPlayer_Implementation` and `Server_MovePlayer_Implementation` contains **no call** to
either `IsAdmin` address — but `IsAdmin()` is a one-line flag read that the compiler would inline, so
that is not evidence of absence. Only the live proof run settles both.

### `POST /debug/set-admin` *(debug-gated: token **and** `TAKARO_PLUGIN_DEBUG=1`)*
| body | response |
|---|---|
| **gameId** (SteamID64, `steam:` prefix accepted), `admin?` (bool, default `true`) | `{success:true,gameId,admin,sessionArrayNum,listedInSessionArray,via}` |

Grants or revokes on demand so a proof lane can take the before/after of the admin panel without a
restart. `400` on a missing or malformed `gameId`, `501` when there is no live session or no
`SetAdmin` UFUNCTION (the body says which), `503` when the game thread is busy.

## Debug endpoints (need the token *and* `TAKARO_PLUGIN_DEBUG=1`; otherwise `404`)
| endpoint | returns |
|---|---|
| `GET /debug/gamethread` | `{"ranOnThreadId":47,"httpThreadId":123,"latencyMs":31,"stats":{...}}`. Runs a no-op job on the game thread. `503` if the pump never ticked. |
| `GET /debug/symbols` | the ELF facts, the symcache block and the full resolved table. |
| `GET /debug/perf[?reset=1]` | *(lane L9)* the game-thread performance counters — the same object as `/health.diagnostics.perf`. `reset=1` answers with the current window and then starts a fresh one. See **Performance counters** below. |
| `GET /debug/object?path=/Script/Pkg.Name` or `?ptr=0x...` | the object's UPROPERTY tree up the class chain: `{name, type, offset, value}` per property, values decoded for primitives, `FString`, `FName` and object pointers. When the object is itself a `UClass`/`UScriptStruct` the properties it *declares* are listed under `declaredProperties`. `ptr` is refused unless it is inside a readable mapping. |
| `POST /debug/set-admin` | grants/revokes in-game admin — see **Admin grants** above. |
| `GET /debug/inventories?gameId=<id>` | *(lane L3f)* which container the plugin answers `getPlayerInventory` from, and which ones exist: `{gameId,name,characterId,controller,controllerPawn,playerStatePawn,pawnsAgree,spawned,chosen,chosenEntries,chosenRejectedBecause,legacySweep:[{component,owner,ownerIsCurrentPawn,entries,acceptedNow}]}`. `pawnsAgree:false` is the stale-pawn condition; `legacySweep` is what the pre-L3f resolution would have picked, in the order it picked it. |
| `GET /debug/structs?name=X` | property table of a `UClass`/`UScriptStruct`. `X` is a full path (`/Script/JagexChatBackend.ChatMessageData`) or a plain name, which is resolved by scanning the live object array (so the owning module does not have to be known). |

Example *(shape — Dragonwilds output; VEIN structs differ)*:
```
GET /debug/structs?name=ChatMessageData
{"package":"/Script/JagexChatBackend","name":"ChatMessageData","class":"ScriptStruct",
 "propertiesSize":136,"super":"",
 "properties":[{"name":"SenderData","type":"StructProperty","offset":0},
               {"name":"MessageBody","type":"StrProperty","offset":120}]}
```

## Performance counters (lane L9)

`GET /debug/perf` and `/health.diagnostics.perf` return the same object. It exists because the
plugin runs on the server's game thread, and that claim ("it costs almost nothing") has to be
measurable rather than asserted. The policy it enforces is
`context/games/vein/source/plugin/docs/gamethread-policy.md`.

```
{"windowMs":600000,
 "tick":{"samples":1000,"count":36000,"avgUs":3.1,"p50Us":2.1,"p99Us":12.4,"maxUs":5358.2,
         "ringMaxUs":41.2,"hz":60.0,"budgetHits":0},
 "jobs":{"count":120,"perSecond":0.2},
 "gameThreadEntries":{"count":120,"perSecond":0.2},
 "processEventFilter":{"calls":36000,"callsPerSecond":60.0,"avgNs":180.0,"maxNs":41280,
                       "hits":0,"hitsPerSecond":0,"cacheHits":35980,"totalMsInWindow":6.5},
 "eventHandlers":{"calls":0,"avgUs":0,"maxUs":0},
 "sweeps":{"housekeep.sweep":{"calls":40,"perSecond":0.07,"avgUs":900.0,"maxUs":2391.8,"totalMs":36.0}, ...}}
```

| field | meaning |
|---|---|
| `tick.avgUs` / `p50Us` / `p99Us` / `maxUs` | wall time **our** Tick detour spends after the engine's own Tick returns. `p50`/`p99` are over the last 1000 ticks (`samples`); `maxUs` is since the last reset and includes the boot-time work. |
| `tick.ringMaxUs` | the maximum inside the 1000-tick window — the number to compare against the ~33 ms frame. |
| `tick.budgetHits` | ticks that stopped draining jobs because `TAKARO_TICK_BUDGET_US` was spent. |
| `jobs` / `gameThreadEntries` | jobs drained, and jobs *enqueued* from off-thread work. `gameThreadEntries.perSecond` is the number the game-thread policy is about: with nobody online and nobody polling it should be near zero. |
| `processEventFilter` | our `ProcessEvent` detour's decision cost: `avgNs` is per engine RPC through a hooked vtable, `cacheHits` is how often the `UFunction*` decision cache answered without reading an `FName`, `hitsPerSecond` how often a call was actually one of ours. |
| `eventHandlers` | time inside a handler we chose to run (chat / death dispatch), i.e. only on a real event. |
| `sweeps` | every named piece of periodic or per-request game-thread work: `housekeep.*`, `admin.pass`, `catalogue.items`, `snapshot.players`, and one entry per HTTP endpoint that entered the game thread. `*.cacheHit` entries count reads that were served **without** entering it. |

Measured before/after numbers for this build are in
`context/games/vein/evidence/2026-09-17-l9-performance.md`.

## Actions (lane L3, corrected by lane L3e) — every mutating endpoint verifies its own effect

Identity: `gameId` is the SteamID64. `GET /players/{id}`, `/give`, `/teleport`, `/kick`, `/ban`,
`/unban` also accept the player name for convenience.

VEIN ships a complete server-authoritative admin API, `UAdminComponent`, which is what the in-game
admin panel drives, and lane L3 built nine of the fifteen actions on that component's own RPCs,
called through their `*_Implementation` address.

**Lane L3e changed that, because it does not work.** Every
`UAdminComponent::Server_*_Implementation` opens with an owner-side
`AVeinPlayerController::IsAdmin()` check on the component's **own owner**, not on the player named
in its arguments. A component with no live owning controller — the class default object — fails that
check, so `Server_KickPlayer`, `Server_GiveItem` and `Server_MovePlayer` returned `void` having done
nothing at all, and the plugin reported `{"success":true}` over the top of it (L6b cells 4, 5, 6;
`Server_SendServerMessage` went one step further and SIGSEGV'd on the null owner). The admin RPCs are
therefore now the **last** mechanism of each action, never the first, they are only ever called on a
component owned by an *online* controller, and their `via` string says
`(requires an admin online)` out loud.

### The verification contract

Every mutating endpoint — `/kick`, `/give`, `/teleport`, `/ban`, `/unban`, `/message` — tries its
mechanisms cheapest-and-ungated first, **verifies each one before trying the next**, and answers
with what it observed, not with what it called:

| status | meaning | body |
|---|---|---|
| `200` | the effect was observed | `{"success":true,"verified":true,"via":"<the mechanism that produced it>", …the observation}` |
| `409` | every mechanism ran and nothing changed | `{"success":false,"verified":false,"attempted":[…],"error":…}` |
| `501` | no mechanism was available at all | `{"success":false,"verified":false,"attempted":[],"error":…}` |
| `503`/`504` | the game thread was unavailable or the job never completed | `{"error":…}` |

What "observed" means per endpoint: **kick** — the player is no longer in
`AGameStateBase::PlayerArray`; **teleport** — the pawn's root-component location moved *and* is
within 500 cm of the target; **give** — the summed inventory stack count for that item code went up;
**ban**/**unban** — a fresh read of both ban lists shows / no longer shows the id. **`/message` is
the single exception** and says so in its own body: a chat multicast leaves no server-side state to
read back, so it reports `verified:false` with `verifiedBy:"dispatch-only…"` rather than claiming an
effect it cannot see.

`via` always names the mechanism that actually produced the effect, and `attempted` lists everything
that ran, so a proof run records the real path.

| endpoint | body (required in bold) | response |
|---|---|---|
| `GET /players` | — | `[{gameId,name,steamId,platformId:"steam:<id>",ping,spawned,pawn,characterId,online:true,connectedAt}]` from `GameState.PlayerArray`. `pawn` is the **controller's current** pawn class (lane L3f: `AController::Pawn`, not `APlayerState::PawnPrivate`, which can still name the pre-death character after a respawn). `spawned` is "that pawn is an `AVeinPlayerCharacter`" (F14). `characterId` is `AVeinPlayerState::LoadedCharacterID` printed as 32 upper-case hex digits — the **same** value VEIN's own `127.0.0.1:8080/status` reports — or `null` before a character is loaded |
| `GET /players/{id}` | — | one player object, else `404 {"error":"player not online"}` |
| `GET /players/{id}/location` | — | `{x,y,z,yaw,pitch}` (UE cm, doubles) from the pawn's root component; `503` when the player has no pawn yet |
| `GET /players/{id}/inventory` | — | `[{code,name,amount,inventory,slot,assetPath}]` — VEIN's items are *virtual*: `UBaseInventoryComponent::Items` is an `FInventoryArray` whose own `Items` member is a `TArray<FVirtualItemInstance>`. `amount` is **1 per array entry unless the item class's `bStackable` is true**, in which case it is the entry's `Stack` (lane L3f/F19: VEIN's `bPseudoStackable` only makes the UI *group* identical rows — `BP_Corn_C` is `bStackable=false, bPseudoStackable=true, MaxStack=50`, so one entry is one corn however large its `Stack` field is); `name` is its `CustomLabel`, and `code` the `UItem` class behind its `TSoftClassPtr`, so it round-trips into `POST /give`. Pure reflection, no call into the game. **Lane L3f / finding F19:** the component is resolved *strictly* as `AVeinCharacter::Inventory` on the **controller's current pawn** — not from a sweep of every `UBaseInventoryComponent` under the pawn or the controller, which could return a `UPersistentCorpseInventory` (a `UBaseInventoryComponent` **subclass**) or the controller's `UOfflineCharacterCache` and reported items the player was not carrying. `404 {"error":"no character: …"}` when the player has no character pawn (character screen, or dead and not yet respawned) — never an empty array, and never another container's items |
| `GET /items[?search=]` | — | `[{code,name,description,category}]` — every loaded `UClass` deriving from `UItem`. `search` matches code or name, case-insensitively. `name` is the class name unless `TAKARO_ITEM_NAMES=1` is set, which makes the plugin call `UItem::GetDefaultNameInvariant()` on each class default object for the readable name |
| `GET /entities` | — | `[{code,name,type,description}]` — the `AVeinZombieCharacter` (`type:"hostile"`) and `AVeinAnimalCharacter` (`type:"passive"`) subclasses loaded so far. VEIN streams its AI content, so this is not the full bestiary |
| `GET /locations` | — | `[{code,name,position:{x,y,z}}]` — the `ALocationMarker` actors currently streamed in. The world is partitioned, so this is not every point of interest |
| `GET /bans` | — | `[{gameId,name,reason,expiresAt,createdAt,enforcedBy}]` — the union of VEIN's own replicated `TArray<FBan>` on the game state and the plugin ban list in `<serverdir>/takaro/bans.json`. The game's list carries the id and the reason; `createdAt` and `expiresAt` come from the plugin list (timed bans are the sidecar's job). `enforcedBy` is `"game"` for an entry the game itself will refuse and `"plugin"` for one only the plugin list carries |
| `POST /message` | **text**, `recipientGameId?`, `senderName?` | `{success:true,verified:false,verifiedBy:"dispatch-only…",delivered:<n>,via}`. **It is the one mutating endpoint that cannot be verified (L3e):** a chat multicast leaves no server-side state to read back and whether a client rendered it is only visible on the client, so it reports `verified:false` and says why rather than claiming an effect it cannot see. The 200 means the multicast was dispatched on the game thread and the job ran to completion. Without a recipient the text is broadcast through `AVeinGameStateBase::NetMulticast_BroadcastServerMessage` — the replicated multicast entry that takes **no sender**, so it reaches every client and cannot null-deref — falling back to `UAdminComponent::Server_SendServerMessage` and, only when a real `AVeinPlayerState` sender exists, to `AVeinGameStateBase::NetMulticast_SendChat` (the line then wears that player's name; `via` says which sender). `NetMulticast_SendChat` is **never** called with a null sender: that SIGSEGVs the server. Text is prefixed `[<senderName>] <text>`. With no safe path: `503 no sender available`. **With** a `recipientGameId` it is delivered as that one player's on-screen notification (`AVeinPlayerController::Client_SendNotification`), because a multicast cannot be targeted. `senderName` defaults to `TAKARO_SENDER_NAME`, else `Server`. Order pinnable with `TAKARO_BROADCAST_VIA` = `chat`\|`servermessage`\|`admin`\|`auto` |
| `POST /teleport` | **gameId**, **x**,**y**,**z** *or* **target**, `yaw?` | **Verified (L3e).** `{success:true,verified:true,via,position:{x,y,z},from:{…},target:{…},offsetCm}`. Mechanisms in order, each verified before the next is tried: `AActor::TeleportTo(FVector,FRotator,false,bNoCheck)` on the live pawn (stock engine, no admin gate) → `K2_TeleportTo` through `UObject::ProcessEvent` → `UAdminComponent::Server_MovePlayer` on an *owned* admin component (requires an admin online). Success means the pawn **both** left `from` **and** ended within 500 cm of `target`, read back off its root component after the call; the pre-call position is never echoed as the result. `404` player offline or unknown named `target`, `409` connected with no character, or every mechanism ran and the pawn did not arrive (`verified:false`, `attempted:[…]`), `501` no mechanism available, `503` game thread unavailable |
| `POST /give` | **gameId**, **code**, `amount` (default 1, max 1000) | **Verified (L3e).** `{success:true,verified:true,code,name,amount,received,before,after,via,attempted}`. The player's own `UBaseInventoryComponent` add-item `UFunction` is called through `UObject::ProcessEvent`, with the item class going into the function's `TSubclassOf` parameter and the count into its integer parameter — both located by walking the `UFunction`'s own `ChildProperties`, never by a hard-coded offset, and an `ObjectProperty` is deliberately **not** accepted (that would mean the function wants an item instance). Candidate names, in order: `AddItem`, `TryAddItem`, `GiveItem`, `AddItemByClass`, `AddNewItem`, `ServerAddItem`, `CreateItem`, `AddItemOfClass`. Last resort `UAdminComponent::Server_GiveItem` on an *owned* admin component (requires an admin online). `amount` is split into one `AddItem` per stack by `ActionsUtil::StackSplit` using the item class's own `MaxStack` — and **one instance per unit when the item is not stackable** (lane L3f/F19: the old split built a single instance with `Stack=amount`, and VEIN ignores `Stack` on a non-stackable item, so `amount:3` of corn delivered one corn). Success means the summed stack count for that `code` went **up**; `received` is the real delta, which can be less than `amount` when the inventory ran out of room. `404` unknown code or player offline, `409` no character yet, or every mechanism ran and the count did not move, `501` nothing available |
| `POST /kick` | **gameId**, `reason?` | **Verified (L3e).** `{success:true,verified:true,gameId,online:false,via,attempted}`. Mechanisms in order: `AGameSession::KickPlayer(PC, FText)` (stock engine, carries the reason, no admin gate) → `APlayerController::ClientReturnToMainMenuWithTextReason(FText)` → `UAdminComponent::Server_KickPlayer` on an *owned* admin component (requires an admin online). After each one the player table is polled on the game thread — 5 s for the first mechanism, 1.5 s for the others, so the worst case still fits the sidecar's 10 s timeout — and success means **the player is no longer in `AGameStateBase::PlayerArray`**. `404` not online, `409` every mechanism ran and the player is still connected (`verified:false`, `attempted:[…]`), `501` nothing available |
| `POST /ban` | **gameId**, `reason?`, `expiresAt?` | **Verified (L3e).** `{success:true,verified:true,gameId,online,persisted,pluginList,via,enforcedBy,disconnected,detail}`. Three things happen, in this order: the id, reason and expiry go into the plugin ban list (`takaro/bans.json`), which accepts an id the server has never seen and which lane L2's `PreLogin` hook refuses a rejoin with immediately; the id goes into **VEIN's own** list through `AVeinGameStateBase::BanID(FString id, FString reason)`, which is keyed on the id string and therefore works offline too; and an online player is disconnected through the same mechanism ladder as `POST /kick`. `verified` is decided by **re-reading both ban lists**: if neither shows the id, the answer is `409`, because a ban that is not listed cannot refuse a rejoin. `disconnected` reports the observed session state (`disconnected`/`still connected`) and is deliberately separate from `verified` — a ban is a list entry, not a disconnect |
| `POST /unban` | **gameId** | **Verified (L3e).** `{success:true,verified:true,gameId,removedFromPluginList,removedFromGameList,via,detail}` — removes the id from the plugin list and from the game's list (`AVeinGameStateBase::UnbanID`), then **re-reads both**. `verified:true` means the id is in neither; while either one still holds it `PreLogin` still refuses the rejoin, so that is a `409`, not a success |
| `POST /command` | **command** | `{success,output}` — `output` is a string (JSON text for the list commands) |
| `POST /shutdown` | — | `{success:true,"detail":"saving, then SIGTERM"}`, then `UAdminComponent::Server_RequestDedicatedServerSave` → `SIGTERM` to our own pid; the container restarts under its compose policy |

### `POST /command` command set
```
help | players | bans | items [query] | entities | locations
say <msg> | whisper <gameId> <msg>
give <gameId> <code> [amount] | tp <gameId> <x> <y> <z>
kick <gameId> [reason] | ban <gameId> [reason] | unban <gameId>
save | shutdown
raw <console command>        # UEngine::Exec with our own FOutputDevice; output is captured
vein <admin exec command>    # UAdminComponent::Server_Exec; dispatched, but returns no output
cheat <gameId> <dom command> # 501: the dedicated server creates no CheatManager
```

### Extra configuration (lane L3)
| env | `plugin.json` key | default | meaning |
|---|---|---|---|
| `TAKARO_SENDER_NAME` | `senderName` | `Server` | the name a broadcast message is prefixed with |
| `TAKARO_CHAT_SEGMENT` | `chatSegment` | `2` (Global) | the `EChatSegment` value a broadcast uses. From the depot's DWARF: `All = 0, Local = 1, Global = 2, Radio = 3` |
| `TAKARO_ITEM_NAMES` | `itemNames` | off | `1` makes `GET /items` call `UItem::GetDefaultNameInvariant()` on each class default object for a readable name. Off by default because calling into a Blueprint CDO is the exact shape that crashed the Dragonwilds server |

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
  original first, then drains at most 16 queued jobs. HTTP threads only enqueue and wait (5 s → 503).
- **Hooks** (`src/hooks.cpp`): vtable slot swaps only, with `mprotect` + restore on unload. No inline
  detours. `/health.diagnostics.resolved` reports `hooked` and `fired` per name.
- **Actions** (`src/actions.cpp`, pure helpers in `src/actions_util.cpp`): every UObject touch runs
  inside `GameThread::Run`. The world is found through `GetObjectsOfClass(UWorld)`; players come from
  `GameState.PlayerArray`; the SteamID64 is found by scanning `APlayerState::UniqueID` for the
  `0x0110000100000000 | accountId` bit pattern rather than by stringifying a net id. Nine of the
  fifteen actions call `UAdminComponent`'s own server RPCs through their `*_Implementation` address.
  `FString`/`FText` arguments are non-trivial class types: under the Itanium ABI they are passed by
  invisible reference and destroyed by the **caller**, so every buffer the plugin hands the game is
  allocated with `FMemory::Malloc` and freed with `FMemory::Free` after the call returns.
  `AVeinGameStateBase::NetMulticast_SendChat` is called at its *replicated* address, never its
  `_Implementation`: the implementation alone would run the server half and never reach a client.
  `ALocationMarker::GetLocationName()` and `UItem::GetDefaultNameInvariant()` are **not** called by
  default - the readable name is read from a reflected property instead, because calling a getter on
  an uninitialised Blueprint CDO is what crashed the Dragonwilds server.
- **Safety**: every handler is wrapped in `try/catch(...)`; every game pointer is checked against a
  cached `/proc/self/maps` before dereference; a failed capability degrades with a reason and the
  server keeps running.
