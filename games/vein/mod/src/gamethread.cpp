#include "gamethread.h"

#include "hooks.h"
#include "perf.h"
#include "state.h"
#include "resolve.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>

namespace {

struct Job {
    std::function<void()> fn;
    bool done = false;
    bool started = false;
    bool cancelled = false;
};

Mutex g_q;
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
std::deque<std::shared_ptr<Job>> g_jobs;

std::atomic<uint64_t> g_ticks{0};
std::atomic<long> g_tickTid{0};
std::atomic<uint64_t> g_tickChanges{0};
std::atomic<uint64_t> g_lastTickMs{0};
std::atomic<uint64_t> g_firstTickMs{0};
std::atomic<uint64_t> g_jobsRun{0};
std::atomic<uint64_t> g_jobsTimedOut{0};
std::atomic<uint64_t> g_maxDrainMs{0};
bool g_installed = false;
std::string g_how = "not installed";

// LANE L9: per-tick budget for the job queue. Jobs that do not fit wait for the next tick; one job
// always runs so a single slow job can never starve the queue. TAKARO_TICK_BUDGET_US overrides.
uint64_t TickBudgetNs() {
    static uint64_t cached = 0;
    if (!cached) {
        std::string v = ConfigValue("TAKARO_TICK_BUDGET_US", "tickBudgetUs", "500");
        long us = strtol(v.c_str(), nullptr, 10);
        if (us < 50) us = 50;
        if (us > 33000) us = 33000;
        cached = (uint64_t)us * 1000ull;
    }
    return cached;
}

void OnTick() {
    uint64_t t0 = Perf::NowNs();
    size_t jobsRan = 0;
    bool budgetHit = false;
    uint64_t now = NowMs();
    // gettid() is a real syscall; the game thread never changes identity mid-run, so it is sampled
    // once and then only every 256th tick as a cheap sanity check (L9).
    long tid = g_tickTid.load();
    if (!tid || (g_ticks.load() & 0xff) == 0) {
        tid = (long)syscall(SYS_gettid);
        long prev = g_tickTid.exchange(tid);
        if (prev && prev != tid) g_tickChanges++;
    }
    if (!g_ticks++) {
        g_firstTickMs = now;
        PluginLog("gamethread: first tick on thread %ld", tid);
        PluginState::Get().SetCapability("gameThread", "ok", "");
    }
    g_lastTickMs = now;

    const uint64_t budget = TickBudgetNs();
    for (size_t n = 0; n < GameThread::kJobsPerTick; n++) {
        if (n && Perf::NowNs() - t0 >= budget) { budgetHit = true; break; }
        std::shared_ptr<Job> job;
        {
            Guard g(g_q);
            if (g_jobs.empty()) break;
            job = g_jobs.front();
            g_jobs.pop_front();
            if (job->cancelled) continue;
            job->started = true;
        }
        uint64_t t0 = NowMs();
        try {
            job->fn();
        } catch (const std::exception& e) {
            PluginLog("gamethread: job threw: %s", e.what());
        } catch (...) {
            PluginLog("gamethread: job threw a non-standard exception");
        }
        uint64_t took = NowMs() - t0;
        if (took > g_maxDrainMs) g_maxDrainMs = took;
        g_jobsRun++;
        jobsRan++;
        {
            Guard g(g_q);
            job->done = true;
            pthread_cond_broadcast(&g_cv);
        }
    }
    Perf::RecordTick(Perf::NowNs() - t0, jobsRan, budgetHit);
}

using FnTick = void (*)(void* self, float dt, bool idle);

// One detour slot per Tick candidate. The engine class on this build is discovered at runtime, so
// all three stock/likely names are tried and whichever binds wins.
struct TickHook {
    const char* symbol;
    FnTick orig;
    std::atomic<uint64_t>* fired;  // L9: resolved once at install; no lock on the tick path
};
TickHook g_tick[] = {
    {"UVeinGameEngine::Tick", nullptr, nullptr},
    {"UGameEngine::Tick", nullptr, nullptr},
    {"UEngine::Tick", nullptr, nullptr},
};
const size_t kTickCount = sizeof(g_tick) / sizeof(g_tick[0]);

template <size_t I>
void TickDetour(void* self, float dt, bool idle) {
    if (g_tick[I].orig) g_tick[I].orig(self, dt, idle);
    if (g_tick[I].fired) g_tick[I].fired->fetch_add(1, std::memory_order_relaxed);
    try { OnTick(); } catch (...) {}
}
void* const kDetours[kTickCount] = {(void*)&TickDetour<0>, (void*)&TickDetour<1>, (void*)&TickDetour<2>};

// Swaps every *exported* vtable slot that holds `target`.
//
// Hooking only the declaring class is not enough: a derived class that inherits the implementation
// gets its own vtable with its own copy of the same function pointer, so swapping the base class
// alone can bind a vtable that never fires. We therefore sweep .dynsym for every `_ZTV*` that
// contains the address. On a binary that exports no vtables at all this finds nothing and the live
// engine object below is the only route.
size_t SwapEverywhere(const char* symName, void* detour, size_t& slotOut, std::string& howOut) {
    uint64_t target = Resolve::Addr(symName);
    if (!target) return 0;
    size_t hooked = 0;
    std::string tables;
    for (auto& vt : VTableSymbols()) {
        uint64_t addr = vt.second.first;
        uint64_t vsize = vt.second.second;
        auto* words = (const uint64_t*)(uintptr_t)addr;
        size_t count = (size_t)(vsize / 8);
        if (count < 3 || !MemReadable(words, (size_t)vsize)) continue;
        for (size_t i = 2; i < count; i++) {
            if (words[i] != target) continue;
            std::string err;
            if (Hooks::SwapVTableSlot(symName, (void*)(uintptr_t)addr, i - 2, detour, nullptr, err)) {
                hooked++;
                slotOut = i - 2;
                if (tables.size() < 200) tables += (tables.empty() ? "" : ",") + vt.first;
            } else {
                PluginLog("gamethread: %s slot %zu in %s not swapped: %s", symName, i - 2, vt.first.c_str(),
                          err.c_str());
            }
        }
    }
    howOut = tables;
    return hooked;
}

// Fallback for a binary that exports no `_ZTV*`: take the vtable off the **live** engine object
// (GEngine), find the slot that holds the resolved Tick address and swap that one slot. The slot is
// measured, never assumed.
size_t SwapLiveEngine(const char* symName, void* detour, size_t& slotOut, std::string& howOut) {
    uint64_t target = Resolve::Addr(symName);
    uint64_t gengineSym = Resolve::Addr("GEngine");
    if (!target || !gengineSym) return 0;
    if (!MemReadable((const void*)(uintptr_t)gengineSym, 8)) return 0;
    void* engine = *(void* const*)(uintptr_t)gengineSym;
    if (!engine || !MemReadable(engine, sizeof(void*))) return 0;
    void* vt = *(void* const*)engine;
    if (!vt || !MemReadable(vt, 8 * 512)) return 0;
    auto* words = (const uint64_t*)vt;
    size_t hits = 0, slot = SIZE_MAX;
    for (size_t i = 0; i < 512; i++) {
        if (words[i] == target) { slot = i; hits++; }
    }
    if (hits != 1) {
        PluginLog("gamethread: %s appears %zu times in the live engine vtable (exactly one required)", symName, hits);
        return 0;
    }
    std::string err;
    if (!Hooks::HookObjectVTable(symName, engine, slot, detour, nullptr, err)) {
        PluginLog("gamethread: live engine vtable slot %zu not swapped: %s", slot, err.c_str());
        return 0;
    }
    slotOut = slot;
    char b[160];
    snprintf(b, sizeof b, "live GEngine object %p vtable %p", engine, vt);
    howOut = b;
    return 1;
}

bool InstallOne(size_t idx) {
    const char* symName = g_tick[idx].symbol;
    uint64_t addr = Resolve::Addr(symName);
    if (!addr) {
        PluginLog("gamethread: %s unresolved", symName);
        return false;
    }
    // The original is the symbol itself: every swapped slot held exactly that address.
    g_tick[idx].orig = (FnTick)(uintptr_t)addr;
    size_t slot = SIZE_MAX;
    std::string tables;
    size_t n = SwapEverywhere(symName, kDetours[idx], slot, tables);
    std::string via = "exported vtables";
    if (!n) {
        n = SwapLiveEngine(symName, kDetours[idx], slot, tables);
        via = "live engine object";
    }
    if (!n) {
        g_tick[idx].orig = nullptr;
        return false;
    }
    g_tick[idx].fired = Hooks::FiredCounter(symName);
    PluginLog("gamethread: %s hooked in %zu vtable(s) at slot %zu via %s (%s)", symName, n, slot, via.c_str(),
              tables.c_str());
    g_how += std::string(g_how.empty() ? "" : "; ") + symName + " x" + std::to_string(n) + " slot " +
             std::to_string(slot) + " via " + via + " [" + tables + "]";
    return true;
}

}  // namespace

void GameThread::Init() {
    if (g_installed) return;
    g_how.clear();
    for (size_t i = 0; i < kTickCount; i++) g_installed = InstallOne(i) || g_installed;
    if (g_how.empty()) g_how = "not installed";
    if (!g_installed) {
        PluginState::Get().SetCapability("gameThread", "degraded", "no engine Tick vtable slot could be hooked");
        PluginLog("gamethread: NOT installed - all UObject work will return 503");
    } else {
        PluginState::Get().SetCapability("gameThread", "degraded", "hook installed, waiting for the first tick");
        PluginLog("gamethread: installed (%s)", g_how.c_str());
    }
}

bool GameThread::Alive() {
    uint64_t last = g_lastTickMs.load();
    return last && NowMs() - last < 5000;
}

uint64_t GameThread::TickCount() { return g_ticks.load(); }

#ifdef TAKARO_GAMETHREAD_TEST
void GameThread::TestEnable() { g_installed = true; }
void GameThread::TestPumpOnce() { OnTick(); }
size_t GameThread::TestQueued() { Guard g(g_q); return g_jobs.size(); }
#endif

bool GameThread::Run(std::function<void()> fn, uint32_t timeoutMs) {
    if (!g_installed) return false;
    Perf::RecordEntry();
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    {
        Guard g(g_q);
        if (g_jobs.size() >= 256) return false;  // the pump is stuck; do not pile up
        g_jobs.push_back(job);
    }
    uint64_t deadline = NowMs() + timeoutMs;
    Guard g(g_q);
    while (!job->done) {
        uint64_t now = NowMs();
        if (now >= deadline) {
            if (!job->started) job->cancelled = true;
            g_jobsTimedOut++;
            return false;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t wait = deadline - now;
        ts.tv_sec += (time_t)(wait / 1000);
        ts.tv_nsec += (long)((wait % 1000) * 1000000);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, g_q.raw(), &ts);
    }
    return true;
}

bool GameThread::RunJson(std::function<std::string()> fn, std::string& out, uint32_t timeoutMs) {
    // The job can continue after Run times out. Both the function and its result therefore belong
    // to the queued job, never to this caller's stack.
    auto result = std::make_shared<std::string>();
    bool ok = Run([fn = std::move(fn), result] {
        try {
            *result = fn();
        } catch (...) {
            *result = "{\"error\":\"handler threw on the game thread\"}";
        }
    }, timeoutMs);
    if (ok) out = *result;
    return ok;
}

std::string GameThread::StatsJson() {
    uint64_t last = g_lastTickMs.load(), first = g_firstTickMs.load(), ticks = g_ticks.load();
    double hz = (ticks > 1 && last > first) ? (double)(ticks - 1) * 1000.0 / (double)(last - first) : 0.0;
    size_t queued;
    {
        Guard g(g_q);
        queued = g_jobs.size();
    }
    return "{\"installed\":" + std::string(g_installed ? "true" : "false") + ",\"how\":" + JsonStr(g_how) +
           ",\"alive\":" + (Alive() ? "true" : "false") + ",\"tickCount\":" + std::to_string(ticks) +
           ",\"tickThreadId\":" + std::to_string(g_tickTid.load()) +
           ",\"tickThreadChanges\":" + std::to_string(g_tickChanges.load()) +
           ",\"approxHz\":" + JsonNum(hz) +
           ",\"msSinceLastTick\":" + std::to_string(last ? NowMs() - last : 0) +
           ",\"jobsRun\":" + std::to_string(g_jobsRun.load()) +
           ",\"jobsTimedOut\":" + std::to_string(g_jobsTimedOut.load()) +
           ",\"jobsQueued\":" + std::to_string(queued) +
           ",\"maxJobMs\":" + std::to_string(g_maxDrainMs.load()) +
           ",\"tickBudgetUs\":" + std::to_string(TickBudgetNs() / 1000) + "}";
}
