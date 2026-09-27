# Exact-build zero-player world discovery

This is an offline binding record for the non-PIE Linux `ShooterGameServer`
build 21241282, executable SHA-256
`7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520`.
It supports `mod/src/world_bootstrap.hpp`; a live server still must prove that
the active world is discoverable before any player joins.

The engine pointer is held at executable address `0x59958F8`. The instruction
at `0x81FF53` is `48 8b 3d 9e 59 17 05` (`mov 0x517599e(%rip),%rdi`), whose
RIP target is that slot. The native world-context lookup at `0x2933F90`
reads the `UEngine` context count from `+0x928` and array pointer from
`+0x920`; its loop at `0x2933FB0` compares the candidate world against the
context's `+0x260` world pointer. The game-context creation call site at
`0x263010D` moves `1` into `esi` before calling `0x292EF10`, which writes
the `WorldType` value into the context. The helper accepts only type `1`
(`Game`), one eligible world, an actual `UWorld` class, and a ready
`ShooterGameMode` at world `+0x250`. A zero-length world `+0x488` player
array is valid; no controller or PostLogin callback is needed for discovery.

Exact callable entry points are `UWorld::StaticClass` `0x2D1E730`,
`ShooterGameMode::StaticClass` `0x13CCDA0`, weak key creation `0x1D56970`,
and generic weak resolution `0x1D56C70`. The first instructions at those
entries, as well as the engine reference, context lookup, and game-context
call site, are checked byte-for-byte before the helper calls or reads through
them. The getter `0x2D1E730` loads class cache `0x59A7E88` and on a miss
registers UTF-32 `World` under `/Script/Engine`. A read-only live memory
check found that cache equal to the zero-player world object's `ClassPrivate`
pointer. An earlier candidate `0x2D1DF50` was wrong: it registers the
`StartPhysicsTickFunc` property, whose cache is `0x59A7E40`, and correctly
failed the class check without dispatching any action. The helper bounds
context count/capacity, verifies class inheritance,
creates a weak index/serial, cross-checks that key against the GUObjectArray,
and resolves it back to the same world. A missing/transient context, stale
weak key, wrong thread, changed instruction, or two distinct eligible worlds
fails closed. Native integration retains the weak key and resolves it again
for each later action.

The offline evidence can be reproduced against the exact executable using
`objdump -d` for `0x81FF53`, `0x2933F90`, `0x263010D`, `0x2D1E730`,
`0x13CCDA0`, `0x1D56970`, and `0x1D56C70`. The focused helper test is
`mod/tests/world_bootstrap_test.cpp`; GCC 14, C++20, `-O2 -Wall -Wextra
-Werror` passed for empty roster, stale identity, wrong thread/prologue,
transient world, and ambiguous worlds. That synthetic test checks fail-closed
logic; it does not establish a live pre-PostLogin world or a persistent ban
after restart.
