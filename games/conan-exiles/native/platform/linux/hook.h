// Inline detour of UObject::ProcessEvent. The detour drains the game-thread queue on the game
// thread and otherwise falls straight through to the original.
#pragma once

#include <cstdint>
#include <string>

namespace Hook {

// Patches ProcessEvent at `target` (from the pins). Must run before the engine starts threads
// (library constructor). Refuses a prologue other than pins::kProcessEventPrologue.
bool Install(uintptr_t target, std::string& error);

// Calls the original ProcessEvent through the trampoline. Game thread only.
void CallProcessEvent(void* obj, void* func, void* parms);

}  // namespace Hook
