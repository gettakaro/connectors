// winmm.dll proxy. ConanSandboxServer-Win64-Shipping.exe imports exactly timeBeginPeriod, timeEndPeriod
// and timeGetTime from WINMM.dll (El-Limon evidence windows-w0). Each is forwarded to the system copy,
// which is loaded by full path on first use (never from DllMain).
#include "dllmain.h"
#include "hook_win.h"

#define WIN32_LEAN_AND_MEAN  // keeps mmsystem.h (and its dllimport declarations of the time* functions) out
#include <windows.h>

#include <atomic>

namespace {

std::atomic<HMODULE> g_real{nullptr};

FARPROC Real(const char* name) {
    HMODULE m = g_real.load(std::memory_order_acquire);
    if (!m) {
        wchar_t p[MAX_PATH];
        UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (!n || n > MAX_PATH - 12) return nullptr;
        lstrcpyW(p + n, L"\\winmm.dll");
        m = LoadLibraryW(p);
        if (!m) return nullptr;
        g_real.store(m, std::memory_order_release);
    }
    return GetProcAddress(m, name);
}

template <typename Fn>
Fn Resolve(std::atomic<Fn>& slot, const char* name) {
    Fn f = slot.load(std::memory_order_acquire);
    if (!f) {
        f = (Fn)Real(name);
        slot.store(f, std::memory_order_release);
    }
    return f;
}

using PeriodFn = UINT(WINAPI*)(UINT);
using TimeFn = DWORD(WINAPI*)();
std::atomic<PeriodFn> g_begin{nullptr}, g_end{nullptr};
std::atomic<TimeFn> g_time{nullptr};

constexpr UINT kTimerNoCanDo = 97;  // TIMERR_NOCANDO

DWORD WINAPI Worker(LPVOID) {
    winplat::StartConnector();
    return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) UINT WINAPI timeBeginPeriod(UINT p) {
    PeriodFn f = Resolve(g_begin, "timeBeginPeriod");
    return f ? f(p) : kTimerNoCanDo;
}

__declspec(dllexport) UINT WINAPI timeEndPeriod(UINT p) {
    PeriodFn f = Resolve(g_end, "timeEndPeriod");
    return f ? f(p) : kTimerNoCanDo;
}

__declspec(dllexport) DWORD WINAPI timeGetTime() {
    TimeFn f = Resolve(g_time, "timeGetTime");
    return f ? f() : (DWORD)GetTickCount64();
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        // A static import initialises on the process's main thread, which is UE's game thread.
        Hook::SetGameThreadId(GetCurrentThreadId());
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
