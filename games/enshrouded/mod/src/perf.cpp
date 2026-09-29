#include "perf.h"

#include "common.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace {

constexpr size_t kRing = 2048;  // recent samples per site for p99

struct Site {
    std::atomic<uint64_t> entries{0}, jobs{0}, totalTicks{0}, maxTicks{0};
    std::atomic<uint64_t> next{0};
    std::atomic<uint32_t> ring[kRing];  // sample duration in 0.1 microsecond units (saturating)
};

Site g_sites[kPerfSiteCount];
std::atomic<uint64_t> g_windowStart{0};
const char* kNames[kPerfSiteCount] = {"tick", "updatePlayers", "moderation", "logSink"};

uint64_t Freq() {
    static const uint64_t f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return (uint64_t)li.QuadPart;
    }();
    return f;
}

}  // namespace

uint64_t PerfNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    uint64_t now = (uint64_t)li.QuadPart;
    uint64_t zero = 0;
    g_windowStart.compare_exchange_strong(zero, now, std::memory_order_relaxed);
    return now;
}

void PerfRecord(PerfSite site, uint64_t startTicks, uint32_t jobs) {
    uint64_t d = PerfNow() - startTicks;
    Site& s = g_sites[site];
    s.entries.fetch_add(1, std::memory_order_relaxed);
    if (jobs) s.jobs.fetch_add(jobs, std::memory_order_relaxed);
    s.totalTicks.fetch_add(d, std::memory_order_relaxed);
    uint64_t m = s.maxTicks.load(std::memory_order_relaxed);
    while (d > m && !s.maxTicks.compare_exchange_weak(m, d, std::memory_order_relaxed)) {
    }
    uint64_t tenthUs = d * 10000000ull / Freq();
    s.ring[s.next.fetch_add(1, std::memory_order_relaxed) % kRing].store(
        (uint32_t)std::min<uint64_t>(tenthUs, 0xffffffffu), std::memory_order_relaxed);
}

std::string PerfJson(bool reset) {
    uint64_t now = PerfNow();
    uint64_t start = g_windowStart.load(std::memory_order_relaxed);
    double windowS = start && now > start ? (double)(now - start) / (double)Freq() : 0;
    char b[512];
    snprintf(b, sizeof b, "{\"windowS\":%.1f,\"note\":\"microseconds of plugin work per entry into the hooked game call\"", windowS);
    std::string o = b;
    for (int i = 0; i < kPerfSiteCount; i++) {
        Site& s = g_sites[i];
        uint64_t n = s.entries.load(std::memory_order_relaxed);
        uint64_t jobs = s.jobs.load(std::memory_order_relaxed);
        double avgUs = n ? (double)s.totalTicks.load(std::memory_order_relaxed) * 1e6 / (double)Freq() / (double)n : 0;
        double maxUs = (double)s.maxTicks.load(std::memory_order_relaxed) * 1e6 / (double)Freq();
        size_t have = (size_t)std::min<uint64_t>(s.next.load(std::memory_order_relaxed), kRing);
        std::vector<uint32_t> v(have);
        for (size_t k = 0; k < have; k++) v[k] = s.ring[k].load(std::memory_order_relaxed);
        double p99 = 0;
        if (have) {
            size_t idx = std::min(have - 1, (size_t)((double)have * 0.99));
            std::nth_element(v.begin(), v.begin() + (ptrdiff_t)idx, v.end());
            p99 = v[idx] / 10.0;
        }
        snprintf(b, sizeof b,
                 ",\"%s\":{\"entries\":%llu,\"entriesPerSec\":%.2f,\"jobs\":%llu,\"jobsPerSec\":%.3f,\"avgUs\":%.2f,"
                 "\"p99Us\":%.1f,\"maxUs\":%.1f,\"p99Samples\":%zu}",
                 kNames[i], (unsigned long long)n, windowS > 0 ? n / windowS : 0.0, (unsigned long long)jobs,
                 windowS > 0 ? jobs / windowS : 0.0, avgUs, p99, maxUs, have);
        o += b;
        if (reset) {
            s.entries = 0;
            s.jobs = 0;
            s.totalTicks = 0;
            s.maxTicks = 0;
            s.next = 0;
        }
    }
    if (reset) g_windowStart = now;
    return o + "}";
}
