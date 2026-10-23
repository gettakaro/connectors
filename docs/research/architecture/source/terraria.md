# Terraria (TShock): source-level analysis for #298

Source-only. HEAD 3859fd8. Paths relative to the repo root; `P` = `games/terraria/mod/TakaroTerrariaEvents/TakaroTerrariaEventsPlugin.cs`,
`B` = `games/terraria/bridge/src`. Claims marked **[upstream]** are about TShock/OTAPI internals that are not in this repo and
still need a decompile/runtime check. Claims marked **[image]** come from a read-only `docker create` + `docker export` of the
locally cached pinned image (no container started, nothing run).

## 1. Architecture on main

| Item | Value | Ref |
|---|---|---|
| Placement | **Two parts.** In-process TShock plugin (`TakaroTerrariaEvents.dll`) + out-of-process Node "bridge" sidecar | `games/terraria/DEVELOPMENT.md:8-24`, `catalog/terraria/game.json:56-64` |
| Plugin role | Emits `TAKARO_EVENT` log markers (death, NPC kill); registers admin commands `/takarotp /takaropos /takaroinv /takarogive /takaroban /takarounban /takaromap`; renders world map PNG | `P:33-72`, `P:1086-1090`, `games/terraria/mod/TakaroTerrariaEvents/MapRenderer.cs:14-40` |
| Bridge role | Outbound WSS to Takaro, drives TShock over REST (`/v2/server/status`, `/v2|v3/server/rawcmd`, bans), tails TShock log, polls players, serves map image, /health | `B/index.ts:42-161`, `B/tshock/client.ts:70-137` |
| Plugin runtime | net9.0, compiled in pinned `mcr.microsoft.com/dotnet/sdk:9.0.318-bookworm-slim` | `games/terraria/mod/TakaroTerrariaEvents/TakaroTerrariaEvents.csproj:3`, `catalog/terraria/targets/tshock-v6.2.1.json:40-45`, `games/terraria/Dockerfile.builder:4` |
| TShock runtime | TShock 6.2.1 / Terraria 1.4.5.8 / OTAPI 3.3.14; image `ghcr.io/pryaxis/tshock:6.2.1@sha256:70e5…` | `catalog/terraria/targets/tshock-v6.2.1.json:12,30-34` |
| .NET runtime in image | **[image]** `DOTNET_VERSION=9.0.20`, shared framework `/usr/share/dotnet/shared/Microsoft.NETCore.App/9.0.20/`; `TShockAPI.deps.json` `runtimeTarget .NETCoreApp,Version=v9.0`; entrypoint `./TShock.Server … -additionalplugins /plugins` | image config + `server/ServerPlugins/TShockAPI.deps.json` |
| Loader | TSAPI `TerrariaPlugin` + `[ApiVersion(2, 1)]`; loaded from `ServerPlugins/` (renter) or `/plugins` (image `-additionalplugins`) | `P:11-12`, `games/terraria/connector.json:16`, `catalog/terraria/targets/tshock-v6.2.1.json:71` |
| Bridge runtime | Node 22 (`node:22.23.2-bookworm-slim` pinned), ESM, deps shipped in zip | `catalog/terraria/targets/tshock-v6.2.1.json:63-67`, `games/terraria/bridge/package.json:1-22` |
| Transport lib | `ws` ^8.18.3 (lock: 8.21.3) | `games/terraria/bridge/package.json:13-15`, `games/terraria/bridge/package-lock.json:547-549` |
| TLS verification | `new WebSocket(url)` with no options; no `rejectUnauthorized`/`NODE_TLS*` anywhere in `B` → Node default (verify on). Default URL `wss://connect.takaro.io/` | `B/takaro/client.ts:73`, `B/config.ts:45` |
| Plugin→bridge channel | **None direct.** Bridge→plugin = REST rawcmd, reply parsed from command output (`TAKARO_POSITION/INVENTORY/GIVE/BAN` markers); plugin→bridge = TShock log file tailed | `B/terraria/adapter.ts:142-168,190-215`, `B/logs/logParser.ts:22,76-88` |
| Protocol | v1 offered (`protocolVersion=1` default) with v0 fallback | `B/config.ts:89`, `B/takaro/client.ts:55-58,233` |
| Default target | `tshock-v6.2.1` (`default: true`, `support.status: maintained`) — the only target | `catalog/terraria/targets/tshock-v6.2.1.json:3,7-9` |
| Connector version | `0.6.0` | `games/terraria/version.txt:1` |

**Extra-focus answers**
- **.NET:** TShock 6.2.1 runs on .NET 9 (image ships 9.0.20 shared runtime; plugin TFM net9.0). Repo pins only SDK 9.0.318 and the image digest; nothing in the repo records the runtime patch version.
- **`ClientWebSocket` in a plugin:** available without extra dependencies. **[image]** `Microsoft.NETCore.App/9.0.20/System.Net.WebSockets.Client.dll`, `System.Net.WebSockets.dll`, `System.Net.Security.dll` + `libSystem.Net.Security.Native.so`, `/usr/lib/x86_64-linux-gnu/libssl.so.3` and `/etc/ssl/certs/ca-certificates.crt` are present. The plugin already uses BCL-only APIs (`System.Text.Json`, `System.Threading.Timer`, `P:4`, `P:106`), and `CopyLocalLockFileAssemblies=false` (`.csproj:9`) means it ships a single DLL; `ClientWebSocket` keeps that property. Default `ClientWebSocket` validates certificates via OpenSSL + system CA store. Gap: renter hosts using the upstream *release zip* (not the image) need the zip's runtime layout checked (self-contained vs framework-dependent) — not verifiable from repo; TER-1 must check it on Linux and Windows.

## 2. Drift vs the issue (Terraria section, written against 147acb21)

| Issue hypothesis (quoted) | Status at HEAD |
|---|---|
| "can the existing plugin become the first practical native example while preserving its actual supported behavior?" | **Still open, more plausible.** Plugin already owns 6 of 8 game-touching paths (position, inventory, give, teleport, ban/unban, death/kill events, map); bridge-only logic is REST status, broadcast/kick/rawcmd, chat/join/leave log parsing, item catalog, config, WSS. No native attempt exists. |
| TER-1 "Confirm the exact Terraria/TShock/.NET pairing … outbound WSS" | **Partly answered from source/image:** 6.2.1 / 1.4.5.8 / OTAPI 3.3.14 / .NET 9.0.20 (`catalog/terraria/targets/tshock-v6.2.1.json:12`). Outbound WSS from plugin untested. Ordinary-renter path still unproven. |
| TER-2 "Trace REST helper commands and TShock hooks for thread ownership" | **Still true and important** — see §4: REST-invoked plugin commands mutate game state on a non-game thread **[upstream]**; nothing in repo schedules onto the main thread. |
| TER-3 "log rotation, bridge-only restart, game restart, … Do not assume the current log cursor is durable" | **Still true.** Rotation following was already in 147acb21 (`git show 147acb21:games/terraria/bridge/src/logs/logTailer.ts:26`); cursor is still in-memory only (`B/logs/logTailer.ts:29,115`). **Changed:** v1 numbered/acked event buffer added after 147acb21 (`B/takaro/client.ts:7,192-213`; absent at 147acb21), so Takaro-side outages now replay within one bridge process. |
| TER-4 "Assess memory snapshots first; retain TShock's authoritative bans" | **Still true.** Bans stay in TShock's ban store via plugin `/takaroban` (`P:196-276`) and REST list (`B/tshock/client.ts:124-128`). No connector-owned durable state except `TakaroConfig.txt` identity and map files. |
| Starting evidence "existing bridge contract/poller tests" | **Grown:** + `protocolV1.test.ts`, `presenceGate.test.ts`, `reconfigure.test.ts`, `configFile.test.ts`, `identity.test.ts` (none at 147acb21). Still **no plugin (C#) tests**. |
| (implicit) map not part of the scope | **Changed:** plugin now renders a map on a background thread (`MapRenderer.cs:14-40`, #483) — new off-thread game read not covered by the issue. |

## 3. C1 install (ordinary renter)

Steps from `games/terraria/README.md:13-109`:
1. Copy `TakaroTerrariaEvents.dll` → `<server>/ServerPlugins/` (`README.md:72`). Managed TSAPI plugin DLL — normal TShock workflow.
2. Enable TShock REST: `RestApiEnabled: true`, `RestApiPort: 7878` in `tshock/config.json` (`README.md:84-90`); create an application REST token for a user with `takaro.admin` (+ `tshock.broadcast`) (`README.md:91-97`).
3. Extract bridge zip to `<server>/TakaroTerrariaBridge/` (`README.md:76-80`); **Node.js 22 on the same host** (`README.md:19`, `games/terraria/connector.json:7`).
4. Paste registration token + REST token into `TakaroConfig.txt` (created on first start, mode 0600) (`README.md:105-109`, `games/terraria/DEVELOPMENT.md:156-162`).
5. Restart TShock once (plugin load); start bridge with `npm start` and keep it running (`README.md:100-103`).
6. Writable state: `TakaroConfig.txt` (identity written back), `tshock/takaro/` map dir (`MapRenderer.cs:27`), TShock logs readable by bridge (`logFiles=tshock/logs`, `B/configFile.ts:50`).
7. Network: outbound WSS from bridge; loopback HTTP 7878 (REST) and 3020 (/health, bound `127.0.0.1`, `B/health/server.ts:33`).

| Classification | |
|---|---|
| Plugin | managed plugin DLL (no native lib, no launch args) |
| Bridge | **extra long-running process** (Node 22) + REST credential management |
| Host control needed | shell / process supervisor for a second process, Node install, REST enabled. A panel-only renter (Shockbyte/GPortal: file manager + TShock) can do step 1–2 but usually **not** 3/5. A plugin-only connector would reduce this to "drop DLL + edit one config file + restart". |

## 4. C3 thread trace (source-only)

| Path | Game access | JSON build | Network send | Disk write | Logging |
|---|---|---|---|---|---|
| **getPlayers** (also 10 s poller) | TShock REST `/v2/server/status?players=true` handler thread **[upstream: TShock `Rest`/HttpServer worker reading `TShock.Players`]** (`B/tshock/client.ts:70-77`) | TShock (REST) + bridge Node main thread | bridge Node main (`B/takaro/client.ts:169-172`) | none | bridge stdout; TShock logs every REST call (why `logExcludePatterns` exists, `games/terraria/DEVELOPMENT.md:304-309`) |
| **giveItem** | `/v2|v3/server/rawcmd cmd=/takarogive …` → `TakaroGive` → `ApplyPlacementPlan` writes `TPlayer.inventory[...]` and `NetMessage.SendData` (`B/terraria/adapter.ts:197`, `P:439`, `P:673-708`) on the **REST request thread [upstream: RestManager rawcmd → `Commands.HandleCommand` synchronously]** — not the game loop | plugin on REST thread (`P:711-720` / success marker) | REST reply → bridge → WSS on Node main | none | `TShock.Log` on REST thread |
| **teleportPlayer** | same route, `TSPlayer.Teleport` (`P:124-157`) on REST thread **[upstream]** | — | as above | none | — |
| **getPlayerLocation / inventory** | `/takaropos` reads `TPlayer.position` (`P:159-185`); `/takaroinv` iterates 13 item arrays (`P:368-412`) on REST thread **[upstream]** | plugin, `JsonSerializer` on REST thread | as above | none | — |
| **player-death** | `GetDataHandlers.KillMe` (`P:35,774-789`) on the packet-processing thread **[upstream: game loop]** | plugin, same thread (`P:1086-1089`) | — | **synchronous log write on that thread** via `TShock.Log.ConsoleInfo` (`P:1089`) | same |
| ↳ bridge | — | Node main (`B/logs/logTailer.ts:128-150`, 1 s poll `:44`) | Node main (`B/index.ts:64-69`) | — | — |
| **chat-message** | none from plugin; TShock writes `Broadcast: <name> msg` to its log **[upstream]** | bridge parses (`B/logs/logParser.ts:7,51-56`) | Node main | TShock log | — |
| **player-connected** | REST poll diff (`B/events/playerPoller.ts:275-287`) + log `has joined.` (`B/logs/logParser.ts:9,63-66`), de-duplicated by `PresenceGate` (`B/events/presenceGate.ts:14-26`) | Node main | Node main | — | — |
| **map render** | `Main.tile` read on dedicated `Thread` BelowNormal (`MapRenderer.cs:14-15,35-38`), started by `System.Threading.Timer` (thread pool) or command (`P:106,117-122`) | same thread | — | PNG + `map.json` atomic write in `tshock/takaro` (`MapRenderer.cs:95-123,170-174`) | `TShock.Log` |

**Workers holding live game objects:** (a) REST thread holds `TSPlayer`/`TPlayer`/`Item[]` references and *writes* inventory + sends packets while the main loop runs (`P:673-708`) — a real cross-thread mutation if the [upstream] REST-thread claim holds; (b) map thread reads `Main.tile` concurrently by design (read-only, `MapRenderer.cs:14-15`). No connector code marshals anything onto the game thread; there is no scheduler.

## 5. C4 queues

| Queue / channel | Capacity | Admission | Drain | Timeout | Shutdown |
|---|---|---|---|---|---|
| Takaro request → handler | **unbounded**; each WS `request` spawns a detached promise (`B/index.ts:96-98`) | none | concurrent, no limit | none at bridge level | pending promises dropped at `process.exit` (`B/index.ts:229-240`) |
| Bridge → TShock REST (`fetch`) | unbounded concurrent GETs | none | TShock HTTP server | `tshockTimeoutMs` default **5000** via `AbortController` (`B/config.ts:79`, `B/tshock/client.ts:155-166`). **A timed-out rawcmd still runs in TShock** (abort only cancels the client read); give/ban are not idempotent → caller sees error, item may still be delivered | — |
| REST → game state | none — runs inline on REST thread **[upstream]** | — | no per-tick budget | — | — |
| v1 event outbox | `EVENT_BUFFER_LIMIT = 2000` (`B/takaro/client.ts:7`) | always push; drop **oldest** on overflow, warn at 1st and every 100th (`:192-200`) | sent immediately if socket open; resent in full after identify (`:234-236`) | **no ack timeout** (no reconnect-on-stale-ack, unlike TF2) | lost on exit (memory) |
| v0 events | none | `send` returns false when socket down → logged "dropped" (`B/index.ts:67-68`, `B/takaro/client.ts:169-172,193`) | — | — | — |
| `ws.send` | `ws` internal buffer, **unbounded**, no `bufferedAmount` check (`B/takaro/client.ts:171`) | — | — | — | — |
| Log tailer | reads *whole* delta into one `Buffer.alloc(size - offset)` (`B/logs/logTailer.ts:134`), 1 s interval, overlap guarded (`:79-80`) | — | all lines per tick | — | stops on WS disconnect, offset kept (`B/index.ts:150-154`) |
| Player poller | 1 in-flight (`B/events/playerPoller.ts:276`), `pollIntervalMs` default **10000** (`B/config.ts:82`) | — | — | REST 5 s | — |
| Map render | single thread, `lock(Gate)` start-if-idle (`MapRenderer.cs:30-40`); bridge polls dir every 30 s (`B/index.ts:19,197-213`) | reject if running | — | — | `_mapTimer.Dispose()` only (`P:88`); running thread not joined |
| Reconnect | backoff 3 s ×2ⁿ cap 60 s (`B/takaro/client.ts:38-39,270-279`) | — | — | — | `shutdown()` cancels (`:122-128`) |

## 6. C5 state inventory

| State | Authority | Connector store | Persistence | Invalidation on restart/world change |
|---|---|---|---|---|
| Players online | TShock (`TShock.Players`) via REST | `PlayerPoller.previous` Map (`B/events/playerPoller.ts:9`), `adapter.players` name index (`B/terraria/adapter.ts:29,124-133`), `PresenceGate.online` (`B/events/presenceGate.ts:12`) | memory | cleared on every identify (`B/index.ts:145-147`) → all online players re-emitted as connected; leaves during WS outage not emitted |
| Last location | game (`TPlayer.position`) | none (read on demand, `P:178`) | — | — |
| Inventory | game (`TPlayer.*` arrays) | none (`P:368-412`) | — | — |
| Item catalog | static generated file (6147 entries) | `B/terraria/itemCatalog.ts` | in bridge build | **not** tied to the running server: a TShock update with new items needs a bridge rebuild (`games/terraria/DEVELOPMENT.md:319-332`) |
| Identities | character name → `terraria:<name>` (`B/terraria/identity.ts:20-31`) | derived, stateless | — | name change = new player |
| Server identity | `identityToken` in `TakaroConfig.txt` | file, written back atomically (`games/terraria/DEVELOPMENT.md:159-162`) | file | survives upgrade |
| Bans / timed bans | TShock ban DB; `[takaro:<name>]` tag in reason (`games/terraria/DEVELOPMENT.md:364-379`, `P:196-276`) | none | TShock | no connector-side timed-ban expiry visible in plugin |
| Event cursor (log) | TShock log file offset | `LogTailer.active {path,inode,offset}` (`B/logs/logTailer.ts:13-18,29`) | **memory only**; bridge restart → `startAtEnd=true` skips backlog (`:45,111-116`) | rotation → new file from 0 (`:119-125`) |
| Pending events / outbox | bridge | v1 buffer ≤2000 (`B/takaro/client.ts:32`) | memory; `streamId` new per process (`:30`) | lost on bridge restart |
| Map image | plugin renders | `tshock/takaro/map-*.png` + `map.json`; bridge `MapImageStore` | file | re-render at world load (`P:99-100`) |
| Config | `TakaroConfig.txt` | file, hot-reloaded every 5 s (`B/index.ts:18,215-227`) | file | — |

## 7. C6 delivery

| Scenario | Behaviour | Ref |
|---|---|---|
| Takaro WS drop (bridge + game up) | poller and tailers **stop**; tail offset kept; on re-identify tailers resume from offset → log events (chat/death/kill) during outage are **delivered late** (v0 and v1). v1 also resends unacked buffer. Joins re-emitted for everyone online; **leaves during outage lost** (poller reset). | `B/index.ts:142-154`, `B/takaro/client.ts:234-236` |
| Ack semantics | v1: `eventAck {streamId, upTo}` trims buffer (`B/takaro/client.ts:209-213`); v0: fire-and-forget; `ping`→`pong` only (`:259-261`). No stale-ack watchdog. | |
| Overflow | oldest dropped beyond 2000 (`B/takaro/client.ts:196-200`); v0: dropped when socket down (`B/index.ts:68`) — only for poller events since tailers are stopped while down | |
| Bridge restart | new `streamId`, empty buffer, tail starts at EOF → **everything between bridge stop and start is lost** (not replayed from log) | `B/takaro/client.ts:30`, `B/logs/logTailer.ts:111-116` |
| Game crash/restart | events already written to the log are read; TShock opens a new log, read from byte 0 (`B/logs/logTailer.ts:119-125`); REST down → poller keeps last `previous` (no false leaves) then diffs (`B/events/playerPoller.ts:291-299`) | |
| Game crash before log flush | lost — plugin has no outbox, log write is its only delivery (`P:1086-1090`) | |
| Log text spoofing | chat parsed by regex from log (`B/logs/logParser.ts:7`); a `TAKARO_EVENT {…}` inside any log line is accepted anywhere in the line (`B/logs/logParser.ts:76-80`, `indexOf`) — a player chat line containing that marker could inject an event **(source-level concern, unverified)** | |

## 8. C8 shared code

| Code | Siblings | Diverged? |
|---|---|---|
| `B/takaro/client.ts` (`TakaroWsClient`, v1 buffer) | `games/dune/sidecar/src/takaro/client.ts`, `games/enshrouded/sidecar/src/takaro/client.ts`, `games/vein/sidecar/src/takaro/client.ts` | same class name and concept, **independently written**: `diff` line deltas 597 (dune), 297 (enshrouded), 437 (vein). Terraria has `EVENT_BUFFER_LIMIT = 2000`; Dune numbers by `sendId` (`games/dune/sidecar/src/takaro/client.ts:71,300`). |
| Target resolution | `scripts/lib/target.sh` (shared, all games) via `games/terraria/scripts/lib-target.sh:10-24` | shared, not copied |
| Log tailer / poller / presence gate / health | no sibling copies under `games/*/sidecar/src` with the same paths | Terraria-specific |
| Plugin (C#) | only TSAPI plugin in repo | — |

Native port would need a .NET Takaro wire client; candidates to compare (not checked here): other managed-runtime connectors (Valheim BepInEx, 7D2D, Conan/Dragonwilds/VEIN/Enshrouded native runners are C++). No shared .NET client exists today.

## 9. Existing tests for C4/C6 and gaps

| Test | Covers |
|---|---|
| `B/__tests__/protocolV1.test.ts:94` "version 1: identify offers [0, 1] … events carry seq and are resent until acknowledged" | v1 replay/ack |
| `B/__tests__/protocolV1.test.ts:176` v0 fallback | v0 |
| `B/__tests__/logTailer.test.ts:57,80,105,144` rotation, no re-emit, truncation, deleted file | cursor/rotation |
| `B/__tests__/playerPoller.test.ts:56,84,102,133` failed poll no false leaves; outage diff; reset re-emits | C6 presence |
| `B/__tests__/reconfigure.test.ts:70,91,128` backoff replacement, late close, shutdown wins | reconnect |
| `B/__tests__/presenceGate.test.ts:9` dedupe | |
| `B/__tests__/tshockClient.test.ts:151` failure-text detection | |
| `maintenance/src/takaro_maint/games/terraria/verify.py` (`handshake`, `reconnect` checks; `games/terraria/DEVELOPMENT.md:199-214`) | runtime reconnect (not in CI, `runtime: false`, `:193-195`) |

**Gaps:** no test for 2000-overflow drop path; no REST timeout-but-executed test (duplicate give); no bridge-restart loss test; no test that a WS outage loses leaves; no plugin unit tests at all; no thread-ownership assertion; `TAKARO_EVENT` injection via chat untested; runtime CI disabled.

## 10. Top risks

| # | Risk | Sev | Follow-up |
|---|---|---|---|
| 1 | Plugin mutates inventory / teleports / reads `TPlayer` on the TShock REST thread (if [upstream] holds), racing the game loop; no main-thread scheduler. | **high** | Decompile `TShockAPI.dll` RestManager rawcmd to confirm; if confirmed, marshal `/takarogive`/`/takarotp` onto the game update hook (`ServerApi.Hooks.GameUpdate`) with a bounded queue — required anyway for a native connector. |
| 2 | REST timeout (5 s) does not cancel the command: a slow give is reported failed to Takaro yet delivered → double delivery on retry; unbounded concurrent requests. | **med** | Add request id/idempotency in `/takarogive` and a bounded in-flight limit; test "timed out but executed". |
| 3 | Delivery durability rests on in-memory log cursor + memory outbox: bridge restart drops all events since stop; WS outage drops leaves; log-marker transport is spoofable by chat text. | **med** | Native plugin with in-process outbox fed directly by hooks (`KillMe`, `NpcKilled`, `ServerChat`, `ServerJoin/Leave`) removes the log cursor; until then anchor `TAKARO_EVENT` parsing to TShock's log prefix and persist the tail offset. |

**Sidecar-removal assessment (source-only, C7 inputs):** loader/installability — met (managed DLL, renter-standard). Secure transport — `ClientWebSocket` + OpenSSL + CA bundle present in the .NET 9.0.20 runtime **[image]**; renter-zip layout unverified. Safe game access — needs a GameUpdate-hook scheduler (none today). A plugin-only connector must keep: WSS identify/reconnect/backoff, v1 seq/ack buffer (≥1000), Takaro request dispatch with bounded queue and timeouts, players list/join/leave (TSAPI `ServerJoin`/`ServerLeave` hooks replace polling + presence gate), chat (`ServerChat` hook replaces log regex), death/kill (already hooks), give/teleport/pos/inv/ban (already in plugin), broadcast/kick/rawcmd allowlist (in-process `Commands.HandleCommand` with `TSPlayer.Server`), listBans (TShock ban manager), item catalog (can come from `ItemID`/`Lang` at runtime — fixes the derived-names issue), config file + identity write-back, map serving (chunks from `tshock/takaro`), `/health` equivalent optional. It can drop: REST token/permissions, log tailer, cursor, `logExcludePatterns`, Node runtime. Recommendation input: **conditional/proceed** pending TER-2 thread confirmation and a bounded WSS probe.
