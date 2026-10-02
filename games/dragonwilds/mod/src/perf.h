// Game-thread performance counters.
//
// Tester's rule (docs/gamethread-policy.md): the game thread does only what UE forces there.
// This module is how that rule is *measured* rather than asserted. Everything here is lock-free on
// the hot paths: the tick ring has a single writer (the game thread), the filter counters are plain
// atomics, and the only allocation happens in Json(), which runs on an HTTP thread.
#pragma once
#include "common.h"

#include <cstdint>

namespace Perf {

// CLOCK_MONOTONIC nanoseconds. ~20 ns through the vDSO; cheap enough for a per-call hot path.
uint64_t NowNs();

// One engine tick: `ns` is the wall time our Tick detour spent *after* the original returned,
// `jobs` is how many queued jobs it drained.
void RecordTick(uint64_t ns, size_t jobs, bool budgetHit);

// One ProcessEvent call through our detour: `ns` is the time spent deciding whether we care,
// measured up to (not including) the handler. `hit` is true when a candidate matched.
void RecordFilter(uint64_t ns, bool hit, bool cacheHit);
// Time spent inside an event handler we chose to run (chat / death dispatch).
void RecordHandler(uint64_t ns);

// Named periodic work on the game thread (sweeps, snapshots, catalogue builds). `name` must be a
// string literal: it is stored by pointer, never copied.
void RecordSweep(const char* name, uint64_t ns);

// One entry into the game thread from off-thread work (a queued job). Counted separately from
// ticks so "entries per second" can be reported on its own - the number Tester's rule is about.
void RecordEntry();

std::string Json();   // /health.diagnostics.perf and GET /debug/perf
void Reset();         // GET /debug/perf?reset=1

// Scoped timer for a named sweep.
struct Scope {
    const char* name;
    uint64_t t0;
    explicit Scope(const char* n) : name(n), t0(NowNs()) {}
    ~Scope() { RecordSweep(name, NowNs() - t0); }
};

}  // namespace Perf
