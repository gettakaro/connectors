// Takaro Conan Exiles native connector: Linux entry point.
//
// Loaded with LD_PRELOAD into ConanSandboxServer-Linux-Shipping. The library holds the Takaro
// WebSocket itself (no sidecar, no RCON). At load it:
//   1. reads the config (env first, then ConanSandbox/Saved/Config/Takaro/takaro.json) and stays
//      completely inert when it is missing or invalid (fail closed);
//   2. finds ProcessEvent, GUObjectArray and the FNamePool by a signature scan of the server's own
//      code and checks them against the pinned build (core/pins);
//   3. on a verified build installs the ProcessEvent detour (the game-thread entry point); on any
//      other build installs nothing, but still connects, identifies, sends one critical notice and
//      answers every action with a structured error;
//   4. starts the Takaro bridge, its action workers and the libwebsockets transport thread.
// Nothing runs on the game thread except the detour's queue drain.
#include "common.h"
#include "conan/adapter.h"
#include "conan/events.h"
#include "conan/hook_dispatch.h"
#include "conan/reads.h"
#include "conan/perf_sampler.h"
#include "elfscan.h"
#include "gamethread.h"
#include "hook.h"
#include "pins/pins.h"
#include "takaro/bridge.h"
#include "takaro/config.h"
#include "takaro/fileio.h"
#include "takaro/outbox.h"
#include "transport_lws.h"
#include "ue/ue.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct Runtime {
    conan::Adapter* adapter = nullptr;
    takaro::Store* store = nullptr;
    takaro::LwsTransport* transport = nullptr;
    takaro::Bridge* bridge = nullptr;
    std::thread logThread;
    std::mutex mu;
    std::condition_variable cv;
    bool stopping = false;
};
Runtime* g_rt = nullptr;  // leaked on purpose: other threads may still run while the process exits

std::string ExePath() {
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
}

// <root>/ConanSandbox/Binaries/Linux/<exe> -> <root>/ConanSandbox/Saved
std::string SavedDir(const std::string& exe) {
    size_t at = exe.rfind("/Binaries/Linux/");
    return at == std::string::npos ? "" : exe.substr(0, at) + "/Saved";
}

long LinuxTid() { return (long)syscall(SYS_gettid); }

std::string Env(const char* name) {
    const char* v = getenv(name);
    return v ? std::string(v) : std::string();
}

void StartLogThread(Runtime* rt) {
    rt->logThread = std::thread([rt] {
        std::unique_lock<std::mutex> l(rt->mu);
        while (!rt->stopping) {
            l.unlock();
            FlushNativeLogs();
            l.lock();
            rt->cv.wait_for(l, std::chrono::milliseconds(250), [rt] { return rt->stopping; });
        }
    });
}

__attribute__((constructor)) void Init() {
    const std::string exe = ExePath();
    // The launcher script and every other process in the container inherit LD_PRELOAD too.
    if (exe.find("ConanSandboxServer-Linux-Shipping") == std::string::npos) return;
    SetThreadIdSource(LinuxTid);
    const std::string saved = SavedDir(exe);
    const std::string logPath =
        EnvOr("TAKARO_CONAN_NATIVE_LOG", saved.empty() ? "" : saved + "/Logs/TakaroConanNative.log");
    std::string dirError;
    if (!logPath.empty()) takaro::EnsureDirectory(takaro::DirName(logPath), dirError);
    SetNativeLogPath(logPath);
    NativeLog("Takaro Conan native " TAKARO_CONAN_NATIVE_VERSION " loading (pid %d)", (int)getpid());

    // 1. config, fail closed
    takaro::EnvFn env = [](const char* n) { return Env(n); };
    std::string cfgText, err;
    bool cfgExists = false;
    const std::string cfgPath = takaro::ConfigFilePath(saved, env);
    if (!takaro::ReadWholeFile(cfgPath, cfgText, cfgExists, err)) {
        NativeLog("config: %s; failing closed, the connector stays off", err.c_str());
        FlushNativeLogs();
        return;
    }
    takaro::Config cfg = takaro::LoadConfig(saved, env, cfgText, cfgExists);
    for (auto& w : cfg.warnings) NativeLog("config warning: %s", w.c_str());
    if (!cfg.enabled) {
        NativeLog("connector off: %s", cfg.disabledReason.c_str());
        FlushNativeLogs();
        return;
    }
    NativeLog("config: %s", takaro::ConfigSummaryJson(cfg).c_str());

    // 2. pins
    conan::AdapterOptions ao;
    ao.version = TAKARO_CONAN_NATIVE_VERSION;
    ao.savedDir = saved;
    ao.mutationOptions.shutdownSeconds = atoi(EnvOr("TAKARO_CONAN_SHUTDOWN_SECONDS", "60").c_str());
    const std::string buildId = linuxplat::ReadElfBuildId("/proc/self/exe");
    pins::Result pr = pins::Resolve("linux", buildId, linuxplat::ExecutableRegions(exe),
                                    Env("TAKARO_CONAN_ALLOW_UNPINNED_BUILD") == "1");
    for (auto& d : pr.details) NativeLog("pins: %s", d.c_str());
    NativeLog("pins: build-id %s%s%s, scan %.1f ms: %s", buildId.empty() ? "<none>" : buildId.c_str(),
              pr.build.empty() ? "" : " = build ", pr.build.c_str(), pr.scanMs, pr.ok ? "verified" : pr.reason.c_str());
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
            NativeLog("ProcessEvent hooked at %#llx", (unsigned long long)pr.anchors.processEvent);
        } else {
            ao.refusal = "ProcessEvent hook not installed: " + hookError;
        }
    } else {
        ao.refusal = pr.reason;
    }
    if (!ao.ready) NativeLog("NO HOOK INSTALLED: %s; every action will be refused", ao.refusal.c_str());

    // 4. Takaro bridge + transport
    auto* rt = new Runtime;
    rt->adapter = new conan::Adapter(ao);
    rt->store = new takaro::Store(cfg.stateDir);
    takaro::LwsConfig lc;
    lc.url = cfg.url;
    lc.caFile = cfg.caFile;
    lc.reconnectBaseMs = cfg.reconnectBaseMs;
    lc.reconnectMaxMs = cfg.reconnectMaxMs;
    rt->transport = new takaro::LwsTransport(lc);
    takaro::BridgeOptions bo;
    bo.config = cfg;
    bo.game = rt->adapter;
    bo.transport = rt->transport;
    bo.store = rt->store;
    bo.healthFile = takaro::JoinPath(cfg.stateDir, "health.json");
    rt->bridge = new takaro::Bridge(bo);
    StartLogThread(rt);
    rt->bridge->Start();
    // Game events: hook subscriptions (verified builds only) and the server log tail.
    conan::EventsOptions eo;
    eo.emit = [adapter = rt->adapter](takaro::GameEvent ev) { adapter->Emit(std::move(ev)); };
    eo.savedDir = saved;
    eo.healthFile = takaro::JoinPath(cfg.stateDir, "events-health.json");
    eo.secrets = {cfg.identityToken, cfg.registrationToken};
    eo.hooks = ao.ready;
    eo.entityNameForClass = [](const std::string& cls) {
        conan::ReadService* r = conan::ProductionReads();
        return r ? r->EntityNameForClass(cls) : std::string();
    };
    conan::StartEvents(eo);
    if (ao.ready) HookDispatch::Start();
    if (ao.ready) conan::StartPerfSampler();
    g_rt = rt;
    NativeLog("Takaro bridge started (%s)", ao.ready ? "ready" : "refusing actions");
}

// The library is never unloaded (LD_PRELOAD), so the hook stays in place. At exit the queued
// game-thread jobs are cancelled, the bridge persists the outbox, and the log is flushed.
__attribute__((destructor)) void Fini() {
    Runtime* rt = g_rt;
    if (!rt) return;
    GameThread::Stop();
    conan::StopPerfSampler();
    HookDispatch::Stop();
    conan::StopEvents();
    rt->bridge->Stop();
    {
        std::lock_guard<std::mutex> g(rt->mu);
        rt->stopping = true;
    }
    rt->cv.notify_all();
    if (rt->logThread.joinable()) rt->logThread.join();
    NativeLog("Takaro Conan native stopped");
    FlushNativeLogs();
}

}  // namespace
