#include "gtstats.h"

#include "common.h"
#include "takaro/json_util.h"

#include <atomic>
#include <mutex>

namespace GtStats {
namespace {

const uint64_t g_loadNs = NowNs();  // static initialisation = when the library loaded
std::atomic<uint64_t> g_detourCalls{0}, g_detourSamples{0}, g_detourSampleNs{0};
std::atomic<uint64_t> g_drainNs{0}, g_drainJobs{0}, g_hookNs{0}, g_hookCalls{0};
std::atomic<uint64_t> g_maxDrainWindowNs{0}, g_maxDrainNs{0};

struct State {
    std::mutex mu;
    bool havePrev = false, haveLast = false;
    Snap prev, last;
    uint64_t firstMonoNs = 0;  // the first sample's clock (callers may use their own clock)
    uint64_t samples = 0;
    std::string sampler = "not started";
};
State& St() {
    static State* s = new State;  // leaked: threads may still report while the process exits
    return *s;
}

void Max(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

Snap Take(int64_t frames, uint64_t monoNs) {
    Snap s;
    s.monoNs = monoNs;
    s.frames = frames;
    s.detourCalls = g_detourCalls.load(std::memory_order_relaxed);
    s.detourSamples = g_detourSamples.load(std::memory_order_relaxed);
    s.detourSampleNs = g_detourSampleNs.load(std::memory_order_relaxed);
    s.drainNs = g_drainNs.load(std::memory_order_relaxed);
    s.drainJobs = g_drainJobs.load(std::memory_order_relaxed);
    s.hookNs = g_hookNs.load(std::memory_order_relaxed);
    s.hookCalls = g_hookCalls.load(std::memory_order_relaxed);
    s.maxDrainNs = g_maxDrainWindowNs.exchange(0, std::memory_order_relaxed);
    return s;
}

}  // namespace

void AddDetourCalls(uint64_t calls) { g_detourCalls.fetch_add(calls, std::memory_order_relaxed); }
void AddDetourSample(uint64_t ns) {
    g_detourSamples.fetch_add(1, std::memory_order_relaxed);
    g_detourSampleNs.fetch_add(ns, std::memory_order_relaxed);
}
void AddDrain(uint64_t ns, uint32_t jobs) {
    g_drainNs.fetch_add(ns, std::memory_order_relaxed);
    g_drainJobs.fetch_add(jobs, std::memory_order_relaxed);
    Max(g_maxDrainWindowNs, ns);
    Max(g_maxDrainNs, ns);
}
void AddHook(uint64_t ns) {
    g_hookNs.fetch_add(ns, std::memory_order_relaxed);
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);
}

Window Compute(const Snap& a, const Snap& b) {
    Window w;
    w.seconds = b.monoNs > a.monoNs ? (b.monoNs - a.monoNs) / 1e9 : 0;
    if (b.frames >= 0 && a.frames >= 0) w.frames = b.frames - (a.frames > 0 ? a.frames : 0);
    w.detourCalls = b.detourCalls - a.detourCalls;
    const uint64_t samples = b.detourSamples - a.detourSamples;
    const uint64_t sampleNs = b.detourSampleNs - a.detourSampleNs;
    const double perCallNs = samples ? (double)sampleNs / (double)samples : 0;
    w.hotPathUs = perCallNs * (double)w.detourCalls / 1e3;
    w.drainUs = (b.drainNs - a.drainNs) / 1e3;
    w.drainJobs = b.drainJobs - a.drainJobs;
    w.hookUs = (b.hookNs - a.hookNs) / 1e3;
    w.hookCalls = b.hookCalls - a.hookCalls;
    w.totalUs = w.hotPathUs + w.drainUs + w.hookUs;
    w.maxDrainUs = b.maxDrainNs / 1e3;
    if (w.frames > 0) {
        w.usPerTick = w.totalUs / (double)w.frames;
        if (w.seconds > 0) w.ticksPerSecond = (double)w.frames / w.seconds;
    }
    return w;
}

std::string WindowJson(const Window& w) {
    takaro::ObjBuilder o;
    o.N("seconds", w.seconds);
    if (w.frames >= 0) o.N("frames", (double)w.frames);
    else o.Null("frames");
    if (w.ticksPerSecond >= 0) o.N("ticksPerSecond", w.ticksPerSecond);
    else o.Null("ticksPerSecond");
    o.N("detourCalls", (double)w.detourCalls)
        .N("hotPathUs", w.hotPathUs)
        .N("drainUs", w.drainUs)
        .N("drainJobs", (double)w.drainJobs)
        .N("hookUs", w.hookUs)
        .N("hookCalls", (double)w.hookCalls)
        .N("totalUs", w.totalUs)
        .N("maxDrainUs", w.maxDrainUs);
    if (w.usPerTick >= 0) o.N("usPerTick", w.usPerTick);
    else o.Null("usPerTick");
    return o.Done();
}

void RecordSample(int64_t frames, uint64_t monoNs) {
    State& s = St();
    std::lock_guard<std::mutex> g(s.mu);
    if (s.haveLast) {
        s.prev = s.last;
        s.havePrev = true;
    }
    s.last = Take(frames, monoNs);
    if (!s.haveLast) s.firstMonoNs = monoNs;
    s.haveLast = true;
    s.samples++;
}

void SetSamplerState(const std::string& state) {
    std::lock_guard<std::mutex> g(St().mu);
    St().sampler = state;
}

std::string HealthJson() {
    State& s = St();
    std::lock_guard<std::mutex> g(s.mu);
    std::string window = "null", total = "null";
    if (s.haveLast) {
        if (s.havePrev) window = WindowJson(Compute(s.prev, s.last));
        // Totals since the library loaded: the monotonic clock starts at boot, not at the load.
        Snap zero;
        zero.monoNs = g_loadNs <= s.firstMonoNs ? g_loadNs : s.firstMonoNs;
        zero.frames = 0;  // the engine counts frames from the process start, which is the load
        Snap last = s.last;
        last.maxDrainNs = g_maxDrainNs.load(std::memory_order_relaxed);
        total = WindowJson(Compute(zero, last));
    }
    const uint64_t samples = g_detourSamples.load(std::memory_order_relaxed);
    return takaro::ObjBuilder()
        .S("sampler", s.sampler)
        .N("samples", (double)s.samples)
        .N("detourNsPerCall", samples ? (double)g_detourSampleNs.load(std::memory_order_relaxed) / (double)samples : 0)
        .N("maxDrainUs", g_maxDrainNs.load(std::memory_order_relaxed) / 1e3)
        .Raw("window", window)
        .Raw("sinceStart", total)
        .Done();
}

void ResetForTests() {
    for (auto* a : {&g_detourCalls, &g_detourSamples, &g_detourSampleNs, &g_drainNs, &g_drainJobs, &g_hookNs,
                    &g_hookCalls, &g_maxDrainWindowNs, &g_maxDrainNs})
        a->store(0);
    State& s = St();
    std::lock_guard<std::mutex> g(s.mu);
    s.havePrev = s.haveLast = false;
    s.prev = s.last = Snap();
    s.samples = 0;
    s.sampler = "not started";
}

}  // namespace GtStats
