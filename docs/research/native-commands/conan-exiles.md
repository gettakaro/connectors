# Native and silent Takaro commands: Conan Exiles

Research for #205 (parent #195). Question: can a namespaced Takaro command typed in Conan
Exiles chat be intercepted before Conan broadcasts or logs the original text, with the caller's
identity coming from the server, a private answer, and safe offline and runtime behaviour?

Probe date: 2026-10-22. The probe code is behind an environment variable and does nothing unless
it is set. Nothing ships to users from this work.

## Rescope

The issue was written for the Enhanced DevKit: a cooked Blueprint mod on server and client,
Pippi as an oracle, and `DataCmd` as the fallback. The connector has since gone native (#362).
It is an in-process library (Linux `LD_PRELOAD` `.so`, Windows `winmm.dll` proxy) with no mod,
no RCON and nothing for players to install. Mods built with older DevKits are refused by the
current server build. So this research answers the same question for the current architecture:

- The interception point is the server's own chat RPC,
  `ConanPlayerController.ServerSendChatMessage`. The connector already hooks it for
  `chat-message`, and its `ProcessEvent` detour can veto a call.
- The private reply is the connector's existing targeted `sendMessage`
  (`ClientReceiveChatMessage` on the caller's controller only).
- The DevKit asset inspection, the Pippi `ModCommandStruct` review and a cooked Pippi-free mod
  were not run. They would add a client mod, which the native connector exists to avoid. Pippi
  stays out of the production route, as the issue asks.

## Tested builds

| Component | Exact build |
| --- | --- |
| Conan Exiles Dedicated Server | Steam app 443030 build 25792439, both catalog targets: `windows-25792439` (Shipping exe) and `linux-25792439` (Linux server in a container, `LD_PRELOAD`); RCON off |
| Client | Conan Exiles Enhanced (Steam app 440900) revision #379,176/0, vanilla, no mods, one Steam account |
| Connector | `takaro-conan-exiles` 3.4.0 (`games/conan-exiles/version.txt`), protocol 1, with the probe from this branch. Windows: commit `33447bf` (`winmm.dll` sha256 `a9cdf798…`) for boot 1, then `d830257` (sha256 `bb8399ef…`) for boot 2 after the defect fix below. Linux: `f31d625` code (`libtakaro-conan-native.so` sha256 `c265f3b1…`) |
| Takaro | Staging, one GENERIC game server per target reporting `game: conan-exiles`, `gameVersion: 25792439`, `connectorVersion: 3.4.0` (`gameserverGetOne.json`); command prefix `@`; modules `utils` and a chat-bridge fork with Discord |
| Stub | `games/conan-exiles/research/command-probe/stub.py`, Python 3.12 on the server host |

## Go/no-go

| # | Question | Verdict | Experiment result |
| --- | --- | --- | --- |
| 1 | Intercept a namespaced command on the server before Conan broadcasts or logs it | **go** for a non-slash namespace (`.tk`, or Takaro's `@`) | passed |
| 2 | Slash commands (`/tk ...`) | **no-go** without a client mod: the vanilla client consumes every `/` line itself | passed (reproduced limitation) |
| 3 | Stable identity from the server, not from typed text | **go** | passed |
| 4 | Private, caller-only feedback | **go** | passed (caller side); isolation from a second client blocked: one client |
| 5 | Normal chat, unknown and unowned commands keep the game's behaviour | **go** | passed |
| 6 | Offline: fail privately, no replay, no public fallback | **go** | passed |
| 7 | Catalog: runtime revision, removal, repeated registration | **go** | passed |
| 8 | Permission denial decided on the server | **go** | passed (stub-side; a real Takaro permission check is follow-up work) |
| 9 | Quoted arguments, aliases, intentional fanout | **go** | passed |
| 10 | Reconnect and server restart | **go** | passed |
| 11 | Native help or autocomplete for Takaro commands | not available | passed (observed: none) |
| 12 | `DataCmd` console route | not chosen | not run (console UX, needs a mod-defined command table) |

Decision: **vanilla hook**. The production route is a server-side veto in the native connector's
`ServerSendChatMessage` hook for lines that start with the domain's command prefix and name a
known Takaro command. Players install nothing. `/` cannot be the prefix: on Conan it is
client-owned.

## How the probe works

`games/conan-exiles/native/core/conan/command_probe.{h,cpp}`, off unless
`TAKARO_CONAN_COMMAND_PROBE` names a directory (the host test checks it is off by default):

- The existing chat hook (`events.cpp` `OnChat`, Before `ServerSendChatMessage`) reads channel
  and text. It then asks the probe. A line that starts with the catalog prefix (followed by a
  space) and names a catalog trigger or alias is *claimed*: the handler returns false, so the
  game's implementation never runs, and no `chat-message` event is queued. Everything else goes
  on unchanged.
- On the game thread the probe only classifies and enqueues. A worker thread does the file I/O
  and the replies.
- Catalog discovery and invocation are separate files in the probe directory.
  `catalog.json` is written by the stub (`{revision, prefix, commands:[{trigger, aliases}]}`) and
  re-read every second. A broken or half-written file keeps the last good catalog, so known
  commands never leak.
- Invocation goes to `inbox/<id>.json`: id, catalog revision, trigger, typed word, raw and split
  arguments, the player DTO from the connector's identity code, and the channel. The reply comes
  back in `outbox/<id>.json`, and each line goes to the caller only. Ids carry a random per-boot
  part, so a reply left over from an earlier boot is never read.
- Offline means `stub.json` has had no heartbeat for 3 s. The command then fails privately at
  once and is never written to the inbox. A reply later than 5 s fails privately ("may not have
  run") and the late reply file is deleted, never delivered.
- The stub stands in for Takaro. It answers `ping`, `whoami`, `echo`, `fan` (two backends for
  one trigger), `admin` (admins by server-supplied gameId), `slow` (answers after 7 s) and
  `help`. It logs every invocation to `receipts.jsonl`. Inbox files present when it starts are
  moved aside, never answered.

## Experiments

The table below is the Windows run; the Linux run follows it. Both used the real client, typing
in the client's chat through an input-automation tool. The evidence file names below refer to the
run's evidence folder, which is kept with the test run and not in this repository: screenshots in
`shots/`, logs in `logs/`, raw Takaro API responses at the top level.

### Inspection: the chat path on build 25792439

- Client to server: `ConanPlayerController.ServerSendChatMessage(chatData)`, a server RPC.
  `chatData` is `ChatRpcData` (0x80 bytes): Timestamp (FILETIME) at 0x00, UserName 0x48,
  Channel 0x58, Message 0x68, bGenerated 0x78 (`native/core/conan/text.h`,
  `tools/reflection-manifest.json`). It has no return value or "handled" flag, so nothing in the
  game makes it cancellable. The detour's veto is what cancels it.
- Server to client: `ConanPlayerController.ClientReceiveChatMessage(chatData)`, flags `0x1020cc0`
  (Net, NetReliable, Native, Event, Public, NetClient) in the reflection fixture.
- The game's chat-log line (`ChatWindow: Character … said: …` in `ConanSandbox.log`) and the
  fan-out to other players both happen inside the `ServerSendChatMessage` implementation. A
  vetoed call produced neither the log line nor a Takaro event (below).
- The game's fan-out does **not** pass through `ProcessEvent` for `ClientReceiveChatMessage`.
  The probe's broadcast counter saw 0 calls while 7 public lines were delivered
  (`logs/events-health-boot1.json`, `broadcastsSeen: 0`). So "not broadcast" cannot be proven by
  hooking that function. The evidence is that the implementation never ran.
- The vanilla client has its own chat commands, handled in the client: `/help`, `/whoami`,
  `/fontsize`, `/showchannelname`, `/showtimestamp`, and `(emote)` text
  (`shots/03-client-help.png`, `shots/12-vanilla-whoami.png`). Any `/...` line, known or not,
  is consumed by the client and never sent. `/tk ping` produced no server log line, no hook call
  and no event (`shots/02-tk-ping.png`, `logs/native-boot1-probe.log`).

### Results

| Experiment (row) | What was typed or done | Result | Evidence |
| --- | --- | --- | --- |
| `feature:slash-command-reaches-server` | `/tk ping` (catalog r1, prefix `/tk`) | failed as a route: the client swallows it, and the server sees nothing | `shots/02-tk-ping.png`, `logs/native-boot1-probe.log` |
| `feature:silent-known-command` | `.tk ping` (catalog r3) | passed: vetoed, the stub got the invocation, the private `Pong!` arrived 121 ms later; no `ChatWindow` line, no `chat-message` event, no Discord post | `shots/04-tk-ping.png`, `logs/server-chat-log-boot1.txt`, `events-window.json`, `discord-hook-executed.json` |
| `feature:sender-local-echo` | same | observed: the **sender's own** chat shows `[winlthree]: .tk ping`. The client echoes locally before the server answers; the server never logs or relays it | `shots/04-tk-ping.png` |
| `feature:server-identity` | `.tk whoami 76561190000000001 "Server Admin"` | passed: the stub got gameId/steamId `76561198000735875`, `platformId steam:…` from the server; typed ids ignored | `shots/05-whoami-forged.png`, `logs/receipts-all.jsonl` |
| `feature:quoted-args` | `.tk echo a "b c" d` | passed: `[a] \| [b c] \| [d]` | `shots/06-quoted-args.png` |
| `feature:alias` | `.tk p` | passed: ran `ping` | `shots/07-alias.png` |
| `feature:fanout` | `.tk fan` | passed: both backend replies, privately | `shots/08-fanout.png` |
| `feature:permission-denied` | `.tk admin`, caller not an admin | passed: private denial | `shots/09-admin-denied.png` |
| `feature:unknown-namespaced` | `.tk nope 1` | passed: not claimed, normal public chat and `chat-message` | `shots/10-unknown-namespaced.png`, `events-window.json` |
| `feature:help` | `.tk help` | passed: the catalog list, privately (stub-made) | `shots/11-help.png` |
| `feature:vanilla-name-collision` | `/whoami` with `whoami` in the catalog | passed: the client's own `/whoami` still works; `.tk whoami` is ours. They cannot collide because `/` is client-owned | `shots/12-vanilla-whoami.png` |
| `feature:timeout` | `.tk slow` | passed: private "did not answer in time" after 5 s, late reply dropped. Defect found (below) and fixed | `shots/13-slow-timeout.png`, `shots/22-slow-after-fix.png` |
| `feature:offline` | stub stopped, `.tk ping`, `.tk admin` | passed: private "Takaro is offline … was not run", nothing queued (`offline: 2`) | `shots/14-offline.png`, `logs/native-boot1-probe.log` |
| `feature:no-replay-after-recovery` | stub restarted with the player as admin, `.tk admin` | passed: the stub log shows no invocation for the two offline commands; the new call ran | `shots/15-recovered-admin.png`, `logs/stub-all.log` |
| `feature:catalog-revision` | r1 → r2 → r3 → r4 live, r4 written twice, without `fan` | passed: each revision loaded within 1 s without a restart; the identical rewrite did not reload (`catalogLoads` 4 for 4 distinct texts); removed `.tk fan` became normal public chat | `shots/16-catalog-r4-fan-removed.png`, `events-window.json` |
| `feature:reconnect` | Takaro kick, then rejoin, `.tk whoami` | passed: same identity after reconnect | `shots/19-kicked.png`, `shots/20-reconnect-whoami.png` |
| `feature:server-restart` | Takaro shutdown action, restart with the fixed library, `.tk ping` | passed: new boot id, catalog r4 read from disk, command works | `shots/21-restart-ping.png`, `logs/native-boot2-probe.log` |
| `feature:two-clients` | | blocked: one Steam account and one client, so isolation from a second player was not observed | |
| `feature:existing-takaro-commands` | `@ping` | passed: Takaro's chat-based command still answers `Pong!` (public, as today) | `shots/18-takaro-at-ping.png` |

Counts (from `logs/events-health-boot1.json`): 13 claimed, 11 sent to the stub, 2 offline,
1 timeout, 14 private replies, 0 failed, 0 unidentified, 2 namespaced lines passed through.
The server chat log of boot 1 has the 7 public lines and none of the 13 claimed ones.

### Linux (`linux-25792439`)

The same set, repeated on the Linux server with the same client (Direct Connect), with its own
staging game server and catalog revisions `L1` → `L2`. Evidence is in the run's `linux/` folder.
Every row matched Windows:
- `.tk ping`, forged `whoami`, quoted arguments, alias, fanout, permission denial and `help` were
  claimed and answered privately (110-150 ms).
- `/tk ping` and `/whoami` stayed in the client.
- `.tk nope 1` and `.tk fan` after its removal went public.
- `slow` timed out privately and its late reply was dropped once.
- With the stub offline, two commands failed privately and were not replayed after recovery.
- After a Takaro kick and rejoin, the identity was the same.

Probe counters: 12 claimed, 0 in Takaro events, the server chat log or Discord
(`linux/logs/events-health.json`, `linux/events-window.json`, `linux/server-chat-log.txt`,
`linux/discord-hook-executed.json`). Caps Lock was on during most of this run, so lines arrived
in upper case (`.TK PING`). That shows the probe matches the prefix and triggers case-insensitively.

The first Linux boot never reached the client. The engine stopped map loading with "Loading is
stuck, flush will never finish" (`AsyncLoading2.cpp`) about two minutes after `MatchStarting`,
on a host under heavy memory pressure. The second boot of the same library loaded the map in
108 s and ran the whole set. The probe adds one hook subscription and nothing on the loading
path. The same code ran three Windows boots without it. So this is treated as environmental,
and kept as evidence (`linux/logs/first-boot-loading-stuck-ConanSandbox.log`).

### Regression smoke (Takaro staging, real client)

On both targets: `player-connected` (`events-window.json`, `events-after-restart.json`, `linux/events-window.json`), `chat-message` (same),
`testReachability` (`testReachability.json`, `testReachability-after-restart.json`),
`getPlayers` (`getPlayers.json`, `getPlayers-after-restart.json`, `linux/getPlayers.json`) and `sendMessage`
(`sendMessage.json`, `sendMessage-after-restart.json`, `shots/17-sendMessage.png`,
`shots/23-after-restart-smoke.png`) all passed with the probe on. Discord: each public player
line produced one GameToDiscord post, and claimed commands produced none
(`discord-hook-executed.json`).

### Defect found and fixed

On Windows `RemoveFileIfExists` reports success for a file that does not exist, so the probe's
late-reply watch logged "late reply dropped" every 100 ms for a minute (`lateDropped: 600`). It
now checks that the file exists first (commit `d830257`). Boot 2 shows one line and
`lateDropped: 1`. The same commit makes a catalog without a prefix fail to parse, instead of
reading it as the text `undefined` (host test).

## What is not proven

- That no **other** client receives a vetoed line. Strong indirect evidence: the RPC
  implementation that logs and relays never ran. A second client was not available.
- A claimed command with a shifted prefix (`!tk`, `@`). The input tool's `type` drops shifted
  characters (`shots/04a-shifted-prefix-typed-as-1tk.png`), so the live catalog used `.tk`.
  Later, with shift held down, `"` and `@` were typed and reached the server like any text
  (`@ping`, quoted args). The probe does not depend on the prefix character.
- A real Takaro command catalog, permission engine or invocation. The stub stands in for all of
  them. A stub receipt is not a Takaro execution.

## Review notes and known probe limits

An independent review of the branch raised these. Fixed after the live run, with no effect on
the results above, because no command arrived during a `slow` answer and the connector itself
stopped claiming a removed trigger:

- The stub answered on its loop thread, so `slow` paused the heartbeat for 7 s. It now answers
  each invocation on its own thread.
- The stub's `help` and dispatch ignored triggers removed from the published catalog. They now
  answer only published triggers.
- On start, the stub moved every inbox file aside, including one written in the 3 s before the
  old heartbeat went stale. It now moves only files older than 5 s.
- The chat hook read the channel and text before the identity check even with the probe off.
  It now reads them only when the probe is on, so the probe-off path is the same as on main.

Accepted as probe limits (production work must solve them):

- When 64 offers are queued, a further known command is still claimed (it must never go
  public) but gets no private reply. A reply cannot be sent from the game thread.
- Private failure replies run on the same worker that checks reply deadlines. A slow
  `SendChat` can delay another command's timeout message.
- Host tests cover parsing, classification, the payload and off-by-default. The enabled
  veto, the queue, deadlines and the file hand-off were proven live only.
- A reply file written more than 60 s after its timeout stays in `outbox/`.

## Proposed follow-up issues (not filed)

1. **Takaro: command catalog and invocation contract for connectors.**
   Takaro sends the connector the domain prefix and the installed triggers and aliases per game
   server, with a revision, and accepts a command invocation that is not a `chat-message`.
   Needs a backend decision on the protocol messages and on keeping fanout, permissions and help
   in Takaro.
2. **conan-exiles: silent Takaro commands in the native connector.**
   Turn the probe into a feature: claim prefix + known-trigger lines in the existing chat hook,
   and send the invocation over the Takaro socket using contract 1. Private failure when Takaro
   is offline, no replay, no `chat-message` for claimed lines. Configurable, off until the
   backend contract lands.
3. **conan-exiles: second-client isolation check for targeted chat.**
   Add a second client account to the live runbook and prove that a vetoed command and a
   targeted `sendMessage` reach only the caller.
4. **docs: Conan players cannot use `/` for Takaro commands.**
   Document that the vanilla client consumes every `/` line, so a Conan domain needs a non-slash
   command prefix (`@` default works). Takaro should warn when a Conan server sits in a domain
   whose prefix starts with `/`.
