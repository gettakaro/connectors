#include "hooks.h"

#include <mutex>

namespace Hooks {

std::atomic<int> g_count{0};

namespace {
// Fixed slots with atomic fields: the detour reads them without a lock. A slot is published by
// writing fn after func; a reader that sees a fn always sees its func.
struct Slot {
    std::atomic<void*> func{nullptr};
    std::atomic<Fn> fn{nullptr};
};
Slot g_slots[kMaxHooks];
std::mutex& Lock() {
    static std::mutex* m = new std::mutex;
    return *m;
}
}  // namespace

bool Add(void* ufunction, Fn fn) {
    if (!ufunction || !fn) return false;
    std::lock_guard<std::mutex> g(Lock());
    for (auto& s : g_slots)
        if (s.func.load() == ufunction && s.fn.load() == fn) return true;
    for (auto& s : g_slots) {
        if (s.fn.load() == nullptr) {
            s.func.store(ufunction, std::memory_order_relaxed);
            s.fn.store(fn, std::memory_order_release);
            g_count.fetch_add(1, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void Remove(void* ufunction, Fn fn) {
    std::lock_guard<std::mutex> g(Lock());
    for (auto& s : g_slots) {
        if (s.func.load() == ufunction && s.fn.load() == fn) {
            s.fn.store(nullptr, std::memory_order_release);
            s.func.store(nullptr, std::memory_order_relaxed);
            g_count.fetch_sub(1, std::memory_order_release);
        }
    }
}

void Dispatch(void* obj, void* func, void* parms) {
    for (auto& s : g_slots) {
        Fn fn = s.fn.load(std::memory_order_acquire);
        if (fn && s.func.load(std::memory_order_relaxed) == func) fn(obj, func, parms);
    }
}

}  // namespace Hooks
