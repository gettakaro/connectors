// Game-thread cost counters (QueryPerformanceCounter) around the work this plugin adds inside the game's
// own calls: the session-tick, updatePlayers and moderation-system queue drains and the log-sink hook.
// Recording is lock-free and allocation-free; statistics are computed off-thread for /health
// diagnostics.perf (P5 A/B: avg / p99 / max microseconds per entry, entries and jobs per second).
#pragma once
#include <cstdint>
#include <string>

enum PerfSite { kPerfTick = 0, kPerfUpdatePlayers, kPerfModeration, kPerfLogSink, kPerfSiteCount };

uint64_t PerfNow();                                           // QPC ticks
void PerfRecord(PerfSite site, uint64_t startTicks, uint32_t jobs);  // one entry, measured from startTicks to now
std::string PerfJson(bool reset = false);

struct PerfScope {
    explicit PerfScope(PerfSite s) : site(s), start(PerfNow()) {}
    ~PerfScope() { PerfRecord(site, start, jobs); }
    PerfSite site;
    uint64_t start;
    uint32_t jobs = 0;
};
