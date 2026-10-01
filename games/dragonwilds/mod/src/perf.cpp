#include "perf.h"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace {

const size_t kRing = 1000;

// Tick ring: single writer (the game thread), many readers. A reader may tear one sample while the
// pump overwrites it; at 1000 samples that cannot move an average or a p99 meaningfully, and it
// keeps the writer at one relaxed store.
std::atomic<uint32_t> g_ring[kRing];
std::atomic<uint64_t> g_ringPos{0};

std::atomic<uint64_t> g_ticks{0}, g_tickNs{0}, g_tickMaxNs{0}, g_budgetHits{0};
std::atomic<uint64_t> g_jobs{0}, g_entries{0};
std::atomic<uint64_t> g_filterCalls{0}, g_filterNs{0}, g_filterHits{0}, g_filterCacheHits{0}, g_filterMaxNs{0};
std::atomic<uint64_t> g_handlerCalls{0}, g_handlerNs{0}, g_handlerMaxNs{0};

struct Sweep {
    const char* name;
    std::atomic<uint64_t> calls{0}, ns{0}, maxNs{0};
};
const size_t kSweeps = 24;
Sweep g_sweeps[kSweeps];
std::atomic<size_t> g_sweepCount{0};
Mutex g_sweepLock;  // only taken the first time a given name is seen

std::atomic<uint64_t> g_sinceMs{0};

void Bump(std::atomic<uint64_t>& max, uint64_t v) {
    uint64_t cur = max.load(std::memory_order_relaxed);
    while (v > cur && !max.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

Sweep* SweepFor(const char* name) {
    size_t n = g_sweepCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; i++)
        if (g_sweeps[i].name == name) return &g_sweeps[i];
    Guard g(g_sweepLock);
    n = g_sweepCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; i++)
        if (g_sweeps[i].name == name) return &g_sweeps[i];
    if (n >= kSweeps) return nullptr;
    g_sweeps[n].name = name;
    g_sweepCount.store(n + 1, std::memory_order_release);
    return &g_sweeps[n];
}

double Rate(uint64_t count, uint64_t sinceMs) {
    uint64_t dt = NowMs() - sinceMs;
    return dt ? (double)count * 1000.0 / (double)dt : 0.0;
}

}  // namespace

uint64_t Perf::NowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void Perf::RecordTick(uint64_t ns, size_t jobs, bool budgetHit) {
    uint64_t pos = g_ringPos.fetch_add(1, std::memory_order_relaxed);
    g_ring[pos % kRing].store(ns > 0xffffffffull ? 0xffffffffu : (uint32_t)ns, std::memory_order_relaxed);
    g_ticks.fetch_add(1, std::memory_order_relaxed);
    g_tickNs.fetch_add(ns, std::memory_order_relaxed);
    Bump(g_tickMaxNs, ns);
    if (jobs) g_jobs.fetch_add(jobs, std::memory_order_relaxed);
    if (budgetHit) g_budgetHits.fetch_add(1, std::memory_order_relaxed);
}

void Perf::RecordFilter(uint64_t ns, bool hit, bool cacheHit) {
    g_filterCalls.fetch_add(1, std::memory_order_relaxed);
    g_filterNs.fetch_add(ns, std::memory_order_relaxed);
    Bump(g_filterMaxNs, ns);
    if (hit) g_filterHits.fetch_add(1, std::memory_order_relaxed);
    if (cacheHit) g_filterCacheHits.fetch_add(1, std::memory_order_relaxed);
}

void Perf::RecordHandler(uint64_t ns) {
    g_handlerCalls.fetch_add(1, std::memory_order_relaxed);
    g_handlerNs.fetch_add(ns, std::memory_order_relaxed);
    Bump(g_handlerMaxNs, ns);
}

void Perf::RecordSweep(const char* name, uint64_t ns) {
    Sweep* s = SweepFor(name);
    if (!s) return;
    s->calls.fetch_add(1, std::memory_order_relaxed);
    s->ns.fetch_add(ns, std::memory_order_relaxed);
    Bump(s->maxNs, ns);
}

void Perf::RecordEntry() { g_entries.fetch_add(1, std::memory_order_relaxed); }

void Perf::Reset() {
    for (size_t i = 0; i < kRing; i++) g_ring[i].store(0, std::memory_order_relaxed);
    g_ringPos = 0;
    g_ticks = 0; g_tickNs = 0; g_tickMaxNs = 0; g_budgetHits = 0;
    g_jobs = 0; g_entries = 0;
    g_filterCalls = 0; g_filterNs = 0; g_filterHits = 0; g_filterCacheHits = 0; g_filterMaxNs = 0;
    g_handlerCalls = 0; g_handlerNs = 0; g_handlerMaxNs = 0;
    size_t n = g_sweepCount.load();
    for (size_t i = 0; i < n; i++) { g_sweeps[i].calls = 0; g_sweeps[i].ns = 0; g_sweeps[i].maxNs = 0; }
    g_sinceMs = NowMs();
}

std::string Perf::Json() {
    if (!g_sinceMs.load()) g_sinceMs = NowMs();  // first read seeds the window
    uint64_t since = g_sinceMs.load();
    uint64_t pos = g_ringPos.load();
    size_t have = (size_t)(pos < kRing ? pos : kRing);
    std::vector<uint32_t> s;
    s.reserve(have);
    for (size_t i = 0; i < have; i++) {
        uint32_t v = g_ring[i].load(std::memory_order_relaxed);
        s.push_back(v);
    }
    std::sort(s.begin(), s.end());
    auto pct = [&](double p) -> double {
        if (s.empty()) return 0.0;
        size_t i = (size_t)(p * (double)(s.size() - 1) + 0.5);
        return (double)s[i] / 1000.0;
    };
    uint64_t ticks = g_ticks.load();
    double avgUs = ticks ? (double)g_tickNs.load() / (double)ticks / 1000.0 : 0.0;
    uint64_t fc = g_filterCalls.load();
    double filterAvgNs = fc ? (double)g_filterNs.load() / (double)fc : 0.0;
    uint64_t hc = g_handlerCalls.load();

    std::string o = "{\"windowMs\":" + std::to_string(NowMs() - since) +
                    ",\"tick\":{\"samples\":" + std::to_string(have) +
                    ",\"count\":" + std::to_string(ticks) +
                    ",\"avgUs\":" + JsonNum(avgUs) +
                    ",\"p50Us\":" + JsonNum(pct(0.50)) +
                    ",\"p99Us\":" + JsonNum(pct(0.99)) +
                    ",\"maxUs\":" + JsonNum((double)g_tickMaxNs.load() / 1000.0) +
                    ",\"ringMaxUs\":" + JsonNum(s.empty() ? 0.0 : (double)s.back() / 1000.0) +
                    ",\"hz\":" + JsonNum(Rate(ticks, since)) +
                    ",\"budgetHits\":" + std::to_string(g_budgetHits.load()) + "}" +
                    ",\"jobs\":{\"count\":" + std::to_string(g_jobs.load()) +
                    ",\"perSecond\":" + JsonNum(Rate(g_jobs.load(), since)) + "}" +
                    ",\"gameThreadEntries\":{\"count\":" + std::to_string(g_entries.load()) +
                    ",\"perSecond\":" + JsonNum(Rate(g_entries.load(), since)) + "}" +
                    ",\"processEventFilter\":{\"calls\":" + std::to_string(fc) +
                    ",\"callsPerSecond\":" + JsonNum(Rate(fc, since)) +
                    ",\"avgNs\":" + JsonNum(filterAvgNs) +
                    ",\"maxNs\":" + std::to_string(g_filterMaxNs.load()) +
                    ",\"hits\":" + std::to_string(g_filterHits.load()) +
                    ",\"hitsPerSecond\":" + JsonNum(Rate(g_filterHits.load(), since)) +
                    ",\"cacheHits\":" + std::to_string(g_filterCacheHits.load()) +
                    ",\"totalMsInWindow\":" + JsonNum((double)g_filterNs.load() / 1e6) + "}" +
                    ",\"eventHandlers\":{\"calls\":" + std::to_string(hc) +
                    ",\"avgUs\":" + JsonNum(hc ? (double)g_handlerNs.load() / (double)hc / 1000.0 : 0.0) +
                    ",\"maxUs\":" + JsonNum((double)g_handlerMaxNs.load() / 1000.0) + "}" +
                    ",\"sweeps\":{";
    size_t n = g_sweepCount.load(std::memory_order_acquire);
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        uint64_t c = g_sweeps[i].calls.load();
        if (!first) o += ",";
        first = false;
        o += JsonStr(g_sweeps[i].name) + ":{\"calls\":" + std::to_string(c) +
             ",\"perSecond\":" + JsonNum(Rate(c, since)) +
             ",\"avgUs\":" + JsonNum(c ? (double)g_sweeps[i].ns.load() / (double)c / 1000.0 : 0.0) +
             ",\"maxUs\":" + JsonNum((double)g_sweeps[i].maxNs.load() / 1000.0) +
             ",\"totalMs\":" + JsonNum((double)g_sweeps[i].ns.load() / 1e6) + "}";
    }
    return o + "}}";
}
