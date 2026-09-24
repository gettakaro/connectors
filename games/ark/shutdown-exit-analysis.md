# ARK build 21241282 shutdown path

Target: Linux `ShooterGameServer`, SHA-256
`7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520`.
These addresses are valid only for that exact non-PIE executable.

The reflected `DoExit` thunk at `0x15C40E0` invokes native `0xD91900`.
After its cheat authorization check, native `DoExit` calls `GEngine` vtable
slot `+0x290` with the UTF-32 `Exit` literal at `0x42686CC`. The engine slot
targets `0x262EEB0` in the exact-build vtables at `0x405A700` and
`0x442EEB0`. The engine handler matches `EXIT`/`QUIT` at `0x262EF18–4A`,
calls shutdown handler `0x262F340`, and returns `AL=1` (handled).

At `0x262F49A–A1`, the shutdown handler passes `EDI=0` to `0x1B03A90`.
That routine sets exit-request byte `0x5938B26` when the argument is false.
The main loop tests the byte at `0x8206A0`; when set, it calls `0x820260`
at `0x8206C3`. That shutdown function passes `EDI=1` to `0x1B03A90`
at `0x82029F–A4`. The true branch of `0x1B03A90` calls `abort@plt` at
`0x1B03AA2`. A normal requested shutdown therefore reaches SIGABRT /
exit status 134 in this build. The later instructions in `0x820260` are
unreachable after that call.

This static path explains why status 134 alone does not prove an abnormal
failure. It does not attribute any specific historical run without a marker
showing which path ran. It also does not prove save persistence: save-world
completion and preserved save files must be checked before and after a
controlled restart. An HTTP shutdown response must flush before scheduling
the native exit request on a later game tick, because the main loop can terminate
after the exit-request byte is set.

The game's own save path emits `Saving world...` (UTF-32 literal `0x4068414`)
at `0xE64909–0xE6495D` before serialization. It emits `World Save Complete.
Took` (literal `0x40684F8`) at `0xE65A2A–0xE65A55`, after its serialization
loops and a pending-write counter wait at `0xE6599D–0xE659FC`. A shutdown
gate can wait off the game thread for a **fresh** completion line observed
after its `SaveWorld` request, then verify the expected save file. A stale
line, timeout, or file-check failure must leave the server running. The line
establishes that this native save path reached its completion log; it alone
does not prove `fsync` durability or distinguish concurrent save requests.

The game-mode `SaveWorld` virtual slot `+0xD80` resolves to `0xE64830`
(vtable entry `0x4063D40`) and has ABI `void(GameMode*, bool forceWait)`;
CheatManager's reflected `SaveWorld` route at `0x15C8BE0` calls native
`0xD91710`, which passes `ESI=0` to that virtual slot. The native function
has only two early branches to its epilogue: it skips saving if byte
`GameMode+0xCF8` is nonzero or int32 `GameMode+0x73C` differs from `-1`.
Otherwise it performs the save, waits for pending writes, logs completion,
then returns. It has no meaningful boolean return value. A direct call on a
freshly validated game mode, with both guards checked before the call and a
fresh save-file check after return, can establish completion without relying
on the game log. The first whole-instruction entry detour span is 20 bytes:
`55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 48 02 00 00`.

During read-only inspection of the controlled running server, process
`ShooterGameServer` had FD 42 open to
`/server/ShooterGame/Saved/Logs/ShooterGame.log`, but its size and file offset
were both zero. A file-tail completion marker cannot work in that run; a
handled `GEngine::Exec("SaveWorld")` result alone does not prove this native
game-mode method ran. A live integration must either prove the Exec call path
or use the validated game-mode virtual path and preserve the save file.

Offline inspection used `objdump -d -M intel` on this exact executable around
`0xD91900`, `0x1B03A90`, `0x262EEB0`, `0x262F340`, `0x8205D0–0x820700`,
and `0xE64900–0xE65A60`.

## Implemented shutdown and container requirement

The connector calls the guarded `RequestExit(false)` entry directly on the
game thread. It checks the complete 23-byte function signature and the unset
exit-request byte before staging shutdown. It first completes synchronous
`SaveWorld`, then waits for the HTTP acknowledgement to finish sending and
at least 200 ms before requesting exit. While shutdown is pending, the action
dispatcher does not execute subsequent queued mutations. Console aliases
that bypass this sequence are rejected in favor of the dedicated shutdown
action. No signal handlers are replaced.

Run containerized ARK with Docker `--init` or Compose `init: true`. The game
must not be PID 1. The game's normal termination path uses `abort`, and Linux
PID-namespace init signal behavior changes its result when ARK is PID 1.
Both the development rig and maintenance verifier enable an init process.

Earlier isolated runs without init exited 139. Their final instruction was
inside `SDL_HasClipboardText` called by `EngineCrashHandler`; this was a
secondary fault, not evidence that normal shutdown cleanup required SDL.
The saved original signal context instead pointed to the fallback `HLT`
in libc `abort`. The same immutable runtime image reproduced the distinction
with a small abort probe: PID 1 exited 139, while running under init exited
134. Retesting the unchanged connector package under init resolved the
failure; the connector does not force exit or patch the crash handler.

The final implementation at source
`a25e3a30892b7dc6751d28e7c907f3a1f99a5585` passed an isolated two-boot check:
both native shutdowns acknowledged, completed synchronous saves and emitted
fresh exit-request markers before status 134. Between them, the second game
boot opened and read the exact owned world file, as observed with inotify;
its hash was unchanged across loading, and the native boot identity changed.
This establishes the tested save/reload sequence. It does not establish all
real-client acceptance obligations or storage durability after power loss.
