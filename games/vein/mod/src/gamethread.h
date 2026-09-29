// Game-thread pump.
//
// The engine object's Tick is hooked through its class vtable (exported as _ZTV14UJgxGameEngine /
// _ZTV11UGameEngine). The detour calls the original first, then drains at most kJobsPerTick queued
// jobs. UObject work runs there; native action and diagnostic workers enqueue owned jobs.
// Networking, JSON, filesystem work and regex matching stay on background workers.
#pragma once
#include "common.h"

#include <functional>

namespace GameThread {

static const size_t kJobsPerTick = 16;

// Installs the Tick hook. Safe to call before the engine exists (the vtable is static data).
void Init();

// Runs `fn` on the game thread and waits. Returns false on timeout or when the pump is unavailable;
// on timeout the job is cancelled if it has not started, otherwise it is left to finish.
bool Run(std::function<void()> fn, uint32_t timeoutMs = 5000);
// Convenience for handlers that produce a JSON string.
bool RunJson(std::function<std::string()> fn, std::string& out, uint32_t timeoutMs = 5000);

bool Alive();          // a tick was seen in the last 5 s
uint64_t TickCount();
std::string StatsJson();

#ifdef TAKARO_GAMETHREAD_TEST
// Test harness runs the same production queue without a VEIN engine binary.
void TestEnable();
void TestPumpOnce();
size_t TestQueued();
#endif

}  // namespace GameThread
