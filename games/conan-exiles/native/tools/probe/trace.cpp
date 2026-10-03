// Filtered ProcessEvent trace. Off: one relaxed atomic load per ProcessEvent. On: a per-thread
// cache maps each UFunction pointer to the specs whose filters it matches, so the name is
// resolved once per function; matched calls are counted, and up to `rate` per second are
// decoded and queued for the writer thread (no file I/O in the detour).
#include "probe.h"

#include <strings.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Probe {
namespace Trace {

std::atomic<bool> g_on{false};

namespace {
constexpr int kMaxSpecs = 8;

struct Spec {
    std::string id, file;
    std::vector<std::string> filters;  // lower-case substrings of the function name; "*" = all
    std::string owner, objFilter;      // lower-case substrings (function owner class, object class)
    double rate = 20;
    bool params = true, post = false, statsOnly = false;
    uint64_t maxLines = 5000, expiresMs = 0;
    std::mutex lock;
    double tokens = 0;
    uint64_t lastRefillMs = 0, lines = 0, matched = 0, dropped = 0;
    std::unordered_map<uintptr_t, uint64_t> counts;  // by UFunction; named in List()
    std::deque<std::string> queue;
};
using SpecList = std::vector<std::shared_ptr<Spec>>;

std::mutex g_specLock;
// Function-local: StartWriter() runs from the library constructor, before dynamic init.
std::shared_ptr<SpecList>& Specs() {
    static auto* p = new std::shared_ptr<SpecList>(std::make_shared<SpecList>());
    return *p;
}
std::atomic<uint32_t> g_gen{1};
uint32_t g_nextId = 1;

thread_local uint32_t t_gen = 0;
thread_local std::shared_ptr<SpecList> t_specs;
thread_local std::unordered_map<uintptr_t, uint32_t>* t_cache = nullptr;

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
uint64_t RealMs() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint32_t MaskFor(uintptr_t func) {
    if (!t_cache) t_cache = new std::unordered_map<uintptr_t, uint32_t>;
    if (t_gen != g_gen.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> g(g_specLock);
        t_specs = Specs();
        t_gen = g_gen.load();
        t_cache->clear();
    }
    auto it = t_cache->find(func);
    if (it != t_cache->end()) return it->second;
    uint32_t mask = 0;
    const std::string name = Lower(ObjName(func));
    std::string owner;
    for (size_t i = 0; i < t_specs->size(); i++) {
        Spec& s = *(*t_specs)[i];
        bool hit = false;
        for (auto& f : s.filters) hit = hit || f == "*" || name.find(f) != std::string::npos;
        if (hit && !s.owner.empty()) {
            if (owner.empty()) owner = Lower(ObjName(Mem::Rd<uintptr_t>(func + 0x20)));
            hit = owner.find(s.owner) != std::string::npos;
        }
        if (hit) mask |= 1u << i;
    }
    if (t_cache->size() > 200000) t_cache->clear();
    (*t_cache)[func] = mask;
    return mask;
}

std::string Line(const char* phase, void* obj, void* func, void* parms, bool params, bool outOnly) {
    JW w;
    w.beginObj();
    w.knum("t", (long long)RealMs());
    w.kstr("phase", phase);
    w.knum("tid", (long long)syscall(SYS_gettid));
    w.kbool("gameThread", OnGameThread());
    w.kstr("fn", ObjName(Mem::Rd<uintptr_t>((uintptr_t)func + 0x20)) + "." + ObjName((uintptr_t)func));
    w.key("obj");
    w.beginObj();
    w.kstr("addr", Hex((uintptr_t)obj));
    w.kstr("name", ObjName((uintptr_t)obj));
    w.kstr("class", ClassNameOf((uintptr_t)obj));
    w.endObj();
    if (params && parms) {
        FuncInfo fi;
        if (ReadFunc((uintptr_t)func, fi)) {
            w.key("params");
            w.beginObj();
            DecodeOpts o;
            o.depth = 2;
            o.arrayLimit = 8;
            for (auto& p : fi.params) {
                bool isOut = (p.flags & 0x100) || (p.flags & 0x400);
                if (outOnly && !isOut) continue;
                w.key(p.name.c_str());
                DecodeValue(w, p, (uintptr_t)parms + (uintptr_t)p.offset, o, 0);
            }
            w.endObj();
        }
    }
    w.endObj();
    return w.s;
}

// Counts the call and decides whether to log it (rate limit, line cap, expiry).
bool Admit(Spec& s, void* obj, void* func) {
    if (!s.objFilter.empty() && Lower(ClassNameOf((uintptr_t)obj)).find(s.objFilter) == std::string::npos) return false;
    std::lock_guard<std::mutex> g(s.lock);
    s.matched++;
    s.counts[(uintptr_t)func]++;
    if (s.statsOnly) return false;
    uint64_t now = NowMs();
    if (s.expiresMs && now > s.expiresMs) return false;
    if (s.lines >= s.maxLines) {
        s.dropped++;
        return false;
    }
    s.tokens = std::min(s.rate, s.tokens + (double)(now - s.lastRefillMs) * s.rate / 1000.0);
    s.lastRefillMs = now;
    if (s.tokens < 1) {
        s.dropped++;
        return false;
    }
    s.tokens -= 1;
    s.lines++;
    return true;
}

void Rebuild(std::shared_ptr<SpecList> list) {
    Specs() = std::move(list);
    g_on.store(!Specs()->empty(), std::memory_order_release);
    g_gen.fetch_add(1, std::memory_order_acq_rel);
}

void FlushSpec(Spec& s) {
    std::deque<std::string> q;
    {
        std::lock_guard<std::mutex> g(s.lock);
        q.swap(s.queue);
    }
    if (q.empty()) return;
    FILE* f = fopen(s.file.c_str(), "a");
    if (!f) return;
    for (auto& l : q) fprintf(f, "%s\n", l.c_str());
    fclose(f);
}
}  // namespace

uint32_t Pre(void* obj, void* func, void* parms) {
    uint32_t mask = MaskFor((uintptr_t)func);
    if (!mask) return 0;
    uint32_t cookie = 0;
    for (int i = 0; i < kMaxSpecs; i++) {
        if (!(mask & (1u << i)) || i >= (int)t_specs->size()) continue;
        Spec& s = *(*t_specs)[i];
        if (!Admit(s, obj, func)) continue;
        std::string line = Line("pre", obj, func, parms, s.params, false);
        {
            std::lock_guard<std::mutex> g(s.lock);
            s.queue.push_back(std::move(line));
        }
        if (s.post) cookie |= 1u << i;
    }
    return cookie;
}

void Post(uint32_t cookie, void* obj, void* func, void* parms) {
    if (!t_specs) return;
    for (int i = 0; i < kMaxSpecs && i < (int)t_specs->size(); i++) {
        if (!(cookie & (1u << i))) continue;
        Spec& s = *(*t_specs)[i];
        std::string line = Line("post", obj, func, parms, true, true);
        std::lock_guard<std::mutex> g(s.lock);
        s.queue.push_back(std::move(line));
    }
}

std::string Add(const JsonValue& v, std::string& err) {
    auto s = std::make_shared<Spec>();
    const JsonValue* f = v.get("filter");
    if (!f || !f->isStr() || f->str.empty()) {
        err = "filter (comma-separated function-name substrings, or *) is required";
        return "";
    }
    std::string fl = Lower(f->str);
    for (size_t a = 0; a <= fl.size();) {
        size_t b = fl.find(',', a);
        if (b == std::string::npos) b = fl.size();
        if (b > a) s->filters.push_back(fl.substr(a, b - a));
        a = b + 1;
    }
    if (const JsonValue* x = v.get("owner"); x && x->isStr()) s->owner = Lower(x->str);
    if (const JsonValue* x = v.get("objectClass"); x && x->isStr()) s->objFilter = Lower(x->str);
    if (const JsonValue* x = v.get("rate"); x && x->type == JsonValue::Number) s->rate = std::max(0.1, std::min(500.0, x->num));
    if (const JsonValue* x = v.get("params"); x && x->type == JsonValue::Bool) s->params = x->b;
    if (const JsonValue* x = v.get("post"); x && x->type == JsonValue::Bool) s->post = x->b;
    if (const JsonValue* x = v.get("statsOnly"); x && x->type == JsonValue::Bool) s->statsOnly = x->b;
    if (const JsonValue* x = v.get("maxLines"); x && x->type == JsonValue::Number) s->maxLines = (uint64_t)std::max(1.0, x->num);
    double secs = 600;
    if (const JsonValue* x = v.get("seconds"); x && x->type == JsonValue::Number) secs = std::max(1.0, std::min(86400.0, x->num));
    s->expiresMs = NowMs() + (uint64_t)(secs * 1000);
    s->tokens = s->rate;
    s->lastRefillMs = NowMs();
    std::lock_guard<std::mutex> g(g_specLock);
    if (Specs()->size() >= kMaxSpecs) {
        err = "8 traces already active; DELETE one first";
        return "";
    }
    std::string id;
    if (const JsonValue* x = v.get("id"); x && x->isStr() && !x->str.empty()) {
        for (char c : x->str)
            if (isalnum((unsigned char)c) || c == '-' || c == '_') id += c;
    }
    if (id.empty()) id = "t" + std::to_string(g_nextId++);
    for (auto& e : *Specs())
        if (e->id == id) {
            err = "trace id " + id + " already active";
            return "";
        }
    s->id = id;
    s->file = ProbeDir() + "/trace-" + id + ".log";
    auto list = std::make_shared<SpecList>(*Specs());
    list->push_back(s);
    Rebuild(list);
    return id;
}

bool Remove(const std::string& id) {
    std::shared_ptr<Spec> gone;
    {
        std::lock_guard<std::mutex> g(g_specLock);
        auto list = std::make_shared<SpecList>();
        for (auto& e : *Specs()) {
            if (e->id == id) gone = e;
            else list->push_back(e);
        }
        if (!gone) return false;
        Rebuild(list);
    }
    FlushSpec(*gone);
    return true;
}

std::string List() {
    std::shared_ptr<SpecList> list;
    {
        std::lock_guard<std::mutex> g(g_specLock);
        list = Specs();
    }
    JW w;
    w.beginArr();
    uint64_t now = NowMs();
    for (auto& sp : *list) {
        Spec& s = *sp;
        std::lock_guard<std::mutex> g(s.lock);
        w.beginObj();
        w.kstr("id", s.id);
        w.kstr("file", s.file);
        w.key("filter");
        w.beginArr();
        for (auto& f : s.filters) w.str(f);
        w.endArr();
        w.kstr("owner", s.owner);
        w.kstr("objectClass", s.objFilter);
        w.knum("matched", (long long)s.matched);
        w.knum("lines", (long long)s.lines);
        w.knum("dropped", (long long)s.dropped);
        w.knum("secondsLeft", s.expiresMs > now ? (long long)((s.expiresMs - now) / 1000) : 0);
        std::vector<std::pair<uintptr_t, uint64_t>> top(s.counts.begin(), s.counts.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        w.key("top");
        w.beginArr();
        for (size_t i = 0; i < top.size() && i < 100; i++) {
            w.beginArr();
            w.str(ObjName(Mem::Rd<uintptr_t>(top[i].first + 0x20)) + "." + ObjName(top[i].first));
            w.num((long long)top[i].second);
            w.endArr();
        }
        w.endArr();
        w.endObj();
    }
    w.endArr();
    return w.s;
}

void StartWriter() {
    std::thread([] {
        for (;;) {
            usleep(250 * 1000);
            std::shared_ptr<SpecList> list;
            {
                std::lock_guard<std::mutex> g(g_specLock);
                list = Specs();
            }
            uint64_t now = NowMs();
            std::vector<std::string> expired;
            for (auto& s : *list) {
                FlushSpec(*s);
                if (s->expiresMs && now > s->expiresMs) expired.push_back(s->id);
            }
            for (auto& id : expired) {
                NativeLog("trace %s expired", id.c_str());
                Remove(id);
            }
        }
    }).detach();
}

}  // namespace Trace
}  // namespace Probe
