// Inline detour of UObject::ProcessEvent (build 25639945). The detour drains the game-thread
// queue on the game thread and otherwise falls straight through to the original.
#pragma once

namespace Hook {

// Patches ProcessEvent. Must run before the engine starts threads (library constructor).
// The caller has already checked the build-id and the prologue.
bool Install();

// Calls the original ProcessEvent through the trampoline. Game thread only.
void CallProcessEvent(void* obj, void* func, void* parms);

}  // namespace Hook
