// Inline detour of UObject::ProcessEvent on Windows (MinHook, which relocates RIP-relative
// instructions in the stolen prologue). The detour drains the game-thread queue on the game thread
// and otherwise falls straight through to the original. Mirrors platform/linux/hook.h.
#pragma once

#include <cstdint>
#include <string>

namespace Hook {

// The UE game thread is the process's main thread; DllMain (a static import) runs on it.
void SetGameThreadId(unsigned long tid);

// Detours ProcessEvent at `target` (from the pins). Safe while the engine runs: MinHook suspends the
// other threads while it patches.
bool Install(uintptr_t target, std::string& error);

// Calls the original ProcessEvent through the trampoline. Game thread only.
void CallProcessEvent(void* obj, void* func, void* parms);

// Re-pin evidence: copies up to `max` distinct UFunction pointers (the first 64) that passed through
// the detour, and the number of calls seen while that sample was filling.
size_t SampledFunctions(uintptr_t* out, size_t max);
uint64_t CallCount();

}  // namespace Hook
