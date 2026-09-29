// Wine integration host: loads the real dbghelp.dll (the plugin) from its own directory, the way
// enshrouded_server.exe does, and keeps the process alive for N seconds. No game is present, so every game
// capability degrades; the plugin's direct Takaro connection, log-tail fallback and diagnostics run for real.
//   host.exe <seconds>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring dll = dir + L"\\dbghelp.dll";
    HMODULE h = LoadLibraryW(dll.c_str());
    printf("{\"ev\":\"host\",\"loaded\":%s,\"err\":%lu,\"seconds\":%d}\n", h ? "true" : "false", h ? 0ul : GetLastError(), seconds);
    fflush(stdout);
    if (!h) return 2;
    Sleep((DWORD)seconds * 1000);
    printf("{\"ev\":\"host\",\"exit\":true}\n");
    fflush(stdout);
    ExitProcess(0);
}
