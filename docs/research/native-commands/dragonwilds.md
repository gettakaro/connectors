# Native and silent Takaro commands: RuneScape: Dragonwilds

Research for #203 (parent #195). Question: can the Dragonwilds connector take a Takaro command
out of the server chat RPC (`UPlayerChatComponent::Server_SendChatMessage`), resolve the sender
from the live chat component, answer privately, and stop the original line from reaching any
other player, Takaro's `chat-message` stream or Discord? Does `/` reach the server at all?

Probe date: 2026-10-22. The probe ships in this branch but is **off unless
`TAKARO_COMMAND_PROBE=1`** is set together with a nonce and one player id (see
[How the probe works](#how-the-probe-works)). Without those keys no probe code path runs and the
chat hook behaves exactly as on `main`.

## Rescope against current main

The issue was written against game build 25110402 (ELF build-id `3b4ce30aed886594`), connector
0.1.0 and the sidecar architecture. On `main` today:

- The connector is the native plugin alone (0.5.0, Takaro protocol v1, no sidecar). The chat seam
  the issue names is unchanged: `chat-message` still comes from a ProcessEvent slot hook on the live
  `UPlayerChatComponent` vtable, called before the original `Server_SendChatMessage`.
- The default catalog target is `linux-25991736` (`++dominion+live-CL-251392`). Everything below was
  measured on that build, not on 25110402.
- The player record now also carries a Steam id (`platformId: steam:…`) next to the EOS
  ProductUserId `gameId`; the sender is still resolved from the chat component's owning player
  state, as the issue asks.

Everything else in the issue still applies.

## Tested builds

| Component | Exact build |
| --- | --- |
| Dedicated server | Steam app 4019830, build 25991736, catalog target `linux-25991736`, `Build: ++dominion+live-CL-251392` |
| Server binary | `RSDragonwildsServer-Linux-Shipping`, ELF build-id `22bb9848b94d114d` |
| Engine | `5.6.1-251392+++dominion+live` |
| Client | unmodified Windows Steam client (app 1374490), version-locked to the server build (the server refuses other builds) |
| Connector | `libtakaro-dragonwilds.so` 0.5.0 from this branch (main 3859fd8 + the probe): E1–E8 ran on sha256 `ee16f333…5e6e`, the re-run after the review fixes on `ab0b68ef…905e` |
| Takaro | test environment, API `0.0.0-dev.1a17f48`, one test domain, one game server, modules `utils` and a no-echo fork of `chatBridge` (game↔Discord) |

## Go/no-go

| Question | Result | Verdict |
| --- | --- | --- |
| Does the RPC hook see every chat line, before the original runs? | Every typed line fired the hook first: normal text, `@…`, `/…` | **Go** (execution: high) |
| Can the sender be resolved without trusting `SenderData`? | Owner chain component → player state → EOS PUID gave `000204f6…` for every line | **Go** |
| Does `/` reach the server? | `/takaro_probe_<nonce>` and `/takaro_probe_zz000000` both arrived in `Server_SendChatMessage`; the client does not consume them | **Go: server-delivered** |
| Reflected parameter table and `FChatPlayerFilterData` layout | Captured by reflection (table below) | **Go** |
| Suppress only the exact nonce + sender | Every taken line skipped the original: no server fan-out, no `chat-message`, no Discord post, no echo on the sender's screen. Every near-miss line stayed public | **Go** (suppression: medium-high, see limits) |
| Private success / denied / error / timeout | All four answered only the sender; the late stub answer after the timeout was dropped, never replayed | **Go** |
| Private unavailable (stub down) | Answered `unavailable` privately at once; original skipped; chat fine afterwards | **Go** |
| Unknown and colliding prefix-shaped lines stay normal chat | `@takaro_probe_zz000000`, `/takaro_probe_zz000000`, `@takaro_probe_<nonce> rm` reached chat, Takaro and Discord unchanged; Takaro's own `@ping` still answered `Pong!` | **Go** |
| Side effects: warnings, disconnects, moderation, rate limits | None in the server log or on the client during or after the suppressed lines | **Go** (only what is observable on a dedicated server) |
| Second client sees nothing of a taken line | **Not run**: this needs a second client with its own EOS identity, and the test setup has one | **Not testable with one client** (server-side fan-out evidence only) |
| Discovery: chat autocomplete for Takaro commands | `/takaro` typed in the vanilla chat box plus Tab shows no suggestion list; engine-console autocomplete is a different system and not reachable from chat | **No-go** without a client mod |

Confidence: execution **high**, suppression **medium-high** (server-side proof plus one client;
second-client screen not seen), discovery **low** (no autocomplete surface), runtime integration
**low to medium** (needs the parent catalog/invocation contract; nothing here talks to Takaro).

## Reflected layout (build 25991736)

Captured at runtime by walking the UFunction's and the structs' property chains; no offset was
assumed.

| Struct | Size | Properties (name, type, offset) |
| --- | --- | --- |
| `Server_SendChatMessage` params | — | `ChatMessageData` StructProperty @0, `ChatPlayerFilterData` StructProperty @136 |
| `FChatMessageData` (`/Script/JagexChatBackend`) | 136 | `SenderData` StructProperty @0, `MessageBody` StrProperty @120 |
| `FChatPlayerFilterData` | 64 | `SenderId` StructProperty @0, `ReceiverIds` ArrayProperty @48 |
| `FChatPlayerSenderData` | 120 | `PrimaryNetId` @0, `SecondaryNetId` @48, `CharacterGuid` @96, `PlayerId` IntProperty @112, `Color` @116 |

`FChatPlayerFilterData.ReceiverIds` means the client already sends an explicit receiver list with
each line: the game has a native targeted-chat shape. Its runtime contents for global chat were not
dumped in this round.

## How the probe works

Files: `games/dragonwilds/mod/src/command_probe{.h,.cpp,_logic.cpp}`, small hooks in `events.cpp`
and `actions.cpp`, unit test `mod/tests/command_probe_test.cpp`.

- Keys (env or `takaro.cfg`): `TAKARO_COMMAND_PROBE=1`, `TAKARO_COMMAND_PROBE_NONCE` (8–32
  `[a-z0-9]`), `TAKARO_COMMAND_PROBE_PLAYER` (32-hex PUID), `TAKARO_COMMAND_PROBE_STUB=up|down`,
  `TAKARO_COMMAND_PROBE_DEADLINE_MS` (default 2000). Any missing or malformed key keeps it off.
- In the existing ProcessEvent detour, before the original `Server_SendChatMessage` runs, the probe
  takes a line only when it is exactly `@takaro_probe_<nonce>` or `/takaro_probe_<nonce>`,
  optionally followed by `ok|deny|error|timeout`, **and** the sender resolved from the component
  owner is the configured PUID. Then the original call is skipped and no `chat-message` is emitted.
  Everything else (other nonce, unknown mode, other sender) goes through unchanged.
- A local stub answers: success after 150 ms, denied, error, or for `timeout` 3 s after the
  deadline; `STUB=down` answers unavailable at once. The first of {stub answer, deadline} settles
  the invocation; a stub answer after the deadline never wins, and whatever comes later is logged
  and dropped. At most 8 invocations and 16 reply workers exist at once. Past either limit, or if a
  worker cannot start, the line is still suppressed (answered "busy", or not answered and logged),
  so taken input never fails open into public chat.
- Answers go to the sender only, through the same `Client_ReceiveChatMessage` path `sendMessage`
  uses, from a worker via the game-thread pump (never from inside ProcessEvent).
- In probe mode only, every server-side `Client_ReceiveChatMessage` dispatch (the chat fan-out to
  each connected client) is logged with its recipient. This is the server-side oracle for "nobody
  received it".
- The stub is not a command catalog, permission check or invocation API, and nothing it does is
  sent to Takaro.

## Experiments

All on a disposable dedicated server connected to a Takaro test domain, one player, nonce
`p203e9749d02`, the client typing in the vanilla chat box. Logs, screenshots and the raw Takaro
responses are kept with the test run.

| # | Input | Hook fired / sender | Original | Server fan-out | Takaro `chat-message` | Discord | Sender's screen | Result |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| E1 | `probe203 normal hello one` | yes / `000204f6…` | ran | 1 recipient | `89678335…` | posted | line shown | pass |
| E2 | `@takaro_probe_<nonce>` | yes / same | **skipped** | none | none | none | only `[takaro-probe #1] ok` | pass |
| E3 | `/takaro_probe_<nonce>` | yes / same | **skipped** | none | none | none | only `#2 ok` | pass (slash is server-delivered) |
| E4 | `… deny` | yes | skipped | none | none | none | only `#3 denied` | pass |
| E5 | `… error` | yes | skipped | none | none | none | only `#4 failed` | pass |
| E6 | `… timeout` | yes | skipped | none | none | none | `#5 timed out` at +2.0 s; late stub answer at +5.0 s dropped | pass |
| E7a | `@takaro_probe_zz000000` | yes | ran (nonce mismatch) | 1 | `41f762c1…` | posted | shown | pass |
| E7b | `@takaro_probe_<nonce> rm` | yes | ran (unknown mode) | 1 | `70f21521…` | posted | shown | pass |
| E7c | `/takaro_probe_zz000000` | yes | ran | 1 | `d54db49c…` | posted | shown | pass |
| E7d | `probe203 normal hello two` | yes | ran | 1 | `2aba8e21…` | posted | shown | pass |
| E7e | `@ping` (real Takaro command) | yes | ran | 1 | yes | — | `Pong!` | pass |
| E8 | `@takaro_probe_<nonce>` with `STUB=down` (after a server restart) | yes / same | **skipped** | none | none | none | only `#1 unavailable: commands are offline, nothing was run` | pass |
| E8b | `probe203 normal hello four` right after E8 | yes | ran | 1 | `7979dcc3…` | posted | shown | pass |
| D1 | `/takaro` typed, then Tab | — | — | — | — | — | no suggestion list | no autocomplete |
| E9 | second client watches E2–E6 | — | — | — | — | — | — | **not testable with one client** |

The client never echoes a line locally: a taken line simply never appears on the sender's own
screen, only the private answer does. There were no warnings, kicks or disconnects in the server
log while lines were suppressed, and chat kept working afterwards. The server sends periodic EOS
`SendBackendEvent` metrics; whether those would have counted the suppressed lines is not observable
from the server.

## Limits

- One account: remote silence is shown on the server (zero `Client_ReceiveChatMessage` dispatches
  for a taken line, against one per recipient for every public line), not on a second screen.
- Suppressing the RPC also skips whatever the game's own `Server_SendChatMessage` implementation
  does besides fan-out (telemetry, moderation hooks). Nothing visible depended on it here.
- No discovery surface: the vanilla client's chat box has no autocomplete. Players can only learn
  commands from a help command or the Discord/website.
- Build-specific: every claim above is for `linux-25991736` only.

## Production recommendation

Go for a native, silent command path in the connector, gated on the parent contract (#195):

1. Take a line only when it starts with a prefix the connector owns **and** its first word matches
   an entry of a Takaro-owned live catalog for this server; everything else stays chat. Both `@` and
   `/` work; `/` is the better player-facing prefix because it is never valid chat.
2. Resolve the sender from the chat component's owner, never from the message.
3. Skip the original RPC, emit no `chat-message`, answer privately, and settle each invocation
   exactly once with a bounded deadline. Never queue or replay.
4. When Takaro is unreachable, answer "unavailable" privately at once; never fail open.

## Required parent-contract changes (#195)

- A connector-facing live command catalog (names and aliases per server, with a revision).
- An invocation request with an id, the resolved player and the raw arguments, and a correlated
  result (success / denied / error) the connector can show privately.
- Takaro must keep owning permission checks and argument parsing; the connector only routes.

## Verification

- `./mod/tests/run.sh` (container): unit suites pass, including `command_probe_test`
  (config, exact matching, stub outcomes, first-settle-wins, bounded table).
- `./mod/build.sh`: builds with no warnings; no undefined strong symbols.
- `/verify --mode=report-only --scope=branch` (light; Codex as the independent reviewer): five
  findings. Four were fixed (a failed worker start could fall back to public chat; the saturation
  path was not bounded; a late stub answer could win a race with the deadline worker; a trailing
  space was accepted as mode `ok`). One was accepted: the engine-bound hook and reply path has no
  unit test, because it only exists inside the game process; E1–E8 prove it live.
- Live: experiments above, plus the regression smoke (`player-connected`, `chat-message`,
  `testReachability`, `getPlayers`, `sendMessage`) on the probe build.

## Proposed follow-up issues

1. **dragonwilds: silent native Takaro commands behind the catalog contract**
   Implement the recommendation above once #195 defines catalog and invocation.
   Prefix + live-catalog ownership, owner-resolved sender, skip RPC, private bounded answer.
   Unknown and colliding input stays chat; unavailable answers privately.
2. **dragonwilds: prove remote silence with a second client**
   Repeat E2–E6 with a second unmodified client on a distinct EOS identity.
   Record the second screen and server fan-out for each line.
   Only possible on a setup with two game clients.
3. **dragonwilds: native targeted chat via `FChatPlayerFilterData.ReceiverIds`**
   Dump the runtime receiver list for global and private lines.
   If the game honours it, use it for private replies and `sendMessage` whispers.
   Would also fix the "renders under the receiver's name" caveat if the sender id is honoured.
