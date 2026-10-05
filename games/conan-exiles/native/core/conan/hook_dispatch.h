// ProcessEvent hook subscriptions: any module (events, moderation, reads) registers interest in a
// UFunction by name, and the detour calls its handler on the game thread when that function runs.
//
// Usage (any thread, any time; the resolver picks up new subscriptions within a few seconds):
//
//     HookDispatch::Subscription s;
//     s.owner = "moderation";
//     s.baseClass = "GameModeBase";     // the function's outer class must be this class or a subclass,
//     s.function = "K2_PostLogin";      // so Blueprint overrides (BaseGameMode_C.K2_PostLogin) match too
//     s.phase = HookDispatch::Phase::After;
//     s.params = {"NewPlayer"};         // parameter offsets resolved by reflection, per UFunction
//     s.fn = [](const HookDispatch::Call& c, void*) {
//         uintptr_t pc = c.Ptr(0);      // the NewPlayer parameter (0 when the parameter is missing)
//         ...                            // game thread: a few reads, enqueue, return
//         return true;
//     };
//     HookDispatch::Subscribe(s);
//
// Handler rules: game thread, keep it to a few reads plus an enqueue (no I/O, no JSON, no locks
// held for long). A Before handler may return false to skip the original call (and the After
// handlers); After handlers' return value is ignored. Never keep `obj`/parameter pointers for a
// later dereference: resolve names inside the handler while the objects are alive.
//
// Cost: the detour does one atomic load, one bloom-mask test and (on a bloom hit) a probe of a
// small open-addressing set per ProcessEvent: about 4 ns (S4 benchmark), ~50 us per second of
// game thread at 10.5k ProcessEvent/s. UFunctions are found by a sliced object-array scan on the
// game thread (one 16384-object slice per job, about 0.3 ms, jobs ~30 ms apart) at startup and
// whenever a subscription is added or a resolved function dies.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace HookDispatch {

enum class Phase : uint8_t { Before, After };
constexpr int kMaxParams = 8;

struct Call {
    uintptr_t obj = 0;   // the object ProcessEvent runs on
    uintptr_t func = 0;  // the matched UFunction
    uint8_t* parms = nullptr;
    const int32_t* off = nullptr;  // offsets of Subscription::params, -1 when that parameter is missing

    bool Has(int i) const { return parms && off && off[i] >= 0; }
    uint8_t* At(int i) const { return Has(i) ? parms + off[i] : nullptr; }
    template <typename T>
    T Get(int i, T def = T()) const {
        if (!Has(i)) return def;
        T v;
        memcpy(&v, parms + off[i], sizeof v);
        return v;
    }
    uintptr_t Ptr(int i) const { return Get<uintptr_t>(i, 0); }
};

using Handler = bool (*)(const Call& call, void* ctx);

struct Subscription {
    std::string owner;      // module name, for health
    std::string baseClass;  // e.g. "GameModeBase", "ConanCharacter", "ConanPlayerController"
    std::string function;   // e.g. "K2_PostLogin"
    Phase phase = Phase::Before;
    std::vector<std::string> params;  // up to kMaxParams parameter names
    Handler fn = nullptr;
    void* ctx = nullptr;
};

// Any thread. Returns the subscription id (>= 0), or -1 for an invalid subscription.
int Subscribe(const Subscription& s);
// Any thread. Asks the resolver to also find an object by exact name (e.g. a CDO
// "Default__KismetTextLibrary"); FoundObject returns it once found.
void RequestObject(const std::string& name);
// Game thread. The requested object if it was found and is still alive, else 0.
uintptr_t FoundObject(const std::string& name);

// ---- the detour's hot path ----
struct HandlerRef {
    Handler fn;
    void* ctx;
    Phase phase;
    int sub;  // subscription id
    int32_t off[kMaxParams];
};
struct Slot {
    uintptr_t func;
    const HandlerRef* handlers;  // into the owning Table (tables are immutable and never freed)
    uint32_t count;
};
struct Table {
    uint64_t bloom = 0;
    uint32_t mask = 0;
    std::vector<HandlerRef> handlers;
    std::vector<Slot> slots;
};
extern std::atomic<const Table*> g_table;

inline uint64_t SlotHash(uintptr_t f) { return (uint64_t)(f >> 4) * 0x9E3779B97F4A7C15ULL; }
inline uint64_t BloomBit(uintptr_t f) { return 1ULL << (((f >> 4) ^ (f >> 10)) & 63); }

// Any thread: the slot for `func`, or nullptr (the common case, ~4 ns).
inline const Slot* Match(void* func) {
    const Table* t = g_table.load(std::memory_order_acquire);
    if (!t) return nullptr;
    const uintptr_t f = (uintptr_t)func;
    if (!(t->bloom & BloomBit(f))) return nullptr;
    for (uint32_t i = (uint32_t)(SlotHash(f) >> 32) & t->mask;; i = (i + 1) & t->mask) {
        const Slot& s = t->slots[i];
        if (s.func == f) return &s;
        if (!s.func) return nullptr;
    }
}

using OriginalFn = void (*)(void* obj, void* func, void* parms);
// Game thread: runs the Before handlers, the original (unless a Before handler vetoed it), then
// the After handlers.
void Invoke(const Slot* slot, void* obj, void* func, void* parms, OriginalFn original);

// Starts the resolver thread (after UE::SetGlobals). Stop() ends it (process exit).
void Start();
void Stop();
// Health: per subscription the matched functions, hits and handler time.
std::string HealthJson();

// Builds and publishes a table from explicit (func, handler) pairs. Host tests only.
void PublishForTest(const std::vector<std::pair<uintptr_t, HandlerRef>>& entries);
// Publishes every subscription to `function` (by name) as bound to `func`. Host tests only.
void BindForTest(const std::string& function, uintptr_t func);

}  // namespace HookDispatch
