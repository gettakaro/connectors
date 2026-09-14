// dbghelp.dll proxy entry point. enshrouded_server.exe imports only MiniDumpWriteDump from dbghelp.dll;
// we forward it to the system copy and start the plugin on a separate thread (no work under loader lock).
#include "common.h"
#include "hooks.h"
#include "state.h"

HMODULE g_selfModule = nullptr;
void HttpStart();
void AccountsHousekeep();

static HMODULE g_realDbghelp = nullptr;
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, PVOID, PVOID, PVOID);

extern "C" __declspec(dllexport) BOOL WINAPI MiniDumpWriteDump(HANDLE hp, DWORD pid, HANDLE hf, int type, PVOID e,
                                                               PVOID u, PVOID c) {
    if (!g_realDbghelp) {
        char p[MAX_PATH];
        UINT n = GetSystemDirectoryA(p, MAX_PATH);
        lstrcpyA(p + n, "\\dbghelp.dll");
        g_realDbghelp = LoadLibraryA(p);
    }
    auto f = g_realDbghelp ? (MiniDumpWriteDumpFn)GetProcAddress(g_realDbghelp, "MiniDumpWriteDump") : nullptr;
    if (!f) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return f(hp, pid, hf, type, e, u, c);
}

static DWORD WINAPI InitThread(LPVOID) {
    PluginLog("takaro enshrouded plugin %s starting (pid %lu)", TAKARO_PLUGIN_VERSION, GetCurrentProcessId());
    HttpStart();
    HooksInit();
    PluginLog("capabilities: %s", PluginState::Get().CapabilitiesJson().c_str());
    for (unsigned n = 1;; n++) {
        Sleep(250);
        PluginState::Get().Housekeep();
        HooksHousekeep();
        if (n % 20 == 0) AccountsHousekeep();
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = h;
        DisableThreadLibraryCalls(h);
        std::string dir = PluginBaseDir() + "\\takaro";
        CreateDirectoryA(dir.c_str(), nullptr);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
