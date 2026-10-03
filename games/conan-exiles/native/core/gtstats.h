// Game-thread cost accounting for health (perf A/B: idle, one player, Takaro outage).
//
// The detour, GameThread::Drain and HookDispatch::Invoke add their game-thread time here with
// relaxed atomics; the detour itself counts calls in a thread-local and times one call in 1024
// (its own overhead only: the pending check and the hook-table probe, plus the clock reads, so the
// estimate is an upper bound). A sampler (conan/perf_sampler.h) reads the engine frame counter
// every 10 s and calls RecordSample(); health then reports the last window and the totals as
// game-thread microseconds per engine tick.
#pragma once

#include <cstdint>
#include <string>

namespace GtStats {

constexpr uint32_t kDetourSampleMask = 1023;  // one timed detour call per 1024

// ---- game thread (hot paths) ----
void AddDetourCalls(uint64_t calls);  // in chunks of kDetourSampleMask + 1
void AddDetourSample(uint64_t ns);
void AddDrain(uint64_t ns, uint32_t jobs);
void AddHook(uint64_t ns);

// ---- snapshots ----
struct Snap {
    uint64_t monoNs = 0;
    int64_t frames = -1;  // engine frame counter (GFrameCounter), -1 = unknown
    uint64_t detourCalls = 0, detourSamples = 0, detourSampleNs = 0;
    uint64_t drainNs = 0, drainJobs = 0, hookNs = 0, hookCalls = 0;
    uint64_t maxDrainNs = 0;  // largest single drain inside the window ending at this snapshot
};

struct Window {
    double seconds = 0;
    int64_t frames = -1;  // -1 = unknown
    uint64_t detourCalls = 0;
    double hotPathUs = 0, drainUs = 0, hookUs = 0, totalUs = 0;
    uint64_t drainJobs = 0, hookCalls = 0;
    double maxDrainUs = 0;
    double usPerTick = -1;  // -1 = unknown (no frame count)
    double ticksPerSecond = -1;
};

// Pure: the cost between two snapshots `a` (earlier) and `b`. A zero-initialised `a` gives the
// totals since the library loaded (frames then = b.frames, the engine's own count).
Window Compute(const Snap& a, const Snap& b);
std::string WindowJson(const Window& w);

// Takes a snapshot of the counters now with `frames` (-1 when unknown) and starts a new window.
void RecordSample(int64_t frames, uint64_t monoNs);
// { sampler, window: {...} | null, sinceStart: {...} | null, maxDrainUs, samples }
std::string HealthJson();
void SetSamplerState(const std::string& state);

// Host tests only.
void ResetForTests();

}  // namespace GtStats
