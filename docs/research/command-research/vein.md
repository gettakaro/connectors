# VEIN: native and silent Takaro commands (research)

Refs gettakaro/connectors#204 (parent #195). Research and a bounded probe only: no production
command routing ships from this work. The probe is compiled into the plugin but stays inert unless
`TAKARO_CHAT_PROBE=1` is set.

## Decision

**Interception: go.** Choose interception of the inbound `Server_Say` RPC, not registration.

| Question | Result | Go / no-go | Confidence |
| --- | --- | --- | --- |
| Q1 Which functions carry chat, in which order, with which parameters? | passed | go | discovery: **high** |
| Q2 Does controller → PlayerState → SteamID64 resolve on the live inbound call, after a reconnect and after a server restart? | passed | go | lifecycle: **medium-high** |
| Q3 Can one owned trigger be consumed before the public broadcast, with exactly one stub envelope, while other chat keeps VEIN's behaviour? | passed | go | execution + suppression: **high** for the RPC path |
| Q4 With the backend unavailable, does an owned trigger fail privately (no public fallback, no native fallthrough, no queue, no replay)? | passed | go | suppression: **high** |
| Q5 Does the ProcessEvent path stay bounded, with no network, JSON or catalog work on the game thread? | passed | go | measured, see below |
| Q6 Can vanilla autocomplete discover a server-only command? | static evidence only (not experimented) | no-go for autocomplete | autocomplete: **medium** that it is client-only |
| Q7 Native registration (fabricating `UChatCommand` classes) | not attempted, by design | no-go | registration: **low** |
| Q8 Envelope contract: fanout and dedupe | design below | proposal | n/a |

Confidence per issue dimension: **discovery high, execution high, suppression high, lifecycle
medium-high** (reconnect and restart passed; offline/rapid-reconnect races and long-run performance
under load are not yet exercised). Production work should start from the follow-up issues at the end.

## What was tested

| | |
| --- | --- |
| Date | 2026-10-22, 16:41Z to 17:18Z |
| Game server | VEIN Linux dedicated server, Steam app 2131400, **Steam build 25969514**, game version **0.025h3** (`gameVersion` in `/health`), Unreal Engine **5.6.1** (`LogInit: Engine Version: 5.6.1-0+UE5`), binary `VeinServer-Linux-Test` |
| Client | One vanilla Windows VEIN client (Steam app 1857950, `Vein-Win64-Test`, window title `VEIN 64 PCD3D_SM6 Test`), server browser shows **0.025h3** |
| Connector | vein **0.7.0** (`games/vein/version.txt`), origin/main `3859fd8`, plus probe commit `45fd95b` (live runs; library sha256 `c579abd55855356a7f14f6c0b2e257855e157599042de41169ebc68f4fb3fb4f`). A later commit on this branch hardens edge paths found in review (unidentified sender, full queue, worker start failure, notice delivery, oversized messages); those paths are covered by unit tests only and were not re-run live |
| Takaro | staging, one GENERIC game server (protocol v1) |

Second run on the catalog default target (`linux-25708939`): server Steam build **25708939** and client
build 25708540, both from VEIN's `0.025h2` Steam rollback branch (`gameVersion 0.025h2`), with the
final probe library (commit `0f68b27`, sha256 `2e04c672…`). It repeated Q1 and Q3 in Local and Global,
plus the regression smoke: identical RPC signatures and order, and every owned trigger consumed with
one envelope and no broadcast, log line or Takaro event. Q2 restart and Q4 were run on 0.025h3 only.

Rescope against the issue text: the issue was written for build **25035268 / v0.024h8** and predicts
`Server_Say(FString, EChatSegment, TSubclassOf<UChatCommand>)`. On 0.025h3 the RPCs carry **no
`EChatSegment` parameter** (Q1). The connector is already the native in-process generation the issue
describes (no sidecar), so no architecture rescope was needed. Conclusions below apply to 0.025h2 and 0.025h3;
rerun the probe after every game update.

## How the probe works

`games/vein/mod/src/chat_probe.{h,cpp}`, wired into the existing ProcessEvent detour in
`games/vein/mod/src/events.cpp` (`ProbeChat`):

- `TAKARO_CHAT_PROBE=1` enables it (read once at boot). Unset, the detour behaves as in 0.7.0: one
  relaxed atomic load on a chat RPC hit, nothing else.
- Every chat RPC the detour sees is traced into a 128-entry ring: function, receiver class, message,
  the `UChatCommand` class argument (null / class name / unreadable), sender SteamID64 and name, and
  whether it was consumed.
- An inbound `Server_Say` whose first word is exactly `!tkprobe204` or `/tkprobe204` (hard-coded,
  case-sensitive, whole word) is **consumed**: the detour returns without calling VEIN's original
  function, so `Server_Say_Implementation` never runs and VEIN neither logs nor broadcasts it. The
  detour builds one envelope (`dedupeId = <bootId>:<seq>`, SteamID64, name, trigger, raw args) and
  pushes it onto a bounded queue (32). A probe worker thread (the "stub") takes it. Nothing is sent to
  Takaro and no `chat-message` event is emitted.
- A trigger from a sender whose SteamID64 cannot be resolved is still consumed (never broadcast) but
  creates no envelope (`consumedWithoutIdentity`). A full queue drops the item and counts it
  (`queueDropped`); if the worker thread cannot start, the probe disables itself.
- `TAKARO_CHAT_PROBE_STUB=down` simulates an unavailable backend: the trigger is still consumed, no
  envelope is created or queued, and the worker sends the sender a private notification
  (`AVeinPlayerController::Client_SendNotification`, through the existing game-thread job queue).
- `GET /debug/chat-probe` (debug-gated loopback diagnostics) returns the traces, envelopes and
  counters. Unit tests: `TestChatProbeMatch`, `TestChatProbeDecision`, `TestChatProbeStub` in `mod/tests/unit_test.cpp`.

The test server was switched with a compose override that only adds `TAKARO_CHAT_PROBE=1` (and
`TAKARO_CHAT_PROBE_STUB`). Messages were typed in the real client's chat box; raw evidence for every
row is listed in the evidence table.

## Q1: inbound and outbound functions, order and parameters: passed

Parameter lists as the live UFunctions report them (plugin log, `chat RPC params`):

- Inbound, client → server: `AVeinPlayerController::Server_Say(Message: FString @0, SelectedChatType: TSubclassOf<UChatCommand> @16)`,
  received on `BP_VeinPlayerController_C`.
- Outbound, server → clients: `AVeinGameStateBase::NetMulticast_SendChat(Speaker: Object @0, Message: FString @8, CommandClass: TSubclassOf<UChatCommand> @24, PlayerLocation: Struct @32)`,
  on `BP_VeinGameStateBase_C`.
- Order: `Server_Say` always first; `NetMulticast_SendChat` follows inside the original call, about
  0.1 to 0.16 ms later (trace `monoNs`). VEIN then writes `LogVeinChat: <Segment> | [SteamID64] …`.
- The segment is **not** a parameter any more. It follows from the command class the client chose.

| Case | Typed in client | `Server_Say` message | Command class | Multicast | `LogVeinChat` | Takaro `chat-message` |
| --- | --- | --- | --- | --- | --- | --- |
| A1 normal, Local | `tk204 normal local A1` | same | `CC_Local_C` | yes | `Local` | yes |
| A2 unknown slash, Local | `/tk204unknown A2` | same | `CC_Local_C` | yes | `Local` | yes |
| A3 native `/me`, Local | `/me tk204 waves A3` | `tk204 waves A3` (prefix stripped by the client) | `CC_Me_C` | yes | `Me` | yes |
| B1 normal, Global | `tk204 normal global B1` | same | `CC_Global_C` | yes | `Global` | yes |
| B2 unknown slash, Global | `/tk204unknown B2` | same | `CC_Global_C` | yes | `Global` | yes |
| B3 native `/me`, Global | `/me tk204 nods B3` | `tk204 nods B3` | `CC_Me_C` | yes | `Me` | yes |

The `UChatCommand` class was resolved in every case, never null. Unknown slash text is not
intercepted by the client: it reaches the server verbatim under the default command of the selected
channel. Native prefixes (`/me`, and by the same mechanism the others below) are parsed on the
client, which sends only the remaining text plus the command class.

## Q2: identity on the live inbound call: passed

`PlayerStateOf(controller)` → `IdentFromPlayerState` gave the test player's SteamID64 for every
inbound call, the same value each time:

- first join (A1 to B4);
- after **exit to main menu and rejoin** (new controller object; R1, R2);
- after a **server restart** (new process, new boot id; S1, S2).

Takaro recorded one `player-connected` per join (five joins), all against the same Takaro player.

## Q3: consume one owned trigger before the broadcast: passed

| Case | Typed | Trace | Multicast | `LogVeinChat` | Sender's own chat box | Takaro `chat-message` | Envelope |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A4 Local | `!tkprobe204 arg1 A4` | `Server_Say`, `consumed:true` | **none** | **none** | not shown | **none** | `682bfa939b4b523d:1`, args `arg1 A4` |
| A5 Local | `/tkprobe204 two words A5` | `consumed:true` | none | none | not shown | none | `…:2`, args `two words A5` |
| B4 Global | `!tkprobe204 argg B4` | `consumed:true` | none | none | not shown | none | `…:3`, args `argg B4` |
| R1 after rejoin | `!tkprobe204 after-reconnect R1` | `consumed:true` | none | none | not shown | none | `…:4` |

Each consumed call produced exactly one envelope (`envelopes` = `stubReceived` = `consumed`). Normal,
unknown and native messages sent around them kept VEIN's behaviour (the A/B rows above, R2, S2). The
client has no local echo: the sender's own chat box shows A1 to A3 and not A4. So the multicast is the
only render path, and skipping it hides the trigger from every client. A second observer client was
not available. The server-side proof is that `NetMulticast_SendChat` never ran for a consumed call,
and that multicast is the only server-to-client chat path.

A consumed trigger produces no `chat-message` event, so chat hooks and the Discord chat bridge
cannot see it. Discord itself was not read back in this run.

Control: with the probe **off** (default), the same trigger `!tkprobe204 probe-off D2` was broadcast
and arrived in Takaro as ordinary chat.

## Q4: backend unavailable fails privately: passed

Server restarted with `TAKARO_CHAT_PROBE_STUB=down`:

- `!tkprobe204 stub-down S1`: `consumed:true`, no multicast, no `LogVeinChat`, no Takaro event, **no
  envelope**, `failedPrivate:1`. The sender got a private top-centre notification: `[Takaro] Takaro
  command unavailable (probe stub is down). Nothing was sent.` (screenshot).
- No native fallthrough: VEIN's original `Server_Say` was not called.
- No queue and no replay: after an exit-to-menu and rejoin the counters were unchanged (`consumed:1,
  envelopes:0, privateNotices:1`). After a server restart the counters start at zero: envelopes are
  memory-only and never persisted, so nothing can be replayed.

## Q5: bounded game-thread work and performance: passed

The probe adds to the game thread only: one reflection walk over the RPC's parameter list, a
whole-word compare, the identity read the chat path already does, a class-name read, a bounded
ring/queue push and one log line. Envelope JSON is built only by the HTTP diagnostics thread.
The stub and private-notice work runs on the probe worker. No network, JSON or catalog work happens
on the game thread.

| Measurement (`/debug/perf`, same client, same server) | probe off | probe on |
| --- | --- | --- |
| Chat event handler, 10 RPCs (5 messages × in+out) | avg **43.8 µs**, max 75.3 µs | avg **64.9 µs**, max 106.4 µs |
| ProcessEvent filter, all RPCs | avg 0.54 µs | avg 0.51 µs |
| Tick p50 / p99 | 1.68 / 35.5 µs | 1.77 / 27.2 µs |

So the probe costs about 21 µs per chat RPC and nothing on non-chat RPCs. A production router should
avoid the second parameter walk by reusing `ReadChatParams` (about half of that cost).

## Q6: native commands, collisions and autocomplete: static evidence

Native commands the vanilla client lists when `/` is typed (screenshot): `/a` (admins), `/g`
(global), `/it`, `/l` (local, default), `/me`, `/pm "Player Name"`, `/r` (radio), `/w` (whisper
range), `/y` (yell). Observed classes: `CC_Local_C`, `CC_Global_C`, `CC_Me_C`.

From the depot DWARF (`VeinServer-Linux-Test.debug`, build 25969514):

- `UChatCommand : UPrimaryDataAsset` holds `CommandPrefixes: TArray<FString>`, `bShowInList`,
  `PrivilegeToUse` / `PrivilegeToReceive` (`EChatCommandPrivilege {Anyone, Admin, SuperAdmin,
  Nobody}`), `bSendToDiscord`, `ChatSegments: TSet<EChatSegment>`, `bParseParameters`,
  `RequiredParameters`, `TotalParameters`, `PlayerNameParameters`, `MessageFormat`, `Name`,
  `Description`, `Radius`. Its virtual hooks are `CanUseCommand`, `GetMessageReceivers`,
  `GetChatMessage`, `ParseMessageParameters`, `OnUse`, `UserHasPrivilege`, `SearchPlayerByName`.
  Native C++ subclasses: `URadioChatCommand`, `UDirectMessageChatCommand`. The `CC_*_C` commands
  are cooked Blueprint assets.
- `FChatCommandList { TArray<TSubclassOf<UChatCommand>> }` and the chatbox widget functions
  `RefreshChatCommands`, `GetChatCommandListVisibility`, `GetActiveChatCommandVisibility` build the
  list and prefix matching **on the client**. `UChatboxUserWidget` itself holds `ChatCommandList`,
  `ActiveChatCommand`, `SelectedChatType: TSubclassOf<UChatCommand>`,
  `LastTypedCommand: TSoftObjectPtr<UChatCommand>` and `bShowCommandList`.
- `AVeinGameStateBase` holds no command list: only `DefaultChatCommand: TSubclassOf<UChatCommand>`
  and an `OnChat` dynamic multicast delegate. So the server has no replicated list it could extend.

Collision behaviour: a Takaro trigger that equals a native prefix (`/me`, `/g`, …) never reaches the
server with its prefix, because the client strips it and sends the native class instead. Such
triggers can only be served by inspecting `SelectedChatType`, which would take a native command away
from players. **Recommendation: Takaro triggers on VEIN must not collide with the native prefixes
listed above**. Report collisions at catalog sync and use a non-colliding prefix (for example `!` or
an unused `/word`).

Autocomplete: a server-only `UChatCommand` subclass has no cooked asset on a vanilla client, and the
list is assembled by the client widget from classes it already has. No server-only path was found
that adds an entry. This was **not** tried live (Q7), so the claim is "no verified path", not
"impossible".

## Q7: native registration: not attempted

Fabricating `UChatCommand` classes at runtime would need matching cooked classes on every client.
The issue's own guidance is to prefer interception, and Q3 succeeded on the predicted ingress. So
registration was not tried. Leave it open only together with an official mod or client companion.

## Q8: proposed invocation envelope (not implemented)

```json
{
  "type": "command-invocation",
  "invocationId": "<bootId>:<seq>",
  "gameServer": "<authenticated by the connector connection>",
  "player": { "gameId": "<SteamID64>", "platformId": "steam:<SteamID64>", "name": "<persona>" },
  "trigger": "!tp",
  "rawInput": "!tp home",
  "rawArgs": "home",
  "chat": { "commandClass": "CC_Local_C", "segment": "local" },
  "observedAt": "<ISO-8601>"
}
```

- **Fanout stays in Takaro.** The connector sends one envelope per consumed message and never
  chooses a Command or ModuleInstallation. Takaro resolves every matching Command across
  installations, as it does for chat today.
- **Dedupe ownership.** The connector owns `invocationId` uniqueness: `bootId` changes on every
  process start and `seq` is monotonic within a boot. Takaro owns idempotency: it should ignore a
  repeated `invocationId` for the same game server. The transport gives no exactly-once guarantee.
- **No replay.** An invocation not acknowledged before a disconnect is dropped with a private
  "unavailable, try again" notice. It is never queued in the durable event outbox (see Q4).

## Evidence

Raw evidence was kept with the test run (not committed); file names for reference:

| Row | Evidence |
| --- | --- |
| `check:rescope` | `00-baseline-health-0.7.0.json` (`gameVersion 0.025h3`), plugin log param dump in `25-plugin-log-chat-run1.txt` |
| `feature:inbound-outbound-order` | `21-probe-after-run1.json`, `24-vein-log-chat-run1.txt`, `25-plugin-log-chat-run1.txt`, `11-…`, `12-…`, `13-…`, `16-global-B1-B4.png` |
| `feature:identity-reconnect-restart` | `31-probe-after-reconnect.json`, `42-probe-stubdown-after-reconnect.json`, `56-takaro-eventsearch-lifecycle-smoke.json` |
| `feature:consume-owned-trigger` | `21-…`, `14-local-A4-owned-bang.png`, `15-local-A5-owned-slash.png`, `20-takaro-eventsearch-chat.json`, `30-reconnect-R1-R2.png` |
| `feature:fail-privately` | `40-stubdown-S1-t1.png` … `t5`, `42-…`, `43-plugin-log-stubdown.txt`, `44-vein-log-chat-stubdown.txt` |
| `feature:bounded-perf` | `01-baseline-perf-0.7.0-idle.json`, `26-perf-probe-on-5msgs.json`, `50-perf-probe-off-5msgs.json` |
| `feature:native-inventory-autocomplete` | `10-native-command-list-slash.png`, DWARF dumps of `UChatCommand`, `UChatboxUserWidget`, `AVeinGameStateBase` |
| `feature:probe-off-default` | `51-probe-off-D1-D2.png`, `52-vein-log-chat-probe-off.txt`, `53-probe-off-endpoint.json` |
| target `linux-25708939` (0.025h2) | `90-h2-health-probe-on.json`, `93-h2-probe-local.json`, `95-h2-probe-global.json`, `96-h2-vein-log-chat.txt`, `97-h2-plugin-log-chat.txt`, `99-h2-mcp-smoke-and-events.json`, `92-h2-local-H1-H5.png`, `94-h2-global-G1-G2.png`, `98-h2-smoke-sendMessage-M2.png` |
| regression smoke | `57-mcp-smoke-actions.json` (testReachability, getPlayers, sendMessage), `55-smoke-sendMessage-M1.png`, events in `20-…` and `56-…` |

Commands run (all exit 0): `./games/vein/mod/build.sh --tests` on `45fd95b` (unit suite plus the
timeout, transport, native-bridge and native-behaviour suites); after the review fixes
`./games/vein/mod/tests/run.sh` (542 checks) and `./games/vein/mod/build.sh`.

## Side findings

- **`chat-message.channel` is always `global` on 0.025h3.** `ReadChatParams` looks for an
  `EChatSegment` parameter that no longer exists, so Local and Global chat are both reported as
  `global` (every event in `20-takaro-eventsearch-chat.json`). The segment can be derived from the
  command class (`CC_Local_C`, `CC_Global_C`, …) or from the `ChatSegments` set of its CDO.
- `/me` (and every native command) reaches Takaro without its prefix, so modules cannot tell `/me x`
  from `x`.

## Proposed follow-up issues (not filed)

1. **VEIN: route owned chat triggers to Takaro as command invocations (interception).**
   Reuse the probe's `Server_Say` interception with the catalog's exact triggers instead of a nonce.
   Emit the Q8 envelope over the v1 connection and fail privately when the connection is down.
   Gate it behind a capability flag; depends on the shared catalog/invocation protocol slice of #195.
2. **VEIN: report the real chat channel on 0.025h3.**
   Map `SelectedChatType` / `CommandClass` (`CC_Local_C`, `CC_Global_C`, `CC_Radio*`, …) to Takaro's
   channel instead of the removed `EChatSegment` parameter. Unit-test the mapping and prove a Local
   and a Global message in Takaro.
3. **VEIN: detect trigger collisions with native chat commands.**
   At catalog sync, compare Takaro triggers with the native `CommandPrefixes` read from the live
   `UChatCommand` CDOs. Report collisions to Takaro and do not route them; document the fallback prefix.
4. **VEIN: two-client and long-run proof for silent commands.**
   Repeat Q3/Q4 with a second vanilla client as observer, rapid reconnect and server crash mid-invoke,
   plus a 10-minute perf A/B under chat load. Required before calling suppression production-ready.
