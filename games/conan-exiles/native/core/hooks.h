// UFunction hooks: lets core code run a small callback on the game thread whenever the engine
// calls one specific UFunction through UObject::ProcessEvent (BlueprintEvents such as
// GameModeBase.K2_PostLogin pass through it). The platform detour calls Dispatch() before the
// original ProcessEvent, on the game thread only.
//
// Rules for a hook body (it runs inside the engine's own call): a few memory reads and a
// lock-free or tiny-mutex enqueue. No I/O, no JSON, no waiting, no nested GameThread::Run.
// Shared by every lane that needs a game event (bans: K2_PostLogin; events: login, logout,
// chat, death).
#pragma once

#include <atomic>
#include <cstdint>

namespace Hooks {

using Fn = void (*)(void* obj, void* func, void* parms);

constexpr int kMaxHooks = 32;

// Any thread. Registers `fn` to run before every ProcessEvent call of `ufunction`. The same
// (ufunction, fn) pair is stored once. Returns false when the table is full.
bool Add(void* ufunction, Fn fn);
// Any thread. Removes the pair; a Dispatch already running may still call it once.
void Remove(void* ufunction, Fn fn);
// Number of registered hooks (cheap check for the detour's hot path).
extern std::atomic<int> g_count;
// Game thread, from the detour: runs every hook registered for `func`.
void Dispatch(void* obj, void* func, void* parms);

}  // namespace Hooks
