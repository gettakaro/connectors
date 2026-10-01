// Generic hooking primitives: vtable slot swaps (mprotect + restore) and the shared
// resolve/hook/fire registry that feeds /health.diagnostics.resolved.
//
// We only swap vtable slots. Inline detours are deliberately not implemented: the binary is
// non-PIE with full symbols, so every interesting entry point is reachable through a vtable or a
// direct call, and a slot swap is trivially reversible.
#pragma once
#include "common.h"

#include <atomic>

namespace Hooks {

// Registry used by /health. `how` explains where the address came from.
void RecordResolve(const std::string& name, uint64_t addr, const std::string& how, bool hooked);
void MarkHooked(const std::string& name, bool hooked);
void MarkFired(const std::string& name);  // takes the registry lock: cold paths only
// Hot paths (the Tick detour) resolve their counter ONCE at install time and then bump it with a
// relaxed atomic add. The returned pointer stays valid for the process's life (the registry is a
// std::map, whose nodes never move).
std::atomic<uint64_t>* FiredCounter(const std::string& name);
std::string ResolvedJson();               // merged symbol table + hook state

// Swaps one slot of a vtable in place. `slot` is the C++ vtable index (slot 0 = first virtual
// function, i.e. the word right after the two header words the _ZTV symbol points at).
// Returns false with `err` set; never crashes.
bool SwapVTableSlot(const std::string& name, void* ztvSymbolAddr, size_t slot, void* detour, void** origOut,
                    std::string& err);
// Same, but resolves the vtable through .dynsym (e.g. "_ZTV7UObject").
bool HookVTableSymbol(const std::string& name, const char* ztvName, size_t slot, void* detour, void** origOut,
                      std::string& err);
// Convenience: hooks the UObject::ProcessEvent slot of a class vtable (slot index from sym.cpp).
bool HookProcessEvent(const std::string& name, const char* ztvName, void* detour, void** origOut, std::string& err);
// Swaps a slot on a *live object's* vtable (the object's own vtable pointer, not the class symbol).
bool HookObjectVTable(const std::string& name, void* obj, size_t slot, void* detour, void** origOut, std::string& err);

// Puts every swapped slot back. Called from the destructor path; safe to call twice.
void RestoreAll();
size_t InstalledCount();

}  // namespace Hooks
