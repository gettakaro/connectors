# Native and silent Takaro commands: Enshrouded

Research for #202 (parent #195). Question: can the Enshrouded dedicated server recognise a Takaro
player command in chat, keep it out of the broadcast, and tie it to the sender's SteamID64, so
that a later production feature can run Takaro commands privately?

Probe date: 2026-10-22. The probe is in this branch under
[`games/enshrouded/research/command-probe/`](../../../games/enshrouded/research/command-probe/README.md).
It is compiled only by `TAKARO_COMMAND_PROBE=1 ./mod/build.sh` and acts only while
`<server>\takaro\command-probe.json` exists. A normal build produces a byte-identical
`dbghelp.dll` (see [Release build unchanged](#release-build-unchanged)).

## Rescope against current main

The issue cites dedicated-server revision `1024233` and `ChatSystem::addMessage` at RVA
`0x80bbf0`. That build is no longer current:

- `main` targets Enshrouded 1.0 (game build `1267041`, Steam build 25967645, catalog target
  `proton-1267041`).
- The public branch has since moved to the 1.0 hotfix, game build `1273043` (Steam build
  26083076). Steam clients only join a server on the same build (checked: a 1273043 client lists a
  1269579 server as "Version Mismatch" and cannot join), the 1267041 depot manifest is no longer
  served anonymously by Steam, and the support target for 1273043 is open in #582.

So every static and live result below is for **game build 1273043**. `1024233` was not
re-examined. In 1.0, `addMessage` lives at RVA `0x90b330` with a different prologue, and its
8th argument changed from an `Optional64*` to a plain `u64` (the sender's session account).
The issue's premise still holds: an in-process hook on `addMessage`, filtered by the return
address of the player-chat call site.

## Tested builds

| Component | Exact build |
| --- | --- |
| Enshrouded dedicated server | Steam app 2278520, build 26083076, game build `1273043` (`release_1.0`, 2026-10-20), `enshrouded_server.exe` sha256 `d7baa768eb00aa895d9f5a1cd8747ccf66858c7b542f50983092b2e3261588ee`, under GE-Proton |
| Enshrouded client | Steam, Windows, build version `1273043` (shown in the main menu) |
| Connector | `takaro-enshrouded` plugin 0.8.1 (`main` at 3859fd8) plus the 1273043 game data from #582, built with `TAKARO_COMMAND_PROBE=1`. Probe DLLs (md5): `e227edb1…` (first run), `7ee1cbe5…` (whitespace fix, below), `6b0fc03f…` and `0dc00514…` (review fixes, see [Verification](#verification)). Protocol 1, on Takaro staging as a GENERIC server |
| Disassembler | capstone 5.0.7 + pefile over the exe above; function extents from `.pdata` with chained unwind entries followed |

**Static** means read in the disassembly of that exe. **Live** means observed with a real
Enshrouded client joined to a test dedicated server, with Takaro staging as the oracle (stored
events, hook executions) and the client screen as the game-side check.

## Go/no-go

| # | Question (acceptance criterion) | Verdict | Experiment result |
| --- | --- | --- | --- |
| 1 | Preserve disassembly of `addMessage`, the player-chat call site and the normal return path | go | passed (static, 1273043) |
| 2 | Argument widths, error-result size and success value, returned pointer, caller-observed side effects | go | passed (static); the suppressed return survived 22 live suppressions, a reconnect, a graceful shutdown and a restart |
| 3 | Two real clients: ordinary, `/`, `!`, `@`, leading space, quoted, Unicode, max length; server text and sender echo | partial | one client: passed for ordinary, `/`, `!`, `@`, leading space, quoted and maximum length. Unicode: not run (the test input tool sends ASCII only). Peer view: not testable with one client (row 4a) |
| 4 | Suppress one magic probe trigger before broadcast; sender and peer see nothing; chat, reconnect and shutdown stay healthy | go (sender) | sender, Takaro and Discord: passed (live) |
| 4a | Peer isolation: a second player sees neither the suppressed command nor the sender's private reply | not tested | not testable with one client. The test setup has one game account; statically the server creates no broadcast record for a suppressed message (finding 2) and the private reply is marked delivered to every other machine, but neither was observed from a second client |
| 5 | Bind the sender to SteamID64 through handle, slot, login state, entity and machine; reject stale, out-of-range, ambiguous and non-player senders | go | accept path: passed (live, two handle generations, account cross-check). Reject branches: the decision's handling is unit-tested; the memory checks that produce them are code-reviewed only (not reachable with one client, not host-testable) |
| 6 | Unavailable before send, failure during send, restart, stale catalog, duplicate trigger/alias, disabled, reserved `link`, permission denial, unknown command | go | passed (live, each case) |
| 7 | Recognised command stays private, fails privately at once when unavailable, never replays after reconnect, no duplicate invocation | go | passed (live, sender side); private relative to a peer: not testable with one client (row 4a) |
| 8 | Production recommendation (catalog revision, prefix and aliases, reserved names, identity, invocation id, deadline, receipt, private failure, capability disable) | done | [Recommendation](#recommendation-for-production) |
| 9 | `/verify` report-only after each probe phase, `/verify` at the end | done | [Verification](#verification) |

**Overall: go** for server-side recognition, suppression and private reply on Enshrouded 1.0,
with no client mod, as far as the sender, Takaro and Discord are concerned. What a second player
sees is not testable with one client and was not tested (row 4a): statically the server builds no
broadcast record for a suppressed message and marks the private reply as delivered to every other
machine. Unicode input was not run because the test input tool sends ASCII only.

## Static findings (game build 1273043)

The complete listings, made from the exe named above, are in
[`enshrouded/1273043-addMessage.asm`](enshrouded/1273043-addMessage.asm) (the whole function,
all three unwind chunks) and
[`enshrouded/1273043-chat-callsite.asm`](enshrouded/1273043-chat-callsite.asm) (the
player-chat call site with the code before and after it, plus the session lookup it calls).

### 1. `ChatSystem::addMessage`, RVA `0x90b330`

Callers (every direct `call` in `.text`): `0xcbe1b` (ECS notice), `0x7a47d3`, `0x7b50d9`
(player chat in `processIncomingMessages` `0x7b45b0`, return address `0x7b50de`) and `0x7b69ad`
(in `updatePlayers` `0x7b5800`).

| Arg | Where | Width | Meaning |
| --- | --- | --- | --- |
| 1 | `rcx` | pointer | error result, **8 bytes**: `u8 code` at +0, `u32` at +4. Returned in `rax` |
| 2 | `rdx` | pointer | chat system (`server+0x60` on this build) |
| 3 | `r8b` | u8 | message type (client-supplied for player chat) |
| 4 | `r9` | pointer | text view `{const char* ptr; u64 len}` |
| 5 | `[rsp+0x28]` | u32 | localisation tag 1 → record `+0xc` |
| 6 | `[rsp+0x30]` | u32 | localisation tag 2 → record `+0x10` |
| 7 | `[rsp+0x38]` | u32 | sender player handle → record `+0x14` |
| 8 | `[rsp+0x40]` | u64 | sender session account → record `+0x18` (was `Optional64*` on 1024233) |
| 9 | `[rsp+0x48]` | pointer | `{u64 value; u8 has@+8}` → record `+0x38` |

There are exactly two returns:

- `len > 0x400`: writes `[rcx+4]=0`, `[rcx]=0x12`, returns `rcx`. No other write.
- Success: allocates `len+0x40` from `chat+0x190`, evicting the oldest records while the
  allocator is full. It zeroes and copies the text to `+0x40`, fills the record header, appends
  the record to the list (head `+0x1c0`, tail `+0x1c8`, count `+0x1d0`), and walks 32 recipient
  machines (stride `0xc`). An inactive machine gets its done bit set. An active one gets
  `align4(len+0x17)` added to its pending bytes. Then it writes `[rcx+4]=0`, `[rcx]=0` and
  returns `rcx`.

So the success value is code `0` with `+4 = 0`. Apart from the error result, **the record is
the only output**: no global state, logging, event or counter outside the chat system is
touched.

### 2. The player-chat call site ignores the result

At `0x7b5095..0x7b50d9`, `processIncomingMessages` passes `rcx = rbp+8` (a stack error result)
and `rdx = [server+0x60]`. `r8b` is the type byte of the deserialised `ChatMessageClientMessage`,
`r9` is a stack view over its text, and arguments 5 to 9 are the two localisation tags, the
sender handle (`edi`), the sender account and an empty optional. After the call (return address
`0x7b50de`), the caller reads neither `rax` nor `[rbp+8]`. It only writes back its channel read
cursor and jumps to the end of the loop body. `[rbp+8]` is not read again before it is
reinitialised for the next message.

**Consequence:** a detour that recognises the message and returns at once, after writing exactly
what the success path writes (`err[0]=0`, `*(u32*)(err+4)=0`, return `err`), is
indistinguishable to the caller from a delivered message. No record is created, so the per-tick
flush has nothing to send to any machine. That covers the sender and every peer, because the
server builds no broadcast at all.

### 3. Sender identity

- Argument 8 comes from the game's own session lookup `0x9f7600` (`sessionMgr = [server+0x10]`).
  It indexes the active session version's record by `handle & 0x3f` (stride `0x150`), then
  requires `record+0x178 == handle` over the **full 32 bits**. A stale handle fails with code
  `0x1f`, and the caller then logs `Received chat message from unknown Source. Handle '%k'` and
  passes account 0.
- Live, the handle is `generation << 6 | slot`: `0x40` on the first join (slot 0, generation 1)
  and `0x80` after a leave and rejoin (slot 0, generation 2). A handle from an earlier session
  can therefore never match a later occupant of the same slot.
- The session account is the 64-bit `accountIdHash` the game uses for bans, **not a SteamID64**.
  The connector learns SteamID64 ↔ machine from the server's own login log lines (its existing
  `logSink` hook), so the probe binds: handle → exactly one of the 16 slots with that full
  handle → login state `3` → non-zero player entity → session record for the full handle whose
  account equals argument 8 → machine `slot+4 & 0x7f` → exactly one online SteamID64 for that
  machine → exactly one online SteamID64 for that machine, whose accountIdHash in the connector's
  account cache (`accounts.json`) equals the game's session account; a second online SteamID with
  that account is refused as ambiguous, a SteamID with no cached hash as unverified. Persona and
  character names are never used. Residual: the cache entry is itself first learned through the
  machine mapping, so the very first command of a never-seen account rests on the login log
  correlation alone.
- The current production `chat-message` event resolves the player **by persona name**
  (`PlayerJsonByName`). The chain above is strictly better, and it is a follow-up below.

### 4. Threads

`updatePlayers` (`0x7b5800`) and `processIncomingMessages` (`0x7b45b0`) are both called from
the same server update function (`0x7b2940`). The `addMessage` detour therefore runs on the
server thread, so a private reply can be queued right there, with the same whisper bookkeeping
as `sendMessage` (done bit plus pending bytes for every other active machine). No cross-thread
wait is needed. `RunOnWorld` would deadlock here, because its queue is drained by the very
thread that is waiting.

## Live findings

All rows are in the probe's own log (one JSON line per player chat while a catalog is loaded:
exact bytes as hex, every sender check, the decision, the stub receipt and the reply route).
Takaro's stored events were read back through the API after each phase.

| Experiment | Input (as typed) | Server received | Client (sender) | Takaro |
| --- | --- | --- | --- | --- |
| Ordinary chat | `probe202 ordinary 1 x y /z` | same bytes, type 0 | `Character 1: …` line | `chat-message` stored |
| `/` | `/tkprobe e1` | `2f746b…` | public line | stored as typed |
| `!` | `!tkprobe e1` | `21746b…` | public line | stored as typed |
| `@` (no catalog claim) | `@tkprobe e1` | `40746b…` | public line | stored as typed |
| Leading space | ` @tkprobe e1-leading` | `20 40 74…` (space kept) | public line | stored **trimmed**: `@tkprobe e1-leading` |
| Quoted | `"@tkprobe e1-quoted"` | `22 40 …22` | public line | stored with quotes |
| Maximum length | 1030 characters | **256 bytes** (the client caps input) | public line | stored, 256 bytes |
| Unicode | `ü`, `✓` | — | — | not run: the test input tool sends only ASCII (tried plain typing and Alt+numpad) |
| Magic trigger (nonce) | `@tkp<nonce> s1` | suppressed | **no chat line**; only the private reply `[probe] tkp…: submitted (id …-9)` | no `chat-message`, no Discord post |
| Catalog command | `@tkprobe s2 args` | suppressed, args `s2 args` | private `submitted` | nothing stored |
| Disabled | `@off` | suppressed | private `command disabled` | nothing |
| Reserved | `@link 1234` | suppressed | private `reserved command` | nothing |
| Permission | `@admin` (no grant) | suppressed | private `permission denied` | nothing |
| Duplicate alias | `@dup` (two commands claim it) | suppressed | private `ambiguous command` | nothing |
| Unknown | `@nosuch s3` | passed through | public line | stored (as today) |
| Failure during send | `@tkprobe s4` with `failDuringSend` | suppressed, receipt `failed` | private `failed, not run` | nothing |
| Unavailable before send | `@tkprobe s5` with `available:false` | suppressed, no invocation | private `Takaro is unavailable` | nothing |
| Stale catalog | `@tkprobe s6`, catalog older than its max age | suppressed | private `command list is out of date` | nothing |
| Broken catalog file | `@tkprobe s7` after a valid catalog | suppressed (old claims kept, all refused) | private `Takaro is unavailable` | nothing |
| Probe off | catalog file removed, `@tkprobe s8-probe-off` | passed through | public line | stored |
| Reconnect | `@tkprobe s9-before-leave`, leave, rejoin | s9 logged once; new handle `0x80` | after rejoin: ordinary chat public, `@tkprobe s10…` private | s9 never replayed |
| Graceful shutdown + restart | Takaro `shutdown` | save written at the shutdown second, exit 0; probe re-armed, catalog reloaded, no lines replayed | rejoin, chat and probe healthy | `player-connected` and `chat-message` stored |
| Whitespace (after the fix) | ` @tkprobe e1-leading-v2`, `  @link 9999` | `20 40…`, `20 20 40…` | both private | nothing |

What this shows:

- **There is no sender-local echo.** The sender's own line comes back from the server
  broadcast, so a suppressed command is invisible to the sender too. Only the private reply
  appears.
- **Defect found and fixed in the probe:** the client keeps leading spaces, but the connector
  trims `msg` (`native::Str` → `Trim`, C `isspace`) before Takaro matches the prefix. A
  byte-exact claim would therefore show ` @link 1234` publicly in game while Takaro still ran it
  as a command. The probe now claims on the trimmed text. Production must use the **same
  normalisation** as the `chat-message` mapping, so that Takaro and the server always agree on
  what is a command.
- The client caps chat input at 256 bytes, while the server accepts 1024.
- Discord: the chat bridge posted ordinary chat (`GameToDiscord`, 200) and the `PlayerConnected`
  module post (200). Discord→game delivered `[D] probe202: …` to the client. No suppressed line
  ever produced a `chat-message`, so none was bridged to Discord.
- Regression smoke on the final probe build: `testReachability` connectable, `getPlayers` lists
  the player, `sendMessage` rendered as `Takaro: probe202 smoke sendMessage 5902`, and
  `player-connected` and `chat-message` were stored.

## How the probe works

- **Catalog:** a local stub file `<server>\takaro\command-probe.json` (`revision`, `prefix`,
  `nonce`, `issuedAtMs`, `maxAgeSeconds`, `available`, `failDuringSend`, `commands[]` with
  `aliases`, `enabled`, `permission`, plus `grants{steamId: [permission]}`). A probe thread
  re-reads it every 0.5 s. A rejected file keeps the previous claims but refuses everything.
- **Collision preflight:** the nonce must be set, must not be a reserved name (`link`) and must
  not equal any command name or alias. On the staging server, the only installed module
  (`chatBridgeNoEcho`) defines no commands, and the domain prefix is `@`.
- **Decision** (`probe_core.cpp`, pure, host-tested): a name the catalog (or the nonce, or a
  reserved name) claims is **always** suppressed. It is then invoked or refused privately; it
  never falls back to public chat. Unclaimed text passes unchanged.
- **Receipt:** a local JSON line marked `"stub":true` with status `submitted`, `failed` or
  `rejected`. Nothing is sent to Takaro, and a stub receipt is not Takaro execution.
- **Reply:** a type-4 notice queued on the server thread for the sender's machine only.
- **Safety gates:** compiled only with `TAKARO_COMMAND_PROBE=1`; armed only on game build
  `1273043` with the 1.0 chat and session layout, which needs that build's game data (#582, or a
  `1273043` section in `takaro/enshrouded-gamedata.json`); inert without the catalog file.
  Server-thread work is memory reads and queueing: catalog parsing, the account-cache snapshot,
  `plugin.log` and the probe log all run on the probe thread, and the shared lock is held only to
  swap pointers and queues. A catalog file that exists but cannot be read or parsed keeps the
  previous claims and refuses everything; only a missing file turns the probe off.
  `issuedAtMs` and `maxAgeSeconds` (1 s to 7 days) are required, so a bad value can never
  disable the stale check.

## Release build unchanged

With `SOURCE_DATE_EPOCH=1700000000`, `./mod/build.sh` on `main` and on this branch both produce
`dbghelp.dll` sha256 `3e1cc46b3c32389d840c5b211880414cc036eba0fe6cd66215ded9de682136b9`.

## Recommendation for production

The constraints a shared runtime catalog and a command invocation/receipt contract must meet on
Enshrouded:

1. **Where:** detour `ChatSystem::addMessage` and act only on the player-chat call site (by
   return address from the game data, as the connector already does). Suppress by returning the
   success result (`code 0`, `+4 = 0`) without calling the original. Re-derive and re-verify
   both on every game build, and refuse to arm on a build whose ABI was not checked. Never
   extend the synthetic administrator console (`executeConsoleCommand`) into a player-command
   path.
2. **Catalog revisioning:** the connector holds the last catalog Takaro pushed, with a revision
   and an issue time. Takaro sends a new revision on every change, and the connector
   acknowledges the revision it applied. A catalog older than its max age keeps its claims but
   refuses everything privately ("command list is out of date"). A missing or broken update
   never drops the claims.
3. **Prefix and aliases:** the per-server prefix comes with the catalog. Match on the text
   **after the same trim the `chat-message` mapping applies**, case-insensitively for ASCII
   names. A name claimed twice is refused as ambiguous and never runs twice. Unclaimed prefixed
   text stays public chat, as today.
4. **Reserved names:** `link` (and whatever Takaro reserves) is always claimed, even without a
   catalog entry, so a link code never reaches public chat or Discord.
5. **Identity:** bind every invocation to SteamID64 through the chain in finding 3 (full
   handle, one slot, login state, entity, session record whose account equals the game's own
   argument, one machine, one online SteamID64). Refuse privately on any mismatch. Never use
   persona or character names.
6. **Invocation id, deadline, receipt:** one id per chat message (boot id plus sequence),
   created on the server thread and sent once over the existing WebSocket with a deadline.
   Takaro answers with a receipt (`accepted`, `rejected` with reason, or `failed`). There is no
   automatic retry: an invocation without a receipt by its deadline is reported to the player
   as failed, and it is never replayed after a reconnect or restart (the probe keeps none in
   memory across either). Several installations matching one command run on the Takaro side,
   from one invocation.
7. **Private failure:** every refusal and every failure is a private type-4 notice to the
   sender's machine (whisper bookkeeping as `sendMessage`), sent at once from the server thread.
8. **Capability disable:** when the bridge is down, the connector keeps suppressing claimed
   names and replies "Takaro is unavailable". It never fails open into public chat. A connector
   built without the feature (or on an unverified build) claims nothing, so behaviour is
   exactly today's.
9. **`chat-message`:** suppressed commands must not also be emitted as `chat-message`
   (otherwise Takaro would run them a second time through its chat command path, and the chat
   bridge would post them to Discord).

## Proposed follow-up issues

1. **feat(enshrouded): private player commands through the shared command catalog** —
   Implement recommendation 1–9 behind a capability, once the parent (#195) has a Takaro catalog
   and invocation/receipt contract. Reuse the probe's decision core and sender chain, replace the
   stub file and the local receipt with the protocol messages, and keep refusing on unverified
   builds.
2. **test(enshrouded): peer isolation and Unicode input for private commands** —
   In a setup with two game clients on one server, check that a peer sees neither a suppressed
   command nor the sender's private reply while ordinary chat reaches both. Also record the server
   bytes of Unicode chat input (`ü`, `✓`, CJK) typed on a real keyboard.
3. **fix(enshrouded): attribute chat-message to SteamID64 through the session record, not the
   persona name** — Today `chat-message` resolves the player by persona name, which can collide.
   Use the handle → slot → session account → machine → SteamID64 chain from finding 3, and send
   no player rather than a guessed one.

## Verification

- Probe decision core: `games/enshrouded/research/command-probe/tests/run.sh` → `probe_core_test: 38 passed, 0 failed`.
- Connector host tests: `games/enshrouded/mod/tests/run.sh` (unchanged code paths) → see the PR body.
- Release build identical to `main` (above). Probe build: `TAKARO_COMMAND_PROBE=1 ./mod/build.sh` compiles clean.
- `/verify --mode=report-only --scope=branch` (light depth: Codex as the independent reviewer) after
  the probe phases found 8 issues in the probe, all fixed:
  file I/O (`PluginLog`) on the server thread → queued to the probe thread; SteamID picked by
  machine only → cross-checked against the account cache; catalog parsed under the lock the
  server thread takes → parsed outside it; a transient read error made claimed names public →
  claims kept; missing or malformed freshness values disabled the stale check → now required and
  strict; `TAKARO_COMMAND_PROBE=0` enabled the probe → only `=1`; the build/game-data dependency
  and the peer claims are now stated precisely (this document). The fixed build was re-run live:
  ordinary chat public, the nonce suppressed and receipted, `@off` refused privately, sender
  bound with the account cross-check, an unreadable catalog refused everything and logged once,
  and every probe log line came from the probe thread.
- Final `/verify` (light, Codex) on the fixed branch: no regressions in the fixes. Two new points:
  probe log lines were lost when a write failed (fixed: they are re-queued), and a catalog that is
  unreadable at the very first poll leaves the probe off, because no claims are known yet. That is
  the probe's design; production must instead persist the last applied catalog across restarts
  (recommendation 2), so a server never starts without its claim set. Peer isolation is not testable
  with one client (row 4a).
