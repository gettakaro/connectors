# Native and silent Takaro commands: Project Zomboid (Build 42)

Research for #201 (parent #195). Question: can the Project Zomboid connector claim ordinary
player slash input for Takaro commands on the server, answer only the player who typed it, fail
privately when Takaro cannot run it, and leave every other slash input exactly as the game
handles it?

Probe date: 2026-10-22. Nothing in this document changes the connector's default behaviour.
The probe code is in [`games/zomboid/research/command-probe/`](../../../games/zomboid/research/command-probe/README.md)
and the agent binds its hook only when `TAKARO_COMMAND_PROBE_URL` is set.

## Rescope

The issue pins build `42.20.4 b0bbce05d5`. The default catalog target is now `linux-42.21.0`
(Steam build 25485538, game 42.21.0 `4a0e9546ec`), so every descriptor, the bytecode reading and
the live probe below are for 42.21.0. `linux-42.20.4` stays in the catalog; its hook targets
were recorded as unchanged against 42.21.0 when that target was added, and the descriptors below
are the same names and shapes the issue cites.

## Tested builds

| Component | Exact build |
| --- | --- |
| Dedicated server | Project Zomboid 42.21.0 `4a0e9546ec`, Steam build 25485538, catalog target `linux-42.21.0`; `projectzomboid.jar` sha256 `e1a69eb7…3a33` (class file v69, Zulu 25.0.1 runtime) |
| Client | Project Zomboid 42.21.0 `4a0e9546ec` (Steam, Windows), account `takarotester`, role `user` (no capabilities, non-admin) |
| Connector | `takaro-zomboid` 1.4.0 (main at `3859fd8`) plus this branch's probe, built as `1.4.0-probe201.1` with `scripts/build-release.sh --target linux-42.21.0`, registered on Takaro staging as GENERIC `game: zomboid` |
| Probe backend | `stub.py` from this branch (not a Takaro protocol) |

## Go/no-go

| # | Question (issue acceptance criterion) | Answer | Experiment result |
| --- | --- | --- | --- |
| 1 | Exact descriptors and control flow for receive, dispatch, reply, logging and native registry | Recorded below | passed (bytecode) |
| 2 | A non-admin's slash command reaches the candidate handler; identity available | **go** | passed (server side, real client) |
| 3 | A claimed command gets one targeted reply: no public echo, no vanilla unknown error, no duplicate, no native/RCON run, no replay | **go** (server side) | passed (server side); what the sender and an observer see is blocked, see below |
| 4 | A recognized command fails privately while the backend is offline | **go** | passed (server side) |
| 5 | Unknown and unowned input behaves as in the uninstrumented game | **go** | passed (server side + uninstrumented client baseline) |
| 6 | Case collisions, malformed args, denial, timeout, catalog replacement, add/change/remove | **go** | passed (live + unit tests) |
| 7 | Backend multi-install fanout survives the game adapter | **go** | passed (live) |
| 8 | Selective interception or native registration | **Selective interception.** Native registration: **no-go** | decided from bytecode |
| 9 | Production boundary (authorization, identity, idempotency, outcomes, timeouts, private delivery, help) | Defined below | n/a |

"Server side" means the evidence is the agent log, the probe's receipts, the stub's receipts and
Takaro's event store; all of it came from input typed in a real client. During the probed session
the client's renderer froze (byte-identical frames, Esc ignored, while it still sent input), and
two relaunches stalled on a white window, so there is no screenshot of the probed replies on the
client yet. The uninstrumented baseline from the same client, minutes earlier, shows the same
reply path (`sendMessageToServerChat(connection, …)`) arriving in the client's chat. The issue's
two-client observer check was not possible: only one client account exists on the test server.

## Source findings (42.21.0 bytecode)

Read with `games/zomboid/scripts/dump-signatures.py` (`--code` disassembly) on the pinned jar.

### Client side

`media/lua/client/Chat/ISChat.lua`, `ISChat:onCommandEntered`: input that starts with one of the
chat streams `/say /s /yell /y /whisper /w /faction /f /safehouse /sh /all /admin /a` (followed by
a space) is chat and never reaches the command path. Any other input that starts with `/` goes to
`SendCommandToServer(command)` (packet `ReceiveCommand`, id 111, required capability
`LoginOnServer`, so every logged-in player can send it) and is stored only in the local chat
history. The client does not echo the command into chat.

### Receive and dispatch

```
static void GameServer.receiveReceiveCommand(ByteBufferReader, UdpConnection, short)
  s = bb.getUTF()                                  // "/name args…"
  r = handleClientCommand(s.substring(1), conn)    // private static String (String, UdpConnection)
  if r == null: r = handleServerCommand(s.substring(1), conn)   // private static String (String, UdpConnection)
  if r == null: r = "Unknown command " + s
  if s.substring(1) startsWith "roll" or "card":
      ChatServer.getInstance().sendMessageToServerChat(r)          // every player
  else:
      ChatServer.getInstance().sendMessageToServerChat(conn, r)    // only this connection
```

- `handleClientCommand` tokenizes with `([^"]\S*|".*?")\s*`, lower-cases the first token and
  answers `card`, `roll`, `changepwd`, `dragons`, `dance` and `safehouse`; anything else returns
  `null`.
- `handleServerCommand` looks the name up with `CommandBase.findCommandCls(String)`, instantiates
  it with `(username, role, command, connection)` and returns `CommandBase.Execute()`. A role
  without the command's capability gets `Player "<user>" has not right to execute "<cmd>"
  command`; an unknown name ends in `Unknown command <input>` (the trailing concat in
  `handleServerCommand`). `GameServer.rcon(String)` is `handleServerCommand(cmd, null)`.
- The packet handler runs on the server main thread (the probe logged
  `HOOK CONFIRMED receiveReceiveCommand thread=main`).
- **Public-reply trap:** the reply to any input whose name starts with `roll` or `card` (for
  example `/rollcall`, `/cards`) is broadcast to every player, whatever produced it. A Takaro
  command with such a name must not be answered through this method.

### Reply and logging

- `ChatServer.sendMessageToServerChat(UdpConnection, String)` builds a server message
  (`ServerChat.createServerMessage(String, boolean)`) and calls
  `ServerChat.sendMessageToPlayer(UdpConnection, ChatMessage)` → `ChatBase.sendChatMessageToPlayer`.
  It never goes through `ChatServer.sendMessage(ChatMessage)`, which is the connector's
  `chat-message` hook, so neither a vanilla reply nor a probe reply becomes a Takaro chat event.
- Neither `receiveReceiveCommand` nor the dispatch logs the input. In the baseline,
  `/nosuchcmd401`, `/players`, `/tprobe` and `/help` left no line in `*_chat.txt`, `*_cmd.txt`,
  `*_admin.txt` or `*_user.txt`. Individual admin commands log to the admin log when they run.

### Native registry

`CommandBase.childrenClasses` is a `private static final Class[]` filled in `<clinit>` with 67
hard-coded command classes; `findCommandCls` scans it and matches the `@CommandName` annotation
with a case-insensitive regex. There is no registration API, and adding a command means replacing
a static final array or transforming `findCommandCls`. That is interception in disguise with more
version risk, so native registration is a no-go. Selective interception before dispatch is the
seam.

## How the probe works

- Hook: `ReceiveCommandAdvice` on `GameServer.receiveReceiveCommand` (static, 3 args),
  `skipOn = OnNonDefaultValue`, one `Bridge` call. `CommandProbeRuntime` reads the string with
  `getUTF()`; when the name is not in the catalog it puts the buffer position back, returns
  `false`, and the game reads the same bytes and runs unchanged. When it is in the catalog the
  method is skipped: no `handleClientCommand`, no `handleServerCommand`, no "Unknown command",
  no `roll`/`card` broadcast.
- Catalog: `GET /catalog` every 2 s; an immutable snapshot keyed by lower-case name, replaced
  atomically. A failed poll marks the backend offline and keeps the last snapshot, so known
  names still fail privately. Before the first catalog nothing is claimed. Names are refused when
  they are not one token (`^[A-Za-z0-9_]{1,32}$`), are a client chat stream, a
  `handleClientCommand` word, start with `roll`/`card`, or match a native command
  (`CommandBase.findCommandCls`).
- Invocation: on a worker thread, `POST /invoke` with a 3 s timeout, carrying the name, the
  arguments (split like vanilla; an open quote is malformed), every matching installation and
  the caller. The answer is delivered once on the main thread with
  `sendMessageToServerChat(connection, text)`, after checking the connection is still connected.
  A timeout or dropped socket is answered "Takaro did not answer. Your command may or may not
  have run." and is never retried. Receipts are written to `command-probe-receipts.jsonl`.
- Guards: a connection with more than one local player is refused privately ("cannot tell which
  local player sent that command") and nothing is sent; the worker pool is bounded (4 threads,
  16 queued) and a full pool fails the command privately instead of queueing it; a catalog that
  does not parse counts as offline; the catalog is applied on the main thread because the
  native-name check reads the game's command registry; the agent log records only the command
  name, never its arguments (native `/changepwd` carries a password).

## Experiments

The file names below are the raw logs, receipts and screenshots kept with the test run; they
are not committed (SteamID, IP and connection GUID redacted). Timeline in `probe-timeline.txt` (UTC), agent lines in `probe-agent-log.txt`
(container time, UTC+2).

### E0 Uninstrumented baseline: passed

Release-line jar without the probe, same client, 2026-10-22 ~19:00Z. Client chat shows
`Unknown command nosuchcmd401 a b`, `Player "takarotester" has not right to execute "players"
command`, `Unknown command tprobe hello`, `Unknown command TPROBE x`, `Unknown command takaro tprobe`;
`/help` lists every command to a non-admin; `/roll 6` without dice answers the dice hint.
No server log line for any of them (`baseline-unprobed-replies.png`, `baseline-unknown-command.png`,
`baseline-help-and-roll.png`).

### E1 Hook binding and identity: passed

`target-check` ok for `linux-42.21.0`, `hooks installed` with no unbound matcher, first
invocation `command-probe: HOOK CONFIRMED receiveReceiveCommand thread=main`. Every receipt
carries `username=takarotester`, the Steam ID, `role=user`, `localPlayers=1` and the connection
GUID, all read from the receiving `UdpConnection`. The packet carries no local-player index: on a
split-screen connection (`UdpConnection.players[]` holds up to 4) the sender cannot be told
apart, so production should resolve identity per connection (username of player 0) and treat a
connection with more than one local player as ambiguous.

### E2 Claimed command, one private reply: passed (server side)

`/tprobe hello "two words"` → `CLAIMED`, stub got exactly one invocation with
`args=["hello","two words"]`, one reply `[install-a] tprobe ran for takarotester …` sent to that
connection only. No `Unknown command` (vanilla dispatch skipped), no native command (the name is
not native and dispatch never ran), no RCON. Takaro stored no `chat-message` event and the
Discord chat bridge posted nothing for any of the 16 inputs (`takaro-events-during-probe.json`;
the same window has the `player-connected` event and its Discord post). Client-side screenshot:
blocked (render freeze).

### E3 Offline backend: passed (server side)

Stub switched to `offline` (catalog and invoke answer 503). The probe logged
`backend OFFLINE (catalog revision 2 kept)`; `/tprobe off` was claimed and answered "Takaro is
unavailable right now. Your command was not run."; the stub received nothing. Back `online`,
`/tprobe back` ran once; the offline invocation was not replayed (stub receipts skip seq 9).

### E4 Unknown and unowned input: passed

With the probe on: `/nosuchcmd401 a b`, `/players` (native, no rights), `/tdeny` after it left the
catalog and `/nosuchcmd401 off` were `passed to game`; the stub saw none of them. The game read
the restored buffer and dispatched normally (same path as E0). A client-side comparison
screenshot of these replies under the probe is blocked with the client.

### E5 Collisions, malformed, denial, timeout, catalog changes: passed

- Case: `/TPrObE x` matched `tprobe` (receipt `command=TPrObE`, one invocation).
- Catalog revision 2 had `tprobe`/install-a, `TProbe`/install-b, `tnew`, plus `players`,
  `rollcall` and `all`. The probe logged
  `refused=[players: native server command, rollcall: reply would be public, all: client chat stream]`.
- Malformed `/tprobe "open` → private "a quote is not closed", no backend call.
- Denial: `/tdeny` → stub `denied` → private "You may not run tdeny."
- Timeout: stub `slow` (answers at 6 s) → private unknown-outcome reply at 3 s; `drop` (socket
  closed) → same reply; each reached the stub once, nothing retried.
- Add/change/remove: after revision 2, `/tnew` ran, `/tdeny` went back to the game, `/tprobe`
  fanned out to two installations.
- Concurrent replacement: unit test `inFlightInvocationKeepsTheSnapshotItWasClaimedWith` replaces
  the catalog while an invocation is in flight; the invocation keeps its snapshot and the next
  input sees the new one. 15 probe unit tests pass (`unit-tests.log`), including the malformed
  catalog, split-screen refusal and full-pool cases.

### E6 Fanout: passed

`/tprobe fan` with two catalog rows for the same name sent one invocation listing
`["install-a","install-b"]`; the stub answered for both and the player got one line with both
results. The adapter groups rows by name but never drops one.

### Build used for each experiment

E1 to E6 ran on `1.4.0-probe201.1`. Review then added the guards listed under "How the probe
works" (split-screen refusal, bounded pool, malformed catalog as offline, main-thread catalog
apply, name-only logging); `1.4.0-probe201.2` carries them. They are covered by unit tests; the
live re-run of E2 to E6 on `probe201.2` is blocked with the client.

## Production boundary

- **Seam:** selective interception at `GameServer.receiveReceiveCommand` entry, peek and restore
  the buffer, skip only for claimed names. Re-pin the descriptor on every game update, like the
  existing hooks.
- **Ownership:** exact, case-insensitive names from Takaro's catalog snapshot. Never claim client
  chat streams, `handleClientCommand` words, names starting with `roll`/`card`, or native
  commands; report refused names back to Takaro so the operator sees why a command is not
  reachable as `/name`.
- **Authorization:** Takaro's permission model alone. The game role is sent as context only;
  requiring a PZ capability would duplicate Takaro roles and would not exist for most catalog
  commands.
- **Identity:** the receiving connection's username and Steam ID (the same `gameId` and
  `platformId` the connector already reports). Ambiguous split-screen connections should be
  refused, not guessed.
- **Idempotency and outcomes:** one invocation id per claimed input; structured outcomes
  `ok | denied | invalid | unavailable | unknown`. `unavailable` is only for "not sent";
  anything sent and unanswered is `unknown`, never retried by the game side.
- **Timeouts:** a bounded wait (the probe used 3 s) off the main thread; the reply goes back
  through the main-thread queue and is dropped if the connection is gone.
- **Private delivery:** `ChatServer.sendMessageToServerChat(UdpConnection, String)`, which does
  not create a Takaro chat event and is not written to the chat log.
- **Help and autocomplete:** none needed for execution. Vanilla `/help` lists only native
  commands; a server-side `/takaro help` (or a catalog-generated help line) is enough for a first
  increment. Autocomplete would need a client Lua mod, which the server cannot install for
  players.

## Residual version risk

`receiveReceiveCommand`'s body and the `roll`/`card` broadcast branch are game code that can
change in any build; the hook needs the same per-build re-pin as the other hooks. The client's
chat-stream list lives in client Lua and can change without a server update.

## Proposed follow-up issues

1. **zomboid: claim Takaro catalog commands at `receiveReceiveCommand`**
   Promote the probe's hook and name rules into the connector behind the parent protocol's
   command catalog and command-invoke messages.
   Includes refused-name reporting and per-build re-pin notes.
2. **zomboid: private command outcomes over the command-invoke protocol**
   Map `ok/denied/invalid/unavailable/unknown` to one private line per invocation, with the
   bounded wait and no game-side retry.
   Prove with a real client that the reply appears only for the sender.
3. **zomboid: two-client observer proof for private command replies**
   Add a second ordinary client account to the test server and repeat E2–E4 with an observer.
   Confirms no public echo from the observer's own chat.
