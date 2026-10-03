// Takaro Conan Exiles native connector: Windows entry (the twin of platform/linux/main.cpp).
//
// Runs on the worker thread DllMain starts, inside ConanSandboxServer-Win64-Shipping.exe. It:
//   1. reads the config (env first, then ConanSandbox\Saved\Config\Takaro\takaro.json) and stays
//      completely inert when it is missing or invalid (fail closed);
//   2. finds ProcessEvent, GUObjectArray and the FNamePool by a signature scan of the server's .text and
//      checks them, as RVAs, against the pinned build (core/pins; ASLR moves the image every start);
//   3. on a verified build detours ProcessEvent with MinHook; on any other build installs nothing, but
//      still connects, identifies, sends one critical notice and answers every action with an error;
//   4. starts the Takaro bridge, its action workers and the WinHTTP (async) transport threads.
// TAKARO_CONAN_REPIN=1 additionally auto-detects the three anchors at run time (no signatures needed) and
// logs their RVAs and first bytes for tools/sigderive.py --pe; it is for re-pinning a new build only.
#include "anchor_scan.h"
#include "common.h"
#include "conan/adapter.h"
#include "dllmain.h"
#include "gamethread.h"
#include "hook_win.h"
#include "pe_image.h"
#include "pins/pins.h"
#include "takaro/bridge.h"
#include "takaro/config.h"
#include "takaro/fileio.h"
#include "takaro/outbox.h"
#include "transport_winhttp.h"
#include "ue/ue.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <string>
#include <thread>

namespace winplat {
namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string ExePath() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    buf.resize(n);
    return Utf8(buf);
}

// <root>\ConanSandbox\Binaries\Win64\<exe> -> <root>\ConanSandbox\Saved
std::string SavedDir(const std::string& exe) {
    std::string lower = exe;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    size_t at = lower.rfind("\\binaries\\win64\\");
    return at == std::string::npos ? "" : exe.substr(0, at) + "\\Saved";
}

std::string Env(const char* name) {
    // The CRT environment and the process environment agree for a server started from a .bat.
    wchar_t buf[4096];
    std::wstring wname;
    for (const char* p = name; *p; p++) wname += (wchar_t)*p;
    DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, 4096);
    return n && n < 4096 ? Utf8(std::wstring(buf, n)) : std::string();
}

long WinTid() { return (long)GetCurrentThreadId(); }

void LogLoop() {
    for (;;) {
        FlushNativeLogs();
        Sleep(250);
    }
}

// Re-pin: detect the anchors at run time and log them. When the signature path did not hook (a build
// with no Windows signatures yet), it hooks the detected ProcessEvent itself and proves it is one by the
// classes of the UFunctions that pass through it.
void RepinThread(bool hooked, std::string savedDir) {
    AutoAnchors a;
    bool ok = AutoDetectAnchors(a, 15 * 60 * 1000);
    NativeLog("repin: auto-detect %s", ok ? "found all three anchors" : "INCOMPLETE");
    size_t start = 0;
    while (start < a.report.size()) {
        size_t nl = a.report.find('\n', start);
        if (nl == std::string::npos) nl = a.report.size();
        NativeLog("repin: %s", a.report.substr(start, nl - start).c_str());
        start = nl + 1;
    }
    std::string report = a.report;
    if (a.objObjects && a.nameBlocks) {
        report += "name 0 = '" + NameText(a.nameBlocks, 0) + "'\n";
        // Cross-check the Linux reflection layout (core/ue) on the classes sendMessage/getPlayers need.
        for (const char* c : {"GameStateBase", "ConanPlayerController", "PlayerState", "ChatRpcData"}) {
            std::string dump = DumpClass(a.objObjects, a.nameBlocks, c);
            report += dump;
            size_t s0 = 0;
            while (s0 < dump.size()) {
                size_t nl = dump.find('\n', s0);
                if (nl == std::string::npos) nl = dump.size();
                NativeLog("repin: %s", dump.substr(s0, nl - s0).c_str());
                s0 = nl + 1;
            }
        }
        if (!hooked && a.processEvent) {
            std::string err;
            if (Hook::Install(a.processEvent, err)) {
                Sleep(5000);
                uintptr_t f[64];
                size_t n = Hook::SampledFunctions(f, 64);
                size_t fn = 0;
                std::string names;
                for (size_t i = 0; i < n; i++) {
                    std::string cls = ClassName(a.nameBlocks, f[i]);
                    if (cls.find("Function") != std::string::npos) fn++;
                    if (i < 12) names += (i ? ", " : "") + cls + ":" + ObjectName(a.nameBlocks, f[i]);
                }
                char line[256];
                snprintf(line, sizeof line, "detour on the detected ProcessEvent: %llu calls, %zu distinct second "
                         "arguments, %zu of them UFunctions\n", (unsigned long long)Hook::CallCount(), n, fn);
                report += line;
                report += "sample: " + names + "\n";
                NativeLog("repin: %s", line);
                NativeLog("repin: sample: %s", names.c_str());
            } else {
                report += "hook on the detected ProcessEvent failed: " + err + "\n";
                NativeLog("repin: hook on the detected ProcessEvent failed: %s", err.c_str());
            }
        }
    }
    // Player identity: once a player is online, log what PlayerState.UniqueID holds (the Steam64 source).
    if (a.objObjects && a.nameBlocks) {
        for (int tries = 0; tries < 180; tries++) {  // up to 30 minutes
            std::string ids = ProbePlayerIds(a.objObjects, a.nameBlocks);
            if (!ids.empty()) {
                report += ids;
                size_t s0 = 0;
                while (s0 < ids.size()) {
                    size_t nl = ids.find('\n', s0);
                    if (nl == std::string::npos) nl = ids.size();
                    NativeLog("repin: %s", ids.substr(s0, nl - s0).c_str());
                    s0 = nl + 1;
                }
                break;
            }
            Sleep(10000);
        }
    }
    std::string err;
    std::string dir = takaro::JoinPath(savedDir, "Takaro");
    takaro::EnsureDirectory(dir, err);
    takaro::AtomicWriteFile(takaro::JoinPath(dir, "repin-windows.txt"), report, err);
}

}  // namespace

void StartConnector() {
    const std::string exe = ExePath();
    // Dropped next to another program (the game client, a tool): forward winmm only, do nothing else.
    if (exe.find("ConanSandboxServer-Win64-Shipping") == std::string::npos) return;
    SetThreadIdSource(WinTid);
    const std::string saved = SavedDir(exe);
    std::string logPath = Env("TAKARO_CONAN_NATIVE_LOG");
    if (logPath.empty() && !saved.empty()) logPath = saved + "\\Logs\\TakaroConanNative.log";
    std::string dirError;
    if (!logPath.empty()) takaro::EnsureDirectory(takaro::DirName(logPath), dirError);
    SetNativeLogPath(logPath);
    std::thread(LogLoop).detach();
    NativeLog("Takaro Conan native " TAKARO_CONAN_NATIVE_VERSION " (windows) loading (pid %lu)",
              (unsigned long)GetCurrentProcessId());

    // 1. config, fail closed
    takaro::EnvFn env = [](const char* n) { return Env(n); };
    std::string cfgText, err;
    bool cfgExists = false;
    const std::string cfgPath = takaro::ConfigFilePath(saved, env);
    if (!takaro::ReadWholeFile(cfgPath, cfgText, cfgExists, err)) {
        NativeLog("config: %s; failing closed, the connector stays off", err.c_str());
        return;
    }
    takaro::Config cfg = takaro::LoadConfig(saved, env, cfgText, cfgExists);
    for (auto& w : cfg.warnings) NativeLog("config warning: %s", w.c_str());
    const bool repin = Env("TAKARO_CONAN_REPIN") == "1";
    if (!cfg.enabled && !repin) {
        NativeLog("connector off: %s", cfg.disabledReason.c_str());
        return;
    }
    if (cfg.enabled) NativeLog("config: %s", takaro::ConfigSummaryJson(cfg).c_str());

    // 2. pins (RVAs against the ASLR base)
    conan::AdapterOptions ao;
    ao.version = TAKARO_CONAN_NATIVE_VERSION;
    ao.savedDir = saved;
    const std::string shutdownSeconds = Env("TAKARO_CONAN_SHUTDOWN_SECONDS");
    ao.mutationOptions.shutdownSeconds = atoi(shutdownSeconds.empty() ? "60" : shutdownSeconds.c_str());
    const std::string buildId = PeIdentity();
    const uintptr_t base = ImageBase();
    pins::Result pr = pins::Resolve("windows", buildId, ExecutableRegions(),
                                    Env("TAKARO_CONAN_ALLOW_UNPINNED_BUILD") == "1", base);
    for (auto& d : pr.details) NativeLog("pins: %s", d.c_str());
    NativeLog("pins: image base %#llx, PE identity %s%s%s, scan %.1f ms: %s", (unsigned long long)base,
              buildId.empty() ? "<none>" : buildId.c_str(), pr.build.empty() ? "" : " = build ", pr.build.c_str(),
              pr.scanMs, pr.ok ? "verified" : pr.reason.c_str());
    ao.buildId = buildId;
    ao.build = pr.build;
    ao.pinsScanMs = pr.scanMs;
    for (auto& d : pr.details) ao.pinsDetail += (ao.pinsDetail.empty() ? "" : "; ") + d;

    // 3. hook (verified builds only)
    if (pr.ok) {
        std::string hookError;
        if (Hook::Install(pr.anchors.processEvent, hookError)) {
            UE::SetGlobals(pr.anchors.objObjects, pr.anchors.nameBlocks, Hook::CallProcessEvent);
            ao.ready = true;
            NativeLog("ProcessEvent hooked at %#llx (rva %#llx)", (unsigned long long)pr.anchors.processEvent,
                      (unsigned long long)(pr.anchors.processEvent - base));
        } else {
            ao.refusal = "ProcessEvent hook not installed: " + hookError;
        }
    } else {
        ao.refusal = pr.reason;
    }
    if (!ao.ready) NativeLog("NO HOOK INSTALLED: %s; every action will be refused", ao.refusal.c_str());
    if (repin) std::thread(RepinThread, ao.ready, saved).detach();
    if (!cfg.enabled) {
        NativeLog("connector off: %s (re-pin run only)", cfg.disabledReason.c_str());
        return;
    }

    // 4. Takaro bridge + transport. Leaked on purpose: other threads may still run while the process exits.
    auto* adapter = new conan::Adapter(ao);
    auto* store = new takaro::Store(cfg.stateDir);
    takaro::WinHttpConfig wc;
    wc.url = cfg.url;
    wc.caFile = cfg.caFile;
    wc.reconnectBaseMs = cfg.reconnectBaseMs;
    wc.reconnectMaxMs = cfg.reconnectMaxMs;
    auto* transport = new takaro::WinHttpTransport(wc);
    takaro::BridgeOptions bo;
    bo.config = cfg;
    bo.game = adapter;
    bo.transport = transport;
    bo.store = store;
    bo.healthFile = takaro::JoinPath(cfg.stateDir, "health.json");
    auto* bridge = new takaro::Bridge(bo);
    bridge->Start();
    NativeLog("Takaro bridge started (%s)", ao.ready ? "ready" : "refusing actions");
}

}  // namespace winplat
