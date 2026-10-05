#include "conan/hook_dispatch.h"

#include "common.h"
#include "gamethread.h"
#include "gtstats.h"
#include "takaro/json_util.h"
#include "ue/reflect.h"
#include "ue/ue.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace HookDispatch {

std::atomic<const Table*> g_table{nullptr};

namespace {
constexpr int kMaxSubs = 256;
constexpr int32_t kSliceObjects = 16384;
constexpr int kSliceGapMs = 30;     // at most ~one scan slice per server tick
constexpr int kJobTimeoutMs = 2000;
constexpr int kVerifyEveryMs = 30000;
constexpr int kRetryMinMs = 10000, kRetryMaxEarlyMs = 30000, kRetryMaxMs = 120000;
// While the engine names are not loaded or the game thread is busy loading the map, a retry costs
// no game-thread time, so it runs every second: the first scan then lands right after the map
// load, before the first player can log in (a 74 s backoff missed an early player-connected).
constexpr int kRetryCheapMs = 1000;
constexpr uint64_t kEarlyMs = 10 * 60 * 1000;  // the world loads in the first minutes

std::atomic<uint64_t> g_hits[kMaxSubs];
std::atomic<uint64_t> g_ns[kMaxSubs];
std::atomic<uint64_t> g_vetoes{0};

struct SubState {
    Subscription s;
    std::vector<std::string> matched;  // "Outer.Function" of each resolved UFunction
    std::string missing;               // why nothing matched (names not loaded yet, ...)
};

struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<SubState> subs;
    std::vector<std::string> requested;
    std::map<std::string, uintptr_t> found;  // requested object name -> object
    bool dirty = false;
    bool stopping = false;
    bool started = false;
    std::thread thread;
    uint64_t scans = 0, scanGameNs = 0, lastScanMs = 0;
    std::string lastError;
};
State& S() {
    static State* s = new State;  // leaked: the resolver may outlive static destruction
    return *s;
}

void Publish(std::vector<std::pair<uintptr_t, HandlerRef>> entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const std::pair<uintptr_t, HandlerRef>& a, const std::pair<uintptr_t, HandlerRef>& b) {
                         return a.first < b.first;
                     });
    auto* t = new Table;
    t->handlers.reserve(entries.size());
    for (auto& e : entries) t->handlers.push_back(e.second);
    size_t funcs = 0;
    for (size_t i = 0; i < entries.size(); i++)
        if (i == 0 || entries[i].first != entries[i - 1].first) funcs++;
    uint32_t cap = 64;
    while (cap < funcs * 2) cap <<= 1;
    t->mask = cap - 1;
    t->slots.assign(cap, Slot{0, nullptr, 0});
    for (size_t i = 0; i < entries.size();) {
        size_t j = i;
        while (j < entries.size() && entries[j].first == entries[i].first) j++;
        const uintptr_t f = entries[i].first;
        uint32_t k = (uint32_t)(SlotHash(f) >> 32) & t->mask;
        while (t->slots[k].func) k = (k + 1) & t->mask;
        t->slots[k] = Slot{f, &t->handlers[i], (uint32_t)(j - i)};
        t->bloom |= BloomBit(f);
        i = j;
    }
    // The old table is never freed: the game thread may be inside Invoke with one of its slots.
    // Tables are rebuilt only when subscriptions change or a function dies, so this stays tiny.
    g_table.store(entries.empty() ? nullptr : t, std::memory_order_release);
}

// ---- the resolver (worker thread) ----

struct Names {
    std::vector<std::string> list;
    std::vector<uint32_t> idx;
    uint32_t Of(const std::string& n) const {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i] == n) return idx[i];
        return UER::kNoName;
    }
};

struct ScanResult {
    std::vector<uintptr_t> funcs;  // UFunctions whose name is one of the subscribed names
    std::map<std::string, uintptr_t> objects;
    uint64_t gameNs = 0;
    bool complete = false;
};

// Sliced object-array scan on the game thread. Returns false when the game thread did not answer.
bool Scan(const Names& names, const std::vector<std::string>& funcNames, const std::vector<std::string>& objNames,
          ScanResult& out) {
    std::vector<uint32_t> fidx;
    for (auto& n : funcNames)
        if (names.Of(n) != UER::kNoName) fidx.push_back(names.Of(n));
    std::sort(fidx.begin(), fidx.end());
    std::vector<std::pair<uint32_t, std::string>> oidx;
    for (auto& n : objNames)
        if (names.Of(n) != UER::kNoName) oidx.push_back({names.Of(n), n});
    const uint32_t functionClass = names.Of("Function");
    int32_t next = 0, total = 0;
    if (!GameThread::Run([&] { total = UER::ObjectCount(); }, kJobTimeoutMs)) return false;
    while (next < total) {
        {
            std::lock_guard<std::mutex> g(S().mu);
            if (S().stopping) return false;
        }
        const int32_t end = std::min(total, next + kSliceObjects);
        bool ran = GameThread::Run(
            [&] {
                const uint64_t t0 = NowNs();
                for (int32_t i = next; i < end; i++) {
                    uintptr_t o = UER::ObjectAt(i);
                    if (!o) continue;
                    const uint32_t n = UER::NameIndexOf(o);
                    if (std::binary_search(fidx.begin(), fidx.end(), n) &&
                        UER::NameIndexOf(UER::ClassOf(o)) == functionClass)
                        out.funcs.push_back(o);
                    for (auto& on : oidx)
                        if (n == on.first && UER::NameNumberIsZero(o)) out.objects[on.second] = o;
                }
                out.gameNs += NowNs() - t0;
            },
            kJobTimeoutMs);
        if (!ran) return false;
        next = end;
        std::this_thread::sleep_for(std::chrono::milliseconds(kSliceGapMs));
    }
    out.complete = true;
    return true;
}

// One resolve round: names, scan, then match and offsets on the game thread. Returns true when
// every subscription matched at least one function and every requested object was found.
// `scanned` is false when it gave up before the object scan (nothing ran on the game thread).
bool Resolve(bool& scanned) {
    scanned = false;
    std::vector<Subscription> subs;
    std::vector<std::string> requested;
    {
        std::lock_guard<std::mutex> g(S().mu);
        for (auto& s : S().subs) subs.push_back(s.s);
        requested = S().requested;
        S().dirty = false;
    }
    Names names;
    names.list.push_back("Function");
    std::vector<std::string> funcNames;
    for (auto& s : subs) {
        names.list.push_back(s.baseClass);
        names.list.push_back(s.function);
        funcNames.push_back(s.function);
        for (auto& p : s.params) names.list.push_back(p);
    }
    for (auto& r : requested) names.list.push_back(r);
    UER::LookupNames(names.list, names.idx);  // worker: the name pool is append-only
    if (names.Of("Function") == UER::kNoName) {
        std::lock_guard<std::mutex> g(S().mu);
        S().lastError = "engine names not loaded yet";
        return false;
    }

    ScanResult scan;
    if (!Scan(names, funcNames, requested, scan)) {
        std::lock_guard<std::mutex> g(S().mu);
        S().lastError = "game thread did not answer the scan";
        return false;
    }

    scanned = true;
    std::vector<std::pair<uintptr_t, HandlerRef>> entries;
    std::vector<std::vector<std::string>> matched(subs.size());
    std::vector<std::string> missing(subs.size());
    uint64_t buildNs = 0;
    bool ran = GameThread::Run(
        [&] {
            const uint64_t t0 = NowNs();
            for (size_t i = 0; i < subs.size(); i++) {
                const Subscription& s = subs[i];
                const uint32_t fn = names.Of(s.function), base = names.Of(s.baseClass);
                if (fn == UER::kNoName || base == UER::kNoName) {
                    missing[i] = "name " + std::string(fn == UER::kNoName ? s.function : s.baseClass) + " not loaded yet";
                    continue;
                }
                for (uintptr_t f : scan.funcs) {
                    if (UER::NameIndexOf(f) != fn || !UER::Alive(f)) continue;
                    const uintptr_t outer = UER::OuterOf(f);
                    if (!UER::ClassIsA(outer, base)) continue;
                    HandlerRef h{};
                    h.fn = s.fn;
                    h.ctx = s.ctx;
                    h.phase = s.phase;
                    h.sub = (int)i;
                    for (int k = 0; k < kMaxParams; k++)
                        h.off[k] = k < (int)s.params.size() ? UER::PropertyOffset(f, names.Of(s.params[k])) : -1;
                    entries.push_back({f, h});
                    matched[i].push_back(UER::ObjectName(outer) + "." + s.function);
                }
                if (matched[i].empty()) missing[i] = "no " + s.baseClass + " subclass has " + s.function + " loaded yet";
            }
            buildNs = NowNs() - t0;
        },
        kJobTimeoutMs);
    if (!ran) return false;
    Publish(entries);

    bool complete = true;
    std::lock_guard<std::mutex> g(S().mu);
    S().scans++;
    S().scanGameNs += scan.gameNs + buildNs;
    S().lastScanMs = NowMs();
    S().lastError.clear();
    for (size_t i = 0; i < subs.size() && i < S().subs.size(); i++) {
        S().subs[i].matched = matched[i];
        S().subs[i].missing = missing[i];
        complete = complete && !matched[i].empty();
        std::string list;
        for (auto& m : matched[i]) list += (list.empty() ? "" : ", ") + m;
        NativeLog("hooks: [%s] %s.%s -> %zu function(s)%s%s", subs[i].owner.c_str(), subs[i].baseClass.c_str(),
                  subs[i].function.c_str(), matched[i].size(), list.empty() ? "" : ": ",
                  list.empty() ? missing[i].c_str() : list.c_str());
    }
    for (auto& r : requested) {
        auto it = scan.objects.find(r);
        if (it != scan.objects.end()) S().found[r] = it->second;
        else complete = false;
    }
    NativeLog("hooks: scan %d of %zu subscription(s) took %.2f ms of game thread in total, %zu handler(s) live",
              (int)S().scans, subs.size(), (scan.gameNs + buildNs) / 1e6, entries.size());
    return complete;
}

// Game thread: false when a hooked function or a requested object died (world reload).
bool StillValid() {
    bool ok = true;
    GameThread::Run(
        [&] {
            const Table* t = g_table.load(std::memory_order_acquire);
            if (t)
                for (auto& s : t->slots)
                    if (s.func && !UER::Alive(s.func)) ok = false;
            std::lock_guard<std::mutex> g(S().mu);
            for (auto& f : S().found)
                if (!UER::Alive(f.second)) ok = false;
        },
        kJobTimeoutMs);
    return ok;
}

void Loop() {
    int retryMs = kRetryMinMs;
    bool complete = false;
    uint64_t nextResolve = 0, nextVerify = 0;
    int32_t objectsAtScan = 0;
    const uint64_t started = NowMs();
    for (;;) {
        bool dirty;
        {
            std::unique_lock<std::mutex> l(S().mu);
            S().cv.wait_for(l, std::chrono::milliseconds(500), [] { return S().stopping || S().dirty; });
            if (S().stopping) return;
            dirty = S().dirty;
            if (S().subs.empty() && S().requested.empty()) continue;
        }
        if (!UE::HaveGlobals()) continue;
        const uint64_t now = NowMs();
        // The map load adds about a million objects (Blueprint overrides such as BaseGameMode_C.K2_PostLogin
        // among them): the table is rescanned at once instead of waiting out the backoff, so the first
        // login after a restart is not missed. ObjectCount is one aligned int read of the engine's
        // object array header, fine on this thread.
        const int32_t objects = UER::ObjectCount();
        const bool grew = objectsAtScan > 0 && objects > objectsAtScan + objectsAtScan / 4;
        if (dirty || grew || (!complete && now >= nextResolve)) {
            bool scanned = false;
            complete = Resolve(scanned);
            if (scanned) objectsAtScan = objects;
            const int cap = NowMs() - started < kEarlyMs ? kRetryMaxEarlyMs : kRetryMaxMs;
            retryMs = complete ? kRetryMinMs : std::min(cap, retryMs * 3 / 2);
            nextResolve = NowMs() + (uint64_t)(complete || scanned ? retryMs : kRetryCheapMs);
            nextVerify = NowMs() + kVerifyEveryMs;
        } else if (complete && now >= nextVerify) {
            nextVerify = now + kVerifyEveryMs;
            if (!StillValid()) {
                NativeLog("hooks: a hooked function or object died (world reload); resolving again");
                complete = false;
                nextResolve = 0;
            }
        }
    }
}
}  // namespace

int Subscribe(const Subscription& s) {
    if (!s.fn || s.baseClass.empty() || s.function.empty() || s.params.size() > (size_t)kMaxParams) return -1;
    std::lock_guard<std::mutex> g(S().mu);
    if (S().subs.size() >= (size_t)kMaxSubs) return -1;
    SubState st;
    st.s = s;
    st.missing = "not resolved yet";
    S().subs.push_back(std::move(st));
    S().dirty = true;
    S().cv.notify_all();
    return (int)S().subs.size() - 1;
}

void RequestObject(const std::string& name) {
    std::lock_guard<std::mutex> g(S().mu);
    if (std::find(S().requested.begin(), S().requested.end(), name) != S().requested.end()) return;
    S().requested.push_back(name);
    S().dirty = true;
    S().cv.notify_all();
}

uintptr_t FoundObject(const std::string& name) {
    uintptr_t o = 0;
    {
        std::lock_guard<std::mutex> g(S().mu);
        auto it = S().found.find(name);
        if (it != S().found.end()) o = it->second;
    }
    return UER::Alive(o) ? o : 0;
}

void Invoke(const Slot* slot, void* obj, void* func, void* parms, OriginalFn original) {
    uint64_t handlerNs = 0;
    struct Account {  // handler time (not the original call) on every exit path
        uint64_t& ns;
        ~Account() { GtStats::AddHook(ns); }
    } account{handlerNs};
    Call c;
    c.obj = (uintptr_t)obj;
    c.func = (uintptr_t)func;
    c.parms = (uint8_t*)parms;
    bool run = true;
    for (uint32_t i = 0; i < slot->count; i++) {
        const HandlerRef& h = slot->handlers[i];
        if (h.phase != Phase::Before) continue;
        const uint64_t t0 = NowNs();
        c.off = h.off;
        if (!h.fn(c, h.ctx)) run = false;
        const uint64_t dt = NowNs() - t0;
        handlerNs += dt;
        g_hits[h.sub & (kMaxSubs - 1)].fetch_add(1, std::memory_order_relaxed);
        g_ns[h.sub & (kMaxSubs - 1)].fetch_add(dt, std::memory_order_relaxed);
    }
    if (!run) {
        g_vetoes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    original(obj, func, parms);
    for (uint32_t i = 0; i < slot->count; i++) {
        const HandlerRef& h = slot->handlers[i];
        if (h.phase != Phase::After) continue;
        const uint64_t t0 = NowNs();
        c.off = h.off;
        h.fn(c, h.ctx);
        const uint64_t dt = NowNs() - t0;
        handlerNs += dt;
        g_hits[h.sub & (kMaxSubs - 1)].fetch_add(1, std::memory_order_relaxed);
        g_ns[h.sub & (kMaxSubs - 1)].fetch_add(dt, std::memory_order_relaxed);
    }
}

void Start() {
    std::lock_guard<std::mutex> g(S().mu);
    if (S().started) return;
    S().started = true;
    S().thread = std::thread(Loop);
}

void Stop() {
    {
        std::lock_guard<std::mutex> g(S().mu);
        if (!S().started) return;
        S().stopping = true;
    }
    S().cv.notify_all();
    if (S().thread.joinable()) S().thread.join();
}

std::string HealthJson() {
    std::lock_guard<std::mutex> g(S().mu);
    std::string subs = "[";
    for (size_t i = 0; i < S().subs.size(); i++) {
        const SubState& s = S().subs[i];
        std::string m = "[";
        for (size_t k = 0; k < s.matched.size(); k++) m += (k ? "," : "") + JsonStr(s.matched[k]);
        m += "]";
        subs += (i ? "," : "") + takaro::ObjBuilder()
                                     .S("owner", s.s.owner)
                                     .S("function", s.s.baseClass + "." + s.s.function)
                                     .Raw("matched", m)
                                     .S("missing", s.matched.empty() ? s.missing : "")
                                     .N("hits", (double)g_hits[i].load(std::memory_order_relaxed))
                                     .N("handlerMs", g_ns[i].load(std::memory_order_relaxed) / 1e6)
                                     .Done();
    }
    subs += "]";
    std::string objs = "{";
    for (auto& r : S().requested) objs += (objs.size() > 1 ? "," : "") + JsonStr(r) + ":" + (S().found.count(r) ? "true" : "false");
    objs += "}";
    return takaro::ObjBuilder()
        .N("scans", (double)S().scans)
        .N("scanGameThreadMs", S().scanGameNs / 1e6)
        .N("vetoes", (double)g_vetoes.load())
        .S("lastError", S().lastError)
        .Raw("subscriptions", subs)
        .Raw("objectsFound", objs)
        .Done();
}

void PublishForTest(const std::vector<std::pair<uintptr_t, HandlerRef>>& entries) { Publish(entries); }

void BindForTest(const std::string& function, uintptr_t func) {
    std::vector<std::pair<uintptr_t, HandlerRef>> entries;
    {
        std::lock_guard<std::mutex> g(S().mu);
        for (size_t i = 0; i < S().subs.size(); i++) {
            const Subscription& s = S().subs[i].s;
            if (s.function != function) continue;
            HandlerRef h{s.fn, s.ctx, s.phase, (int)i, {}};
            for (int k = 0; k < kMaxParams; k++) h.off[k] = -1;
            entries.push_back({func, h});
        }
    }
    Publish(entries);
}

}  // namespace HookDispatch
