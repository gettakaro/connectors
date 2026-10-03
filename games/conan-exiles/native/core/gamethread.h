// Runs work on the game thread. The ProcessEvent detour calls Drain() on the game thread; any
// other thread queues a job with Run() and blocks until it has run or timed out.
//
// Policy (as in games/vein/mod/docs/gamethread-policy.md): only engine calls and memory reads
// that the engine requires go on the game thread. No sockets, JSON, disk or logging I/O.
#pragma once

#include <atomic>
#include <functional>

namespace GameThread {

// Cheap check for the detour's hot path.
extern std::atomic<bool> g_pending;

// Queues `fn` and waits up to `timeoutMs`. Returns true if `fn` ran. A job that has already
// started is always waited for, so `fn` may safely capture the caller's stack.
bool Run(std::function<void()> fn, int timeoutMs);

// Game thread only. Runs queued jobs within a small budget (4 jobs or 500 us).
void Drain();

// Fails every queued and future Run() immediately (shutdown).
void Stop();

}  // namespace GameThread
