#include "hook_win.h"

#include "common.h"
#include "conan/hook_dispatch.h"
#include "gamethread.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "MinHook.h"

#include <atomic>

namespace Hook {
namespace {
using ProcessEventFn = void (*)(void*, void*, void*);

ProcessEventFn g_original = nullptr;
DWORD g_gameThread = 0;
thread_local int t_isGameThread = -1;  // -1 unknown, 0 other thread, 1 game thread
thread_local bool t_draining = false;

std::atomic<uint64_t> g_calls{0};
constexpr size_t kSamples = 64;
std::atomic<uintptr_t> g_samples[kSamples];
std::atomic<size_t> g_sampled{0};

void Sample(void* func) {
    size_t n = g_sampled.load(std::memory_order_relaxed);
    if (n >= kSamples) return;
    for (size_t i = 0; i < n; i++)
        if (g_samples[i].load(std::memory_order_relaxed) == (uintptr_t)func) return;
    if (g_sampled.compare_exchange_strong(n, n + 1)) g_samples[n].store((uintptr_t)func);
}

void Detour(void* obj, void* func, void* parms) {
    if (g_sampled.load(std::memory_order_relaxed) < kSamples) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        Sample(func);
    }
    if (GameThread::g_pending.load(std::memory_order_acquire) && !t_draining) {
        if (t_isGameThread < 0) t_isGameThread = GetCurrentThreadId() == g_gameThread;
        if (t_isGameThread) {
            t_draining = true;
            GameThread::Drain();
            t_draining = false;
        }
    }
    // Subscribed UFunctions (core/conan/hook_dispatch.h): one bloom test per call when nothing matches.
    if (const HookDispatch::Slot* slot = HookDispatch::Match(func)) {
        if (t_isGameThread < 0) t_isGameThread = GetCurrentThreadId() == g_gameThread;
        if (t_isGameThread) {
            HookDispatch::Invoke(slot, obj, func, parms, g_original);
            return;
        }
    }
    g_original(obj, func, parms);
}
}  // namespace

void SetGameThreadId(unsigned long tid) { g_gameThread = (DWORD)tid; }

bool Install(uintptr_t address, std::string& error) {
    if (!address) {
        error = "no ProcessEvent address";
        return false;
    }
    if (!g_gameThread) {
        error = "game thread id unknown";
        return false;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        error = std::string("MH_Initialize: ") + MH_StatusToString(st);
        return false;
    }
    st = MH_CreateHook((LPVOID)address, (LPVOID)&Detour, (LPVOID*)&g_original);
    if (st != MH_OK) {
        error = std::string("MH_CreateHook: ") + MH_StatusToString(st);
        return false;
    }
    st = MH_EnableHook((LPVOID)address);
    if (st != MH_OK) {
        error = std::string("MH_EnableHook: ") + MH_StatusToString(st);
        return false;
    }
    return true;
}

void CallProcessEvent(void* obj, void* func, void* parms) { g_original(obj, func, parms); }

size_t SampledFunctions(uintptr_t* out, size_t max) {
    size_t n = g_sampled.load();
    if (n > kSamples) n = kSamples;
    size_t k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        uintptr_t f = g_samples[i].load();
        if (f) out[k++] = f;
    }
    return k;
}

uint64_t CallCount() { return g_calls.load(); }

}  // namespace Hook
