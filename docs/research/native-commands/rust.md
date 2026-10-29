# Native and silent Takaro commands: Rust (Carbon and Oxide)

Research for #197 (parent #195). Question: can the Rust connector expose catalog-provided
Takaro commands as native chat commands on Carbon and Oxide, silently, with the caller's
identity taken from the framework, and with safe runtime and offline behaviour?

Probe date: 2026-10-22. Nothing in this document changes the connector. The probe code is in
[`games/rust/research/command-probe/`](../../../games/rust/research/command-probe/README.md)
and does nothing unless it is configured.

## Tested builds

| Component | Exact build |
| --- | --- |
| Rust Dedicated Server | Steam build 25902425 (current public branch on 2026-10-22), protocol `2634.289.1`, changeset 167781, build date 2026-10-13. Catalog target `carbon-25902425`. |
| Carbon | 2.0.262.0 `production_build`, commit `8a81d704288f191b7c4017f74dc284f744e220ac` (2026-10-02) |
| Oxide | Oxide.Rust 2.0.7830 (release `2.0.7830`, commit `8c6518b8b49be63f640238fca2b098e618fbdbb2`, 2026-10-13), Oxide.Core 2.0.4143, Oxide.CSharp 2.0.4149; `Oxide.Rust-linux.zip` sha256 `a5e9de373b9ee6c6d03232c90c95caf24c40749e4ea40ef6b2a6d8cdaa854cd4` |
| Connector on the Carbon server | `TakaroConnector` 0.3.0 with protocol v1 (the #546 branch build `0.3.0-dev.5194999`), registered on Takaro staging as GENERIC, `game: rust` |
| Probe | `TakaroCommandProbe` 0.0.1 and `TakaroCollisionFixture` 0.0.1 from this branch |

The issue's source review used Oxide.Rust `ca69c156` (2026-09-17) and Carbon `4d1b081e`
(2026-09-11). This review re-read the same seams at the installed commits above; the cited
lines are unchanged in substance.

Servers: the Carbon server is the repository's Rust dev server (`server.secure 0`, connector identified
on Takaro staging). The Oxide server is a disposable second server on the same Rust build,
with the Oxide.Rust release files laid over the vanilla `Managed` folder, and no connector.

**Source** below means read in the framework source at the commit above. **Live** means
observed on a running server, with the evidence file named.

## Go/no-go

| # | Question | Carbon | Oxide | Experiment result |
| --- | --- | --- | --- | --- |
| 1 | Register catalog commands at runtime, through one callback, with the caller from `BasePlayer` | go (source) | go (source) | blocked: live client |
| 2 | Caller-only response; no chat event, no public chat, no bridge post | go (source) | go (source) | blocked: live client |
| 3 | Authorized, denied, offline, disconnect and player-disconnect outcomes; no replay | go (source) | go (source) | blocked: live client |
| 4 | Catalog add/remove without restart; reload/unload cleanup | go | go | passed (server side) |
| 5 | Foreign duplicate roots: preflight, skip, fallback | go, with a load-order caveat | go only with preflight | passed (server side) |
| 6 | Discovery, help, typo and autocomplete in the vanilla client | not observed | not observed | blocked: live client |

## Evidence matrix

Confidence per area, after this probe:

| Area | Carbon | Oxide | Basis |
| --- | --- | --- | --- |
| Execution | Medium: source only for invocation | Medium: source only for invocation | Source: both frameworks dispatch registered chat commands before normal chat, with the caller as `BasePlayer`. Live: the probe compiles and registers its roots on both, and catalog callbacks run on the main thread. No invocation, caller identity or private reply has been observed yet. |
| Suppression | Medium-high (source) | Medium-high (source) | Source: prefixed input never reaches `OnPlayerChat` and is never broadcast on either framework; `OnPlayerCommand`/`OnUserCommand` listeners still see it. Live check with the connector and Discord bridge is blocked. |
| Discovery | Low | Low | Source: no portable help or autocomplete; Carbon typo suggestions only for auth level 2+. Client behaviour not observed. |
| Runtime | High | High | Live: catalog add/remove, reload, unload, both duplicate orders, read back from the framework's own registry |

## How the probe works

- A local stub (`stub/stub.mjs`) stands in for Takaro: `GET /catalog` returns
  `{revision, commands:[{name}]}`, `POST /invoke` records a receipt line and answers
  `ok`/`denied`, and `POST /control` switches between `ok`, `deny`, `drop` (destroy the socket
  mid-request) and `slow` (answer late) or replaces the catalog. It is not a Takaro protocol.
- The probe polls the catalog every 2 s with the framework's `webrequest` library, one request
  at a time, and ignores a revision older than the one it has. The catalog callbacks were
  observed on the game main thread (their log lines carry `thread=main`; the probe compares the
  managed thread id with the one captured in `Init`). Invocation callbacks log the same field,
  but no invocation has run yet. Names that are not one typable token (`^[a-z0-9_]{1,32}$`)
  are ignored with a warning.
- A new catalog revision is reconciled: removed names are released with
  `RemoveChatCommand(name, this)`, added names are claimed only when the framework's registry
  shows no other owner. `/takaro <name> [args]` is the namespaced fallback, claimed under the
  same rule; when another plugin holds `/takaro` the probe logs that there is no fallback
  route. Every poll also re-checks ownership: a root another plugin took over is dropped, a
  root that became free is claimed. Every root routes to one callback,
  `OnInvoke(BasePlayer, string, string[])`.
- The caller is `player.UserIDString` from the framework's callback. The invocation body is
  built from that player, and the reply always goes to that player with `player.ChatMessage`.
  The stub's `ok` answer carries a spoofed `caller`, which the probe ignores.
- With the backend offline (last catalog poll failed), a recognized command is answered
  privately with "Takaro is unavailable right now. Your command was not run." and is not sent.
  The backend is offline until a valid catalog arrives, so a malformed catalog counts as
  offline. When a sent request fails, times out or is dropped, Takaro may already have run
  it, so the private answer says the outcome is unknown ("Takaro did not answer. Your command
  may or may not have run"). Nothing is queued or retried.
- `takaroprobe.status <name> ...` (RCON) reads who the framework says owns each root: Carbon
  `CommandManager.Contains(Chat, ...)`, Oxide `Command.chatCommands` and the Covalence
  `registeredCommands` through reflection.

## Source findings

### Oxide 2.0.7830

- `RustCore.IOnPlayerChat` checks `CommandHandler.GetChatCommandPrefix(message)` first. Prefixed
  input goes to `TryRunPlayerCommand` and the hook returns `false`, so the message is never
  broadcast and `OnPlayerChat`/`OnUserChat` are never called for it (`src/RustHooks.cs` 340-368).
- `TryRunPlayerCommand` calls `OnPlayerCommand` and `OnUserCommand` before dispatch; a non-null
  return blocks the command (`src/RustHooks.cs` 378-415). A plugin listening there sees the raw
  command text, which matters for bridges that log commands.
- Dispatch order: Covalence commands first (`Covalence.CommandSystem.HandleChatMessage`), then
  `Command.HandleChatCommand`. A Covalence command with the same name shadows a chat command.
  An unknown command gets the "Unknown command" reply only when Oxide's `Modded` option is on.
- `Command.AddChatCommand` replaces an existing non-core chat command with only a warning
  ("X has replaced the 'name' chat command previously registered by Y"), and also unregisters
  a same-named Covalence command (`src/Libraries/Command.cs` 131-171). There is no public
  "is this name taken" API; `chatCommands` is `internal`.
- `RemoveChatCommand(name, plugin)` only removes an entry owned by that plugin, but removing a
  name that replaced a foreign command does not restore the foreign one: the foreign callback
  was overwritten in the dictionary. Unload calls the same removal for every command the plugin
  owns (`plugin_OnRemovedFromManager`).
- `ChatCommand` has no help or description field. Nothing in Oxide sends chat command names to
  the client.

### Carbon 2.0.262 (8a81d70)

- The `IOnPlayerChat` Harmony patch checks `Command.HasPrefix` first and calls
  `CorePlugin.IOnPlayerCommand`, which always returns `false` for prefixed input, known or
  unknown. `IOnPlayerChat` (and so `OnPlayerChat`) is not reached for it
  (`IOnPlayerChat.cs` 53-67, `Core.Hooks.Commands.cs` 97-182).
- `IOnPlayerCommand` calls `OnUserCommand` (BasePlayer and IPlayer forms) and `OnPlayerCommand`
  before dispatch; any non-null return blocks it.
- The prefix config can echo the command back: `PrintToChat` sends "Command: /x" to the caller
  only, `PrintToConsole` writes it to the server console. Both are `false` by default and on the
  tested server.
- Unknown commands: with `authLevel >= SuggestionAuthLevel` (default 2, so admins only) Carbon
  replies with typo suggestions from the registered chat commands; everyone else gets a plain
  "unknown command" line. Both go only to the caller.
- `CommandManager.RegisterCommand` refuses an existing name ("Command 'x' already exists.") and
  logs a warning; the first owner keeps it. `RemoveChatCommand(name, plugin)` clears only
  commands with that owner, and plugin unload clears every command the plugin owns
  (`ModLoader.cs` 814). `CommandManager.Contains(Chat, name, out cmd)` is public, so a preflight
  needs no reflection.
- Carbon chat commands can carry help text, permissions, groups, auth level and cooldowns
  locally. The portable Oxide-compatible call does not use them, so Takaro authorization stays
  at the invocation boundary, as the issue proposes.

## Experiments

The file names below are the raw logs and screenshots kept with the test run; they are not
committed.

### E4 Catalog add/remove and reload/unload: passed (server side, both frameworks)

Catalog revision 1 `[probe, probedeny]` was replaced with revision 2 `[probe, probetwo]` while
both servers ran. Both probes logged `added=[probetwo] removed=[probedeny]` on the main thread
within one poll, and the framework registry then read `/probedeny owner=free`,
`/probe owner=probe`, `/probetwo owner=probe` (`logs/reconcile-add-remove.log`,
`logs/reload-and-status.log`).

Hot reload on file change and `c.reload` / `oxide.reload`: the new instance's preflight found
every previously held root free (`preflightOwner=none`), which shows both frameworks removed
the old instance's commands on unload. One difference: when an update fails to compile, Oxide
keeps the old instance running with its commands, while Carbon has already unloaded it, so
the commands are gone until a good build loads (`logs/reload-and-status.log` and the first
two compile failures before it).

### E5 Foreign duplicate root: passed (server side, both frameworks)

`TakaroCollisionFixture` owns `/probedup`. Catalog revision 3 added `probedup`.

| Case | Carbon | Oxide | Evidence |
| --- | --- | --- | --- |
| Preflight on, fixture first | `conflict root=/probedup owner=...TakaroCollisionFixture: skipped`; fixture keeps the root | same | `logs/duplicate-preflight.log` |
| Preflight off (`ForceRegisterConflicts`), fixture first | framework refuses: `Command 'probedup' already exists.`; fixture keeps it | framework replaces: `TakaroCommandProbe has replaced the 'probedup' chat command previously registered by TakaroCollisionFixture`; registry owner becomes the probe | `logs/duplicate-force.log` |
| Oxide, the probe that displaced it unloads | | `/probedup` is then **free**: the fixture's command is lost until the fixture reloads | `logs/duplicate-oxide-after-unload.log` |
| Takaro first, foreign plugin loads later | the fixture's registration is refused (`already exists`) and the fixture loads without its command | the fixture replaces the probe's root; the probe's ownership check logs `root=/probedup was taken over by TakaroCollisionFixture; fallback /takaro probedup still works` | `logs/duplicate-takaro-first.log`, `logs/duplicate-late-foreign.log` |

After the review fixes (`logs/reload-after-review-fixes.log`, `logs/reclaim-and-name-validation.log`):
on Oxide, with the catalog unchanged, unloading the fixture freed `/probedup` and the probe
claimed it on the next poll. Catalog names `bad name` and `/slash` were ignored with a warning
on both frameworks.

Preflight capability: Carbon has a public API (`CommandManager.Contains`). Oxide needs
reflection on internal fields (`Command.chatCommands`, `RustCore.Covalence.CommandSystem.registeredCommands`).
It works on 2.0.7830, but nothing guarantees those internals across Oxide releases.

### E1-E3, E6 Live client experiments: blocked

These need a vanilla Rust client on the Windows test machine. On 2026-10-22 the client would not start:
Easy Anti-Cheat refused with "An incompatible Easy Anti-Cheat driver version is already
running", because another game on the same test machine held an EAC driver
(`shots/client-00-menu.png`, `shots/client-02-eac-retry.png`). Starting `RustClient.exe`
without the EAC launcher (both servers run `server.secure 0`) needs its install path, which
could not be found remotely. None of the cells below is counted as passed.

Prepared runs, with the stub and both probes left in place:

| Experiment | Steps | Pass condition |
| --- | --- | --- |
| E1 execution | `/probe a "b c"` and `/takaro probe a "b c"` on each server | stub receipt has `command=probe`, the args as typed and `caller.steamId` = the client's SteamID64; the probe log shows the same id on the main thread |
| E1 spoofing | `/probe caller=76561190000000000` | receipt caller is still the client's SteamID64; the reply goes to the client although the stub answers with a spoofed `caller` |
| E2 suppression (Carbon, with the connector on staging and the Discord bridge) | `/probe`, then a plain chat line as control | the control produces a `chat-message` event and a Discord post; `/probe` produces neither, and the fixture sink logs `OnPlayerCommand` but no `OnPlayerChat`; the server log has no `[CHAT]` line for it |
| E2 suppression (Oxide) | same, with the fixture sink as the bridge stand-in | as above, without Takaro |
| E3 outcomes | stub `ok`, `/probedeny`, stub stopped, stub `drop`, stub `slow` with `delayMs` 4000 (under the probe's 5 s timeout) then disconnect the client within those 4 s | private reply / private denial / private "unavailable" without a receipt / private "may or may not have run" after a receipt with `dropped`, and no second receipt after the stub returns / `reply dropped: caller ... disconnected` and nothing in chat |
| E5 live | `/probedup` and `/takaro probedup` with the fixture owning `/probedup` | fixture answers `/probedup`, Takaro answers `/takaro probedup` |
| E6 discovery | type `/pro` and Tab; `/help`; `/prbe` (typo) as non-admin and as admin; F1 console autocomplete | record exactly what the client shows; promise nothing that was not seen |


## Production recommendation

Go for a production slice on both frameworks, on these terms. The live client cells above
must still be run before that slice ships; this recommendation rests on source review plus
the server-side runtime experiments.

1. **Always offer one namespaced root, `/takaro <command> [args]`**. It needs one root instead
   of one per command, so there is one name to preflight and keep. It is not immune: on Oxide a
   later plugin can still replace `/takaro`, and when another plugin already holds it there is
   no route at all. Make the root name configurable and report its ownership like any other.
2. **Direct roots only when free, and opt-in per command.** Preflight every root:
   Carbon `CommandManager.Contains(Chat, name, out cmd)`, Oxide `Command.chatCommands` plus the
   Covalence `registeredCommands` (reflection). If the Oxide preflight cannot read those fields,
   claim no direct roots at all, because Oxide's `AddChatCommand` silently replaces a foreign
   command and never gives it back.
3. **Re-check ownership on every catalog poll, the namespace root included** (Oxide: a later plugin replaces ours) and report
   conflicts, not just log them. On Carbon a held root makes a foreign plugin that loads later
   fail to register its command, so a direct root can displace a foreign command purely through
   load order. Claim direct roots only after `OnServerInitialized`, and prefer direct roots for
   names unlikely to be used by other plugins; the operator chooses.
4. **Take the caller from `BasePlayer.UserIDString` in the framework callback**, never from the
   message or the backend answer. Reply with `player.ChatMessage` only.
5. **Offline: answer privately and drop.** Keep registrations while Takaro is unreachable so a
   recognized command still fails privately. Never queue, never replay, never broadcast.
6. **Authorization stays in Takaro.** Do not use Carbon's local permission metadata; Oxide has
   no equivalent in the portable call.
7. **Do not promise discovery.** Neither framework sends chat command names to the vanilla
   client. Carbon's typo suggestions are admin-only by default.
8. **Keep the existing `OnPlayerChat` path unchanged.** Prefixed input never reaches it on
   either framework, so the connector's `chat-message` event and chat bridges built on it stay
   silent for commands without extra filtering. Plugins that log `OnPlayerCommand` or
   `OnUserCommand`, and Carbon's `PrintToChat`/`PrintToConsole` prefix options, can still
   expose command text; document both for operators.


## Required parent-contract changes

- A catalog message pushed by Takaro (not polled), with a revision, each effective enabled
  command's name and aliases, and a flag per name saying whether a direct root is wanted. The
  connector reconciles on every new revision and on every reconnect.
- An invocation request from the connector: `invocationId` (unique, so Takaro can refuse a
  duplicate), `command`, `args` as the argument array the framework's callback received.
  Both callbacks only receive already tokenised `string[]` arguments, and the two frameworks
  tokenise quotes with different parsers (Oxide `ParseCommand`, Carbon
  `ConsoleArgEx.TryParseCommand`), so the same input may split differently. Recovering the raw
  text would need the `OnPlayerCommand`/`OnUserCommand` hooks; that is not shown here,
  and E1 should record how quoted arguments arrive on each framework, the `catalogRevision` it was matched against, and the caller's `gameId`
  that the connector derived itself. Takaro must bind it to the authenticated game server and
  must not accept a caller from anywhere else.
- A result: `ok`, `denied`, `unknown` or `error`, plus private reply text for the caller.
  "Takaro unavailable" is decided by the connector and is never sent.
- Delivery: at most once. The connector never retries an invocation; Takaro treats a repeated
  `invocationId` as already handled.
- Capabilities reported separately: `commands.execute`, `commands.silent` (input never becomes
  chat), `commands.liveCatalog`, `commands.discovery` (false for Rust).
- Conflict reporting from the connector: per root, the foreign owner and whether the
  namespaced fallback is in use, so the operator sees it in Takaro.
- A configurable namespace root name (default `takaro`), in case a server already has one.


## Proposed follow-up issues

1. **Rust: native Takaro chat commands (Carbon and Oxide)**
   Implement the recommendation above in `TakaroConnector.cs` behind the parent's catalog and
   invocation messages: namespaced root, opt-in free direct roots with preflight, ownership
   re-check, private offline failure. Unit-test the reconcile and the routing; live-prove on both frameworks.
2. **Rust: finish the #197 live probe with a real client**
   Run E1-E3, E5 and E6 from `docs/research/native-commands/rust.md` on Carbon and Oxide once a
   Rust client can start on the test machine (EAC conflict), and update the go/no-go table with what
   the client showed. No connector change.
3. **Rust: report command-root conflicts to Takaro**
   Send the connector's root ownership (claimed, skipped with foreign owner, taken over) to
   Takaro so operators can see why a command only works as `/takaro <name>`. Depends on the
   parent's conflict-reporting field.

