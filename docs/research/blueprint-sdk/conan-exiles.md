# Blueprint SDK for other mods: Conan Exiles

Research question: can the native Conan Exiles connector expose Takaro to other server mods
through Blueprint nodes, in an optional "TakaroSDK" mod? The SDK would offer
`RunCommandAsPlayer`, `SendCustomEvent`, `GetTakaroPlayerId` and an `OnTakaroReply` event. The
example use is a quest or visual-scripting mod that runs a Takaro command silently on behalf of
a player.

Probe date: 2026-10-31. The probe code is behind an environment variable and does nothing unless
it is set. Nothing ships to users from this work.

## Answer

**Conditional go.**

- **Takaro side:** a silent run-as-player works today, with no Takaro core change (live).
- **Blueprint side:** the SDK mod cannot use the two paths that need no cooked mod. Direct
  library calls are invisible to the connector. The console route needs Conan's admin flag,
  which a Blueprint cannot set.
- **What it must use instead:** a dispatcher or timer handler, which goes through
  `ProcessEvent`, or a request array that the connector polls. Proving either one needs a cooked
  test mod, built with the Conan Exiles Enhanced Dev Kit. That part is not done yet.

## Tested builds

| Component | Exact build |
| --- | --- |
| Conan Exiles Dedicated Server | Steam app 443030 build 25792439, Windows Shipping exe (`windows-25792439`), RCON off |
| Client | Conan Exiles Enhanced (Steam app 440900) revision #379,176/0, vanilla, empty modlist |
| Connector | `takaro-conan-exiles` 3.6.0 (`games/conan-exiles/version.txt`) with the probe from this branch (`winmm.dll` sha256 `fad1e65f…`). A later probe-only change (the lag estimate now matches the newest stand-in call) is not in the tested binary and does not affect any result here |
| Takaro | Staging, one GENERIC game server reporting `game: conan-exiles`, `gameVersion: 25792439`; modules `utils` and a chat-bridge fork with Discord |

## Go/no-go

| # | Question | Verdict | Evidence |
| --- | --- | --- | --- |
| 1 | A Blueprint calling a native library function (a plain "Takaro Run Command" node that wraps one) reaches the connector's `ProcessEvent` hook | **no-go** | live oracle (below); not tested with a cooked SDK mod |
| 2 | Dispatcher `Broadcast` to a handler on the SDK actor reaches the hook | expected go, **not tested** | engine design; needs a cooked mod |
| 3 | `Set Timer by Event` target reaches the hook | expected go, **not tested** | engine design; needs a cooked mod |
| 4 | Custom event on the SDK actor, called directly from another Blueprint | expected no-go, **not tested** | engine design (direct call, like 1) |
| 5 | `ExecuteConsoleCommand("takaro …")` intercepted from the server log | **go only in an admin context**; no-go for a mod | live |
| 6 | Connector polls a reflected request array on the SDK actor | expected go, **not tested** | design; needs a cooked mod |
| 7 | Takaro runs a command as the player, silently, with a private reply | **go** | live, two routes |
| 8 | Custom event reaches Takaro | **go** as a `log` hook; Takaro has no custom event type | live |
| 9 | `OnTakaroReply` back into a Blueprint | expected go, **not tested** | same mechanism as the connector's chat send; needs a cooked mod |
| 10 | `GetTakaroPlayerId` | **go**, no round trip | the Takaro `gameId` is the Steam64 from `PlayerState.UniqueID` (`conan/identity.h`) |

## How the probe works

`games/conan-exiles/native/core/conan/blueprint_probe.{h,cpp}` does nothing unless
`TAKARO_CONAN_BLUEPRINT_PROBE` names a directory. A host test checks that it is off by default.
It has three parts:

- **Console route.** A worker thread tails `ConanSandbox.log`. A line
  `Command not recognized: takaro run <steam64> <text>` (or
  `takaro event <steam64|-> <name> <json>`) becomes a request file for a stub. The stub's replies
  go to the player privately. This part costs the game thread nothing.
- **Stand-in driver.** Until a cooked mod exists, the probe calls
  `KismetSystemLibrary.ExecuteConsoleCommand` on the game thread with exactly the arguments a
  Blueprint node would pass:
  - the player's controller, with or without Conan's admin flag (`pc-admin`, `pc`);
  - no player at all (`none`).

  This stands in for the Blueprint call. It is not one.
- **ProcessEvent oracle.** It adds counting subscriptions on three groups of functions:
  - native library functions that game Blueprints call constantly;
  - native→Blueprint events, as positive controls;
  - the SDK mod's own function names, which stay unresolved until that mod is loaded.

The stub (`games/conan-exiles/research/blueprint-probe/stub.py`) stands in for a Takaro
"invoke command" message. It calls `POST /command/{gameServerId}/trigger` with a staging API key
that it reads from standard input. The connector never holds that key.

## Experiments

All rows ran on the Windows server with the real client joined. Evidence file names refer to the
run's evidence folder, which is kept with the test run and not in this repository.

### Transport: what reaches the connector

| Experiment | Result |
| --- | --- |
| Oracle: 9 native library functions (`IsValid`, `GetPlayerController`, `GetAllActorsOfClass`, `Delay`, `K2_SetTimerDelegate`, `PrintString`, `RandomFloatInRange`, `Concat_StrStr`, `ExecuteConsoleCommand`) vs native→Blueprint events | Library functions: **0 hits** in 15 minutes with a player online. Positive controls: `Actor.ReceiveBeginPlay` 35,749 hits over 431 Blueprint overrides, `ActorComponent.ReceiveBeginPlay` 139,160. A direct Blueprint call into a native function runs the native thunk and skips `ProcessEvent`. So a plain library node is invisible to the hook. The connector's own calls go through the trampoline, so they do not count either. |
| `ExecuteConsoleCommand("takaro run <steam64> @ping")` from the player's controller **with** the admin flag | The engine logged `Command not recognized: takaro run … @ping`. The probe read it and handed it on. |
| Same call from the player's controller **without** the admin flag | **Nothing logged.** Conan drops console commands from non-admin controllers silently. |
| Same call with no `SpecificPlayer` (nobody online, and with a player online) | Nothing logged. With a player online the engine falls back to the first player's controller, which is not an admin. |
| Can a Blueprint set the admin flag? | No. `ConanPlayerController.m_IsAdmin` has property flags `0x48001040000220`: private, and `CPF_BlueprintVisible` is not set. |

So the console route works only where an admin context already exists, for example the
connector's own `executeConsoleCommand` action. It is not a route for an SDK mod.

### Takaro end: silent run as player

| Experiment | Result |
| --- | --- |
| Route A: probe → stub → `POST /command/{gs}/trigger {playerId, msg:"@ping"}` | `command-executed` (ping) for the calling player. Takaro's own private `Pong!` reached only that player, 0.9 s after the log line. |
| Route B: a Takaro module hook on `eventType: "log"` that calls `takaro.command.commandControllerTrigger` | Works **with no stub and no Takaro change**. The connector already sends server log lines as `log` events, and Takaro hooks can match them with a regex. Result: `command-executed` for the right player and a private `Pong!` about 12 s after the log line (the staging command queue; the hook itself ran after 3 s). |
| Custom event as `takaro event <steam64> questDone {"quest":"q1","step":3}` | The hook received the name and the JSON and answered privately 1.7 s later. Takaro's event API has no custom event name, so this is a hook on a `log` line, not a stored event. |
| Silence | No player `chat-message` for any SDK-run command. Only Takaro's own whispers appear, as player-less `chat-message`, and the chat-bridge fork skips those, so there was no Discord post. |
| Identity on the wire | The whisper recipient carries `gameId`, `steamId` and `platformId` `steam:<id>`. The `command-executed` player is the caller. |

### Security finding (fixed in the research hook)

The first route-B hook matched `Command not recognized: takaro (run|event) ` anywhere in the
line. Conan logs public chat as `ChatWindow: Character <name> (…) said: <text>`. A player who
typed `Command not recognized: takaro event <steam64> forged` in global chat triggered the hook.
With `run` and another player's Steam64, the same text runs Takaro commands as that player.

The fix anchors the hook regex to the start of the log line:
`^\[[0-9.:-]*\]\[ *[0-9]*\]Command not recognized: takaro (run|event) |^Command not recognized: takaro (run|event) `.
Takaro's regex check refuses nested quantifiers, hence the alternation. Retest: the same chat
text was ignored, and the engine's own console line still ran the hook.

Any log-hook design must anchor its regex. It must also use a marker that no player-controlled
text can produce at the start of a line.

### Regression and cost

- **Probe off** (the production path): `player-connected`, public `@ping` → `command-executed` →
  `Pong!` are unchanged.
- **Mod-side `ExecuteConsoleCommand`** costs 0.30–0.38 ms of game thread per call. The first call
  after boot took 15 ms. On the connector side the console route costs no game-thread time.
- **Unresolved hook subscriptions are not free.** `HookDispatch` rescans the object array on a
  backoff while any subscription is unresolved: up to 30 s apart early, then 120 s. Each scan is
  about 15 ms of game thread in 0.3 ms slices.
  - With the probe's SDK names subscribed and no SDK mod loaded, the resolver kept scanning
    (26+ scans in 15 minutes).
  - With the probe off it stopped after 5 scans. In comparable 10 s windows the connector used
    30 µs per tick with the probe off, against 340 µs with it on.
  - Production must subscribe to SDK functions only when the SDK mod is in the server's modlist
    (or its class is loaded).

## Recommended design (not built)

1. **SDK mod (optional, opt-in).**
   - A `BP_TakaroSDK` actor spawned by the mod controller.
   - A `BPL_Takaro` library whose nodes call a dispatcher on that actor:
     `OnTakaroRequest(Verb, Player, Payload)`.
   - The actor binds an empty handler event `TakaroRequestHandler` to the dispatcher.
   - The broadcast reaches `ProcessEvent`. The connector subscribes Before on
     `BP_TakaroSDK_C.TakaroRequestHandler`, reads the parameters and enqueues. Cost: one hook hit.
   - The connector takes the Steam64 from the `Player` controller itself (`rx::Steam64Of`), never
     from a mod-supplied string.
   - Fallback if the dispatcher is not visible: a request array on the actor, which the connector
     polls at 2 Hz or less. It finds the actor through its `ReceiveBeginPlay`, which is
     `ProcessEvent`-visible.
2. **Connector → Takaro (interim, no core change).** The connector sends each request as a `log`
   event with a fixed marker at the start of the line, for example
   `TakaroSDK: run <steam64> <text>`. A Takaro module hook anchored on that marker runs the
   command (route B, proven). Players cannot produce a line that starts with the marker.
3. **Connector → Takaro (target).** A connector-protocol "invoke command as player" message plus a
   custom event type, as proposed in #195. That needs a Takaro backend decision.
4. **Return path.** `OnTakaroReply` is a Blueprint event on the SDK actor. The connector calls it
   through `ProcessEvent`, the same way it sends chat. A Takaro module could address it without a
   protocol change by sending `executeConsoleCommand "takarosdk reply <id> <json>"`. The connector
   would intercept that prefix in its own action instead of running it. Not tested.

## Mod ecosystem notes

- **Pippi / Mushi.** `[Enhanced] Pippi - User & Server Management` v4.0.6 (Workshop 3725018456) is
  **live**: 44,033 current subscribers, last updated 11 Jun.
  - The "removed" and "incompatible with Conan Exiles Enhanced" texts on its page are hidden
    templates (`display:none`) that every Workshop page carries. They are not the item's status.
  - Pippi has a command system and Mushi visual scripting (Condition, Action, Chat Message,
    Webhook Message, Give Kit and other nodes). It is closed source and documents no extension
    point for other mods, so a Takaro node in Mushi needs its author.
- **Client side (desk research, not verified).** Conan clients download the server's modlist from
  the Workshop and must match it, so the SDK mod would be on players' modlists too. It stays
  optional: only servers that already run mods would add it. The connector itself stays vanilla.
- **Maintenance (desk research, not verified).** Every Dev Kit update can force a re-cook. The
  current server build already refuses mods cooked with an older Dev Kit ("Mod is too old"). The
  earlier Takaro mod was cooked for Windows, WindowsServer and LinuxServer from one project.
- **Dev Kit.** The Conan Exiles Enhanced Dev Kit is distributed through the Epic Games Store
  ("Conan Exiles Enhanced Dev Kit", free, Epic account required). Whether its current version
  matches server build 25792439 was not checked.

## What is not proven

- Rows 2, 3, 4, 6 and 9: they need a cooked SDK mod and a consumer test mod. None was built,
  because the Dev Kit was not installed.
- The Linux server target. The SDK mod would be server content and the hook code is shared, but
  nothing ran on Linux in this research.
- Isolation from a second client: one client only, as in #205.

## Proposed follow-up issues (not filed)

1. **conan-exiles: cooked TakaroSDK test mod.** Build `BP_TakaroSDK` and `BPL_Takaro` plus a
   consumer test mod with the Enhanced Dev Kit. Prove rows 2, 3, 4, 6 and 9 with the probe's
   oracle, and measure the per-request game-thread cost.
2. **conan-exiles: subscribe to SDK hooks only when the SDK mod is loaded.** Avoids the resolver
   rescans measured above.
3. **Takaro: invoke command as player, and custom events, from connectors.** A follow-up to #195.
   Until it exists, document the anchored `log` hook pattern and its injection risk.
