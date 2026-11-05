# Native and silent Takaro commands: Valheim

Research for #200 (parent #195). Question: can the Valheim connector offer Takaro commands as a
native client terminal command (`/takaro ping`), with tab suggestions from a replaceable catalog,
forwarded privately through the authenticated companion session, without any of it becoming
chat?

Probe date: 2026-10-22. The probe ships in this branch but is **off unless a marker file exists**
(see [How the probe works](#how-the-probe-works)); without the file no probe code path runs and
the companion protocol behaves exactly as on `main`.

## Rescope against current main

The issue was written against Valheim 1.0.15 (Steam build 25390630) and "protocol v2". On `main`
today:

- The default catalog target is `linux-1.0.20` (Steam server build 25909431, Valheim 1.0.20,
  network version 41). All symbols below were re-read in that build.
- The client companion is the optional inventory companion, protocol 3 (`TakaroCompanionV1`
  routed RPC, hello in a protocol-2 envelope, session nonce + rising sequence, peer-bound). The
  "existing companion session" the issue relies on is this one. It still has no catalog,
  receipt or command-result message; the probe adds three probe-only message types to it.

Everything else in the issue still applies.

## Tested builds

| Component | Exact build |
| --- | --- |
| Valheim dedicated server | Steam app 896660, build 25909431 (Valheim 1.0.20, network 41), catalog target `linux-1.0.20` |
| Valheim client | Steam app 892970, build 25909357 (Valheim 1.0.20, n-41), Windows, BepInExPack Valheim 5.4.2351 |
| Decompiled for source review | `assembly_valheim.dll` from depot 896661 manifest 4001404753685205688 (sha256 `c36b81c1…7812`), `ilspycmd` 9.1 |
| Server connector | `TakaroValheim` built from this branch, product version `4.2.0-probe200.1`, protocol v1, on Takaro staging as a GENERIC server |
| Client companion | `Takaro.Valheim.Companion` from this branch, `4.2.0-probe200.1`, companion protocol 3 |

**Source** below means read in the decompiled 1.0.20 assembly. **Live** means observed with a
real Valheim client joined to a test dedicated server, with Takaro staging as the oracle.

## Go/no-go

| # | Question (acceptance criterion) | Verdict | Experiment result |
| --- | --- | --- | --- |
| 1 | Version, symbols, constructor shape, cache refresh recorded | go | passed (source) |
| 2 | Register one collision-free `/takaro` after terminal init, replacing nothing | go | passed (live) |
| 3 | First-argument tab suggestions from a replaceable catalog; refresh without restart | go | passed (live) |
| 4 | `/takaro ping` invokes once; recognised and unknown `/takaro` input never reaches `Talker.Say`, `Say` or `ChatMessage` | go | passed (live + Takaro) |
| 5 | Offline: private error at once, no public fallback, no replay, no success | go | passed (live) |
| 6 | With a session: bounded invocation, no client identity, "submitted" only after a receipt tied to peer, nonce and id | go | passed (live) |
| 7 | Wrong peer, stale session, reconnect; pending input not replayed; duplicate counts and limits | go | wrong-peer and stale: passed (unit); reconnect and no-replay: passed (live) |
| 8 | Preserve `ping`, `say`, mixed case and mod-owned entries; teardown removes only our object | go | passed (live for `say`, `ping`, mixed case, teardown; unit for mod-owned) |
| 9 | Chat and developer Console contexts, Console behaviour defined | go | passed (live, both contexts) |
| 10 | `/verify` report-only between phases, `/verify` at the end | done | see [Verification](#verification) |
| 11 | Source-proven separated from live and from unverified production assumptions | done | this document |

Overall: **go** for a client-companion command surface on Valheim. It needs the optional
companion; a server-only plugin cannot do any of this (the terminal, tab completion and the
private reply live in the client).

## Source findings (Valheim 1.0.20)

`Version.CurrentVersion` is `1.0.20`, `c_networkVersion` 41.

**Registration.** `Terminal.commands` is `protected static Dictionary<string, Terminal.ConsoleCommand>`.
Both public constructors

```csharp
ConsoleCommand(string command, string description, ConsoleEvent|ConsoleEventFailable action,
    bool isCheat = false, bool isNetwork = false, bool onlyServer = false, bool isSecret = false,
    bool allowInDevBuild = false, bool hideBehindDevCommands = false,
    ConsoleOptionsFetcher optionsFetcher = null, bool alwaysRefreshTabOptions = false,
    bool remoteCommand = false, bool onlyAdmin = false)
```

start with `commands[command.ToLower()] = this;`, i.e. they **overwrite** an existing name
silently. A collision check before construction and object-identity ownership for removal are
mandatory. Built-ins are created once in `private static void InitTerminal()`, guarded by
`private static bool m_terminalInitialized`, on the first `Terminal.Awake()` (the main-menu
`Console`). The dictionary is static and outlives scene changes.

**Dispatch.** `Chat.InputText()` turns `/x` into `x` and anything else into `say …`, then calls
`TryRunCommand(text, silentFail: (bool)this, skipAllowedCheck: false)`. In IL the second argument
is `UnityEngine.Object.op_Implicit(this)`, which is `true` for a live `Chat`. So, unlike the
issue's reading, **an unknown slash command in chat is silent**: no "not a recognized command"
line, no chat, nothing. `TryRunCommand` lower-cases the first token (`/TaKaRo` reaches
`takaro`). `Console` uses the base `Terminal.InputText()`, which echoes the line locally and calls
`TryRunCommand(text)` with `silentFail: false`. `Chat.isAllowedCommand` refuses cheat commands;
the base allows everything. `ConsoleCommand.IsValid` also gates `IsCheat`, `IsNetwork` and
`OnlyServer`; a plain command is valid in both contexts.

**Tab options.** `ConsoleCommand.GetTabOptions()` calls the `optionsFetcher` when the cache is
empty or `alwaysRefreshTabOptions` is set, so a fetcher reading an atomically replaced snapshot
gives live suggestions. `RunAction` also maps `args[1]` case-insensitively onto a tab option
(`/takaro Ping` arrives as `ping`).

**Cache refresh.** Root completion uses the per-instance `private List<string> m_commandList`,
rebuilt by `private void updateCommandList()` only when it is empty. A command added after a
terminal filled that list stays invisible to root Tab until `updateCommandList()` runs again on
that instance (`Chat.instance`, `Console.instance`).

**Second-word lookup is case-sensitive.** In `UpdateInput`, Tab on the second word looks up
`commands[array[0]]` (or `array[0].Substring(1)` for chat) without lower-casing, so `/Takaro p`
+ Tab offers nothing while `/takaro p` + Tab works. Dispatch is still case-insensitive. Native
limitation; not worked around.

**Chat path.** `Chat.SendText` → `Talker.Say` → `ZNetView.InvokeRPC(user, "Say", …)` per
recipient, after an async permission check; shouts send routed `ChatMessage`. Nothing on the
slash path reaches these unless the command itself calls them (`say`, `s`, `w`).

**Native names that matter.** `ping` ("ping server") and `say` ("chat message") are built-ins.

## How the probe works

Marker file `BepInEx/config/takaro-command-probe.txt` on each side; absent means off.

- **Server** (dedicated-server plugin): the file is checked once at plugin load. If present, a
  `CommandProbeServer` re-reads it every 2 s: `catalog = ping, balance` (lower-case
  `[a-z0-9_-]{1,32}`, at most 64) and an optional `receipts = off`. A changed catalog gets a new
  revision and is pushed as `command-probe-catalog {revision, commands}` to every negotiated
  companion session. A `command-probe-invoke {invocationId, command, arguments}` is accepted only
  through the existing session checks (the arrival peer, the tracked peer object and character,
  the session nonce, a rising sequence, the per-peer rate limiter), de-duplicated per session by
  invocation id (last 64), logged, and answered with `command-probe-receipt {invocationId,
  "submitted"}` on that peer's session with a rising server sequence. Nothing is sent to Takaro
  and no event is emitted. Without the file, an invoke envelope is ignored like any unknown type.
- **Client** (inventory companion): if the file exists at startup, the probe looks up
  `Terminal.commands`, `m_terminalInitialized` and `updateCommandList` by reflection and stays off
  if any is missing. Once the terminal is initialised it registers `/takaro` (or `root = <name>`)
  only if the name is free, with a fetcher returning the current session's catalog and
  `alwaysRefreshTabOptions: true`, then refreshes both terminals' command lists. The callback
  parses at most 8 arguments of 64 characters, 256 characters in all, refuses offline (no
  negotiated session), without a catalog or for an unknown command, and otherwise sends one
  invocation and keeps it pending (at most 8, 10 s). A receipt for the same nonce and id prints
  "submitted … Submitted is not executed"; a timeout prints that it was not retried; a session
  reset drops all pending invocations with a private note and never re-sends them. All replies
  are `Terminal.AddString` on the invoking terminal only. `console = on` enables the F5 console
  for the session. Teardown removes the entry only if it is still the same object.
- **Audit** (probe only, patched by hand): per slash/console line it logs how often the probe
  callback ran and how often `Talker.Say` and `Chat.SendText` were called, plus a running count of
  routed `Say`/`ChatMessage` sends.

**Console behaviour (defined).** The Console context behaves exactly like chat: same parsing,
same offline/catalog checks, same session, private replies in the console. The console echoes the
typed line locally (vanilla behaviour); nothing is sent as chat.

## Experiments

All live runs: a real Valheim client on Windows, joined to a test dedicated server whose connector
was identified on Takaro staging. Takaro evidence comes from `EventController.search` on that game
server for 15:35–15:43 UTC. Screenshot and log names in the table refer to the run's evidence files.

| Experiment | How | Result |
| --- | --- | --- |
| E1 Symbols | Decompiled 1.0.20 (`Terminal`, `Chat`, `Console`, `Talker`, `ZRoutedRpc`, `Version`), IL of `Chat.InputText` | passed: as above, including the silent chat fallback |
| E2 Registration | Client log at the main menu | passed: `registered '/takaro' (162 terminal commands now)` |
| E3 Offline | Main-menu console, `takaro ping` | passed: private "not connected … not sent and will not be retried", 1 callback, 0 `Talker.Say` (`feature-offline-console.png`) |
| E4 Submit | In world, `/takaro ping` | passed: server log `accepted invocation c744f55a… receipt sent`, client "'ping' submitted … Submitted is not executed", 1 callback, 0 `Talker.Say`/`SendText` (`feature-ping-submitted-chat.png`) |
| E5 Unknown and bad input | `/takaro nosuch`, `/takaro`, `/takarox ping` | passed: private "unknown command", private usage line, native silent fallback; 0 `Talker.Say` each |
| E6 Mixed case | `/TaKaRo Ping arg1 arg2` (shift chords) | passed: dispatched, `command=ping arguments=2`, submitted |
| E7 Tab | `/takaro ` + Tab | passed: completes to `/takaro balance` from catalog revision 1 (`feature-tab-rev1.png`) |
| E8 Refresh | Server marker changed to add `shop`, no restart | passed: revision 2 pushed and applied in about 1 s; `/takaro s` + Tab → `/takaro shop`, submitted (`feature-tab-rev2-refresh.png`) |
| E9 No receipt | `receipts = off`, `/takaro ping` | passed: server accepted, receipt withheld; client after 10 s "no receipt … not retried" (`feature-no-receipt-timeout.png`) |
| E10 Reconnect, no replay | `receipts = off`, `/takaro balance`, logged out within 10 s, rejoined | passed: client `dropped 1 unconfirmed invocation(s) … none will be replayed`; server saw `aeef5462…` once and never again; new peer, new nonce, catalog re-sent; `/takaro ping` submitted on the new session |
| E11 Console in world | F5, `takaro shop 1 1` | passed: submitted in the console, 0 `Talker.Say` (`feature-console-ingame-submitted.png`) |
| E12 Native names | `/say …`, `/ping` | passed: `/say` became chat (Takaro event `321e2ce7…`, Discord bridge 200); `/ping` printed "Got ping reply from server: 37 ms" (`regression-sendMessage-native-ping.png`) |
| E13 Collision | Client `root = say`, restart | passed: `'/say' already exists (chat message); refusing to replace it. Probe command not registered.` |
| E14 Teardown | Quit from the menu | passed: `'/takaro' removed (it was ours)` |
| E15 Takaro sees no command text | `chat-message`/`command-executed` search | passed: exactly 3 chat-message events (two automatic "I have arrived!" and the `/say` line), no `command-executed`, no Discord post for any `/takaro` line (`mcp-chat-message-events.json`) |
| E16 Wrong peer, stale session, duplicate id | Unit tests (`CommandProbeTests`) | passed: another peer replaying a nonce, an un-negotiated peer, an unknown peer, an old sequence and an old nonce after reconnect are all ignored; a repeated id is counted and not receipted twice; a new session starts a new id window |
| E17 Mod-owned root | Unit test | passed: an existing `takaro` entry is never replaced; teardown leaves an entry another mod installed later |

Regression smoke on the same run (Takaro staging, real client): `player-connected` (events
`8a3d49e8…`, `f3fd6e32…`), `chat-message` (`321e2ce7…`), `testReachability`
(`connectable: true`), `getPlayers` (the joined player), `sendMessage` (shown in the client).
Discord: game → Discord posts 200; Discord → game was not exercised in this run.

**Duplicate callback counts.** Every audited `/takaro` line logged exactly one probe callback
(11 lines, chat and console). No line produced a `Talker.Say` or `Chat.SendText` call, and the
routed `Say`/`ChatMessage` counter only moved for the automatic arrival shout and the `/say` line.

**Not verified.** Gamepad input, other UI languages, a client with Jötunn or another command
mod installed (only simulated by `root = say` and unit tests), more than one connected player,
and Discord → game.

## Production recommendation

1. Keep the companion as the only route. Server-only Valheim cannot register client terminal
   commands or show private replies.
2. Register one root (`/takaro`) only, never per-command aliases: aliases collide with built-ins
   (`ping`, `say`) and other mods, and removing them safely needs per-name ownership tracking.
3. Ship the catalog as a whole, revisioned snapshot per session, replaced by one reference write,
   and offer it through `optionsFetcher` with `alwaysRefreshTabOptions`. Call
   `updateCommandList()` on both terminals after registering; fail closed when any reflected
   member is missing, and re-check these members on every Valheim update.
4. Bind invocations to the existing companion session (peer, nonce, rising sequence, rate
   limit) and give them an id; the server resolves the player. Never carry a player, role or
   permission in the payload.
5. Say "submitted" only after the server's receipt; show the Takaro result only when a
   correlated result exists. Drop pending input on any session change; never replay.
6. Handle unknown subcommands in the probe callback (private reply); unknown roots fall into
   Valheim's silent fallback, which is private but gives the player no feedback.
7. Expose the Console the same as chat.

## Required parent-contract changes (#195)

- A connector-side catalog feed: Takaro → connector snapshot with a revision, effective per game
  server, re-sent on reconnect. The Valheim server would forward it to companion sessions.
- A command invocation message from the connector to Takaro carrying the authenticated player,
  the command, the raw argument text and an invocation id, plus a correlated result back
  (accepted, denied with reason, executed, failed). Today only `chat-message` exists.
- Capability and version negotiation for both, so an older connector or companion keeps the
  configured-prefix route. The companion would also need a capability bit for commands.

## Verification

- `dotnet test games/valheim/mod/Takaro.Valheim.sln` (net8.0, reference-free): 527 passed,
  1 failed. The failure is `SetupScriptPassesDeterministicRetryFallbackAndSafetyHarness`, which
  fails the same way on a clean `origin/main` checkout in the same build container (setup harness
  environment), so it is unrelated.
- Real-assembly builds against the 1.0.20 server assemblies: plugin and companion, `net472`,
  0 errors.
- `/verify --mode=report-only --scope=branch` (light: Codex as the independent reviewer, tests,
  the live run above as the exercise). Codex reported four findings; three were fixed after the
  live run and are covered by unit tests only:
  - removing the server marker now pushes a newer, empty catalog, so connected clients stop
    offering commands instead of timing out;
  - the server refuses (no receipt) a command that is not in its current catalog;
  - an unreadable client marker leaves the probe off instead of rolling back the whole
    companion.
  The fourth (no test runs the game-bound startup with the marker absent) is accepted: the test
  project is reference-free by design; the live run covered startup with the marker present and
  every release build runs without it.
- The live run used build `4.2.0-probe200.1`; the three fixes are in `4.2.0-probe200.2`, which
  was built against the real assemblies but not re-run live.

## Proposed follow-up issues

1. **Valheim: catalog relay from Takaro to companion sessions.** Replace the marker-file stub
   with the parent contract's catalog snapshot: keep the latest revision per game server, push it
   to each negotiated companion session, re-send on reconnect, and drop it when Takaro
   withdraws it.
2. **Valheim: production `/takaro` command in the companion.** Promote the probe to a supported
   capability: a companion capability bit, the registration and teardown rules above, catalog
   tab options, localisation of the private replies, and fail-closed checks on game updates.
3. **Valheim: forward companion invocations to Takaro and show correlated results.** Map an
   accepted invocation to the parent contract's invocation message with the server-resolved
   player, and show the correlated result privately in the invoking terminal; keep "submitted"
   distinct from "executed".
