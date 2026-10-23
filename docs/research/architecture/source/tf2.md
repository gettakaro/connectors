# TF2 (Team Fortress 2 / SourceMod): source-level analysis (source-only)

HEAD 3859fd8. All paths relative to the repo root. `sp` = `games/tf2/mod/addons/sourcemod/scripting/takaro_tf2.sp`.
Everything below is source-only: nothing was run.

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | In-process SourceMod plugin (`takaro_tf2.smx`) inside srcds; no sidecar, no inbound port, no RCON | games/tf2/DEVELOPMENT.md:3-6, catalog/tf2/game.json:39 |
| Runtime | SourcePawn VM (SourceMod 1.13), single-threaded plugin callbacks on the srcds main thread | games/tf2/README.md:48-50, games/tf2/capabilities.json:4 |
| Loader | MetaMod:Source 1.12 + SourceMod 1.13, installed by the operator (not shipped) | games/tf2/README.md:48-50, catalog/tf2/targets/linux-25995664.json:15 |
| Transport library | ProjectSky `sm-ext-websocket` 1.0.8 (native extension, ships in the zip) + `sm-ext-json` 1.1.5f | catalog/tf2/targets/linux-25995664.json:103-111; `sp`:4-5, `sp`:383 |
| TLS verification | Not controlled or configured by the plugin: `wss://` URL is handed to the extension (`sp`:81, `sp`:383); no CA bundle, no verify flag anywhere in plugin, build or docs (grep of build-release.sh/README/DEVELOPMENT/targets for cert/tls/verify: no hits). Whether the extension verifies certificates is unverifiable from this repo. | `sp`:383-392 |
| Reconnect | Extension-owned: `AutoReconnect = true`, min 1000 ms / max 30000 ms (cvars), `PingInterval = 30` | `sp`:388-391, games/tf2/mod/cfg/sourcemod/takaro_tf2.cfg:13-14 |
| Protocol | v0 and v1 (offers `[0,1]` + capability manifest; v1 has `streamId`/`seq` and cumulative `eventAck`) | `sp`:509-520, `sp`:1315-1320, `sp`:1348-1368 |
| Default catalog target | `linux-25995664` (default platform linux, `default: true`) and `windows-25995664`; both `support.status: candidate` (no `maintained` target yet) | catalog/tf2/game.json:10, catalog/tf2/targets/linux-25995664.json:6-9, catalog/tf2/targets/windows-25995664.json:7-9 |
| Connector version | 0.1.0 | games/tf2/version.txt:1, `sp`:12 |

## 2. Drift vs the issue

- `games/tf2` does not exist at 147acb21 (`git show 147acb21:games/tf2` -> "exists on disk, but not in 147acb21"). The issue has no TF2 section; its P1 inventory only lists baseline families (issue P1). TF2 must be appended as a "newly maintained first-party family" per P1, though today it is only `candidate`.
- Common-campaign hypotheses that apply: C1 (operator-installed loader + native extension), C3 (all game access on main thread: holds), C4 (one bounded outbox, no request queue), C5 (connector-owned ban ledger file), C6 (v1 acks + in-memory replay), C7 is N/A (already native; no sidecar to remove), C8 (re-implementation of the v1 outbox already in 5+ other connectors).

## 3. C1 install

| Step | Ref |
|---|---|
| Operator pre-installs MetaMod:Source 1.12 + SourceMod 1.13 (SM 1.12 crashes current TF2) | games/tf2/README.md:48-50 |
| Stop server, unzip into `tf/`: `plugins/takaro_tf2.smx`, `extensions/websocket.ext.*`, `json.ext.*` (32/64-bit), `cfg/sourcemod/takaro_tf2.cfg` | games/tf2/README.md:60-62, catalog/tf2/targets/linux-25995664.json:118-119 |
| Edit cfg: registration token + identity token (required) | games/tf2/README.md:63-74 |
| No launch args, no LD_PRELOAD/agent; must NOT use `-insecure` (ban needs SteamID) | games/tf2/README.md:76-77 |
| Writable state: `addons/sourcemod/data/takaro_tf2_bans.txt`, `cfg/banned_user.cfg` (via `writeid`) | `sp`:1857, `sp`:1021, catalog/tf2/targets/linux-25995664.json:126-130 |
| Restart: yes (stop/start) | games/tf2/README.md:60, :76 |
| Outbound WSS to `wss://connect.takaro.io` | games/tf2/mod/cfg/sourcemod/takaro_tf2.cfg:4 |

Classification: script plugin (`.smx`) plus two **arbitrary native SourceMod extensions** (`.so`/`.dll`), loaded by SourceMod's own extension loader (no OS-level preload). Host control needed: file upload into `tf/addons/` (including native binaries) and server restart. Hosts that allow only "SourceMod plugins" but block custom extensions would block it; no extra process needed.

## 4. C3 thread trace (source only)

All plugin code is SourcePawn and runs on the srcds main thread (SourceMod dispatches plugin callbacks, game events, timers and extension callbacks there). The extension's socket I/O thread is outside this repo.

| Path | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| getPlayers: `OnTakaroMessage` -> `HandleTakaroRequest` -> `HandleGetPlayers` (`sp`:418, :589-590, :657-672) | main: `IsClientInGame`, `GetClientName`, `GetClientAvgLatency` (`sp`:1505-1516, :1483-1500) | main, `JSONObject` (`sp`:666) | main enqueues `WriteJSON` to extension (`sp`:1202-1211, :1391-1398) | none | none |
| teleportPlayer (mutation): `HandleTeleportPlayer` (`sp`:1133-1150) | main: `TeleportEntity` synchronously in the message callback (`sp`:1147) | main | main `RespondNull` (`sp`:1149) | none | none |
| banPlayer (mutation w/ disk): `HandleBanPlayer` (`sp`:950-1002) | main: `ServerCommand banid` + `ServerExecute`, `KickClient` (`sp`:977-986) | main | main | main: `SaveBans` -> `KeyValues.ExportToFile` synchronously (`sp`:1000, :1839-1859) | main |
| player-connected: `OnClientPostAdminCheck` -> `EmitPlayerConnected` (`sp`:163-200) | main: client identity/name (`sp`:1522-1541) | main; serialized to string `data.ToString` (`sp`:1280) | main: `SendOutboxEntry` -> `WriteString` (`sp`:1303-1325) | none | `PrintToServer` main |
| chat-message: `say`/`say_team` command listener (`sp`:106-107, :240-269) | main | main | main | none | none |

No worker holds live game objects: requests are handled synchronously in the extension's message callback; outbox stores only strings (`sp`:1290-1292). Every request runs to completion on the main thread in the frame its message is dispatched; `executeConsoleCommand` runs `ServerCommandEx` inline (`sp`:729), `listItems` builds 318+ objects inline (`sp`:817-822).

## 5. C4 queues

| Queue | Capacity | Admission | Drain | Timeout | Shutdown | Ref |
|---|---|---|---|---|---|---|
| Inbound requests | none in plugin; each WS message handled synchronously in its callback | n/a (extension delivers one message per callback) | 1 message = 1 full handler run; no count/time budget per tick | none; no per-request timeout, no cancellation (Takaro-side timeout only) | n/a | `sp`:418-488, :570-628 |
| Event outbox (3 parallel `ArrayList`s) | `OUTBOX_MAX_EVENTS 512`, payload `OUTBOX_DATA_BYTES 4096` | over-size payload dropped with warning; when full, **oldest** entry dropped | sent immediately if identified; `FlushOutbox` resends all on identify | v1: no ack for `ACK_TIMEOUT_SECONDS 30.0` (checked by 5 s repeat timer) -> full reconnect | in-memory only, lost on `OnPluginEnd` (not persisted) | `sp`:19-21, :1277-1301, :1335-1345, :115, :1371-1380 |
| Outbound socket send | not bounded in plugin; `WriteJSON`/`WriteString` hand-off to extension; silently dropped when `!Connected` | none | extension | none | `Disconnect()` on unload | `sp`:1391-1398, :1303-1307, :134-140 |
| Ban commands | `ServerCommand` buffer + immediate `ServerExecute` | n/a | same frame | n/a | n/a | `sp`:977-978, :1019-1022 |

Timed-out jobs: no job objects exist, so a job cannot outlive caller stack data; a Takaro-side timeout on a slow main-thread request still lets the request finish and send a late response.

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation |
|---|---|---|---|---|
| Players online | engine (`IsClientInGame`) live each call | `g_PlayerSteamIds/Names/PlatformIds` by userid key, only for disconnect events | memory | removed in `OnClientDisconnect` (`sp`:225-227) |
| Last location | engine, read live (`GetClientAbsOrigin`) | none | none | n/a (`sp`:752-754) |
| Inventory | engine, live weapon slots 0-5 | none | none | n/a (`sp`:780-800) |
| Item catalog | generated static `.inc` (TF2 schema) + `g_ItemNames` map | memory, built at `OnPluginStart` | compiled-in | rebuild only by regenerating `.inc` (`sp`:805-815, `takaro_tf2_item_catalog.inc`) |
| Locations | live map entities; intel homes cached at round start | `g_FlagHome` | memory | reset `OnMapStart` (`sp`:153-157, :859-870) |
| Identities | engine `GetClientAuthId(SteamID64)`, fallback `SOURCE_<userid>` | `g_LastSeenNames` (SteamID64 -> name) | memory, grows unbounded for the process lifetime | never pruned (`sp`:52, :1535-1536) |
| Bans / timed bans | engine `banid` (in-memory) + connector ledger; permanent in-game bans from `banned_user.cfg` | `g_TakaroBan*` maps + `data/takaro_tf2_bans.txt` (KeyValues) | file; re-applied once per process in `OnConfigsExecuted` | expired entries dropped on load and filtered in `listBans` (`sp`:120-126, :1837-1905, :1051-1053, :1070-1097) |
| Event cursor | `g_StreamId` (per process: `tf2-<time>-<rand>`) + `g_NextSeq` | memory | none; new stream every plugin load | `sp`:64-65, :79 |
| Pending events / outbox | connector | 3 `ArrayList`s | memory only | v0 cleared after send; v1 cleared on cumulative ack (`sp`:1297-1299, :1364-1366) |

## 7. C6 delivery

- Outbox bound: 512 events, drop-oldest; >4096-byte event dropped (`sp`:1280-1288).
- Acks: v0 is fire-and-forget (removed right after `WriteString`, even if the socket is not connected: `SendOutboxEntry` returns early at `sp`:1305-1307 and the entry is still erased at `sp`:1297-1299). v1: cumulative `eventAck {streamId, upTo}`, mismatched streamId ignored (`sp`:1348-1368). WS-level `ping` -> `pong` is separate (`sp`:426-428, :556-568).
- Reconnect replay: v1 resends every unacked event with the same `seq` on identify (`sp`:450, :1335-1339); pre-identify events are buffered for both v0 and v1 (`sp`:1294). Reconnect also happens on every map change because `OnConfigsExecuted` calls `ConnectTakaro()` unconditionally (`sp`:120-128, :372-378).
- Lost on game crash / plugin reload: whole outbox and stream (no persistence); a new `streamId` starts at seq 1 (`sp`:79). Ban ledger survives (file).
- Connector errors feed the same outbox: every `SendErrorResponse`, WS close and WS error emits a `log` gameEvent (`sp`:405-416, :1217, :1236, :1382-1389), so a long outage with repeated reconnect errors can fill the 512 slots and evict real player events.
- Clock mix: `g_LastAckTime` is set with `GetGameTime()` on identify (`sp`:448) but compared against `GetEngineTime()` (`sp`:1317, :1367, :1374). Usually masked because `FlushOutbox` resets it via index 0 (`sp`:1316-1318), but it is a latent false/late ack-timeout.

## 8. C8 shared code

- Only SourcePawn connector in the repo; no CS2 or other Source/SourceMod connector exists (`ls games/`: 7d2d ark conan-exiles dragonwilds dune enshrouded minecraft rust scum terraria tf2 valheim vein zomboid). Nothing is copied byte-for-byte.
- Behaviour re-implemented (diverged by language, same semantics): v1 outbox + 30 s ack-timeout-then-reconnect also in games/vein/mod/src/native_bridge.cpp:242,665,1201 (cap 5000 / 31 MiB), games/dragonwilds/mod/src/native_bridge.cpp:136,728,1333 (cap 5000 / 31 MiB), games/scum/mod/TakaroSCUM/Scripts/takaro/protocol.lua:21,362, games/7d2d/mod/src/Services/OutboundLedger.cs, games/zomboid/.../TakaroWebSocketClient.java. TF2 cap (512) is an order of magnitude smaller and drop-oldest.
- Transport: third-party ProjectSky extension, pinned by SHA-256 (catalog/tf2/targets/linux-25995664.json:103-111); not shared with any other connector.

## 9. Existing tests and gaps

| Test | What it covers | Ref |
|---|---|---|
| `games/tf2/tests/validate-static.mjs` | Source-text asserts: v1 frame has `streamId`/`seq` (:100), `eventAck`/`HandleEventAck` present (:101), identify offers versions + manifest (:102), v1 error shape (:113-114), cvars incl. reconnect min/max (:64-65) | games/tf2/tests/validate-static.mjs:95-137 |
| `games/tf2/tests/live-harness.mjs` + `scripts/run-live-smoke.sh` | Live srcds vs fake Takaro: every non-destructive action shape, allowlist refusal, bot connect/death/disconnect events. Harness answers identify **without `protocolVersion`** so only v0 is exercised; it never sends `eventAck` | games/tf2/tests/live-harness.mjs:300, :347-349; games/tf2/scripts/run-live-smoke.sh |
| CI | `.github/workflows/tf2.yml` runs static checks + reproducible build only (no server on hosted runner) | catalog/tf2/targets/linux-25995664.json:131 |

Gaps: no behavioural test of v1 replay, ack trimming, ack timeout, outbox overflow (512 drop-oldest), >4 KiB event drop, map-change reconnect, Takaro outage, or ban ledger reload/expiry. No test that the extension verifies TLS certificates. No main-thread cost measurement for `listItems`/`listBans`.

## 10. Top risks

| # | Risk | Severity | Follow-up |
|---|---|---|---|
| 1 | v1 event delivery (replay, ack trim, 30 s ack timeout, overflow) is never exercised: harness negotiates v0 only and static checks are text matches; plus the GetGameTime/GetEngineTime mix at `sp`:448 vs :1374 | high | Teach live-harness to answer `protocolVersion: 1`, withhold/send `eventAck`, drop the socket, and assert resend with same seq; use `GetEngineTime()` at `sp`:448. |
| 2 | TLS certificate verification lives entirely in a third-party native extension that the repo neither configures nor tests | med | Bounded probe: point `takaro_tf2_ws_url` at a self-signed WSS endpoint and confirm the extension refuses; record result in the target notes. |
| 3 | Outbox is small (512) and shared with connector `log` events emitted on every WS error/close and error response, drop-oldest, memory-only; plus forced reconnect on every map change | med | Exclude connector-internal log events from the outbox (or cap them separately), and skip `ConnectTakaro()` in `OnConfigsExecuted` when already identified to the same URL. |
