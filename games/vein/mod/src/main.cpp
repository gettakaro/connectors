// Entry point: a library constructor kicks off one init thread and returns immediately, so the
// server's own startup is never blocked or delayed by us.
#include "actions.h"
#include "admin.h"
#include "common.h"
#include "events.h"
#include "gamethread.h"
#include "hooks.h"
#include "http.h"
#include "reflect.h"
#include "state.h"
#include "resolve.h"
#include "native_bridge.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_shutdownStarted{false};
std::mutex g_lifecycle;
pthread_t g_initThread{};
bool g_initStarted = false;

void ShutdownPlugin() {
    if (g_shutdownStarted.exchange(true)) return;
    g_stop = true;
    if (g_initStarted && !pthread_equal(pthread_self(), g_initThread)) pthread_join(g_initThread, nullptr);
    { std::lock_guard<std::mutex> lifecycle(g_lifecycle); NativeBridge::Stop(); }
    FlushPluginLogs();
    Hooks::RestoreAll();
}

void Sleep(unsigned ms) {
    struct timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

void* InitThread(void*) {
    // Populate immutable configuration before hooks can run on the game thread.
    (void)ConfigValue(nullptr, nullptr);
    (void)DebugEnabled();
    PluginState::Get().SetCapability("gameThread", "degraded", "starting");
    PluginLog("takaro vein plugin %s starting (pid %d, bootId %s)", TAKARO_PLUGIN_VERSION, getpid(),
              BootId().c_str());
    PluginLog("maps: %s", MemMapsSelfSoLine().c_str());

    try {
        Resolve::Init();
    } catch (...) {
        PluginLog("sym: init threw; continuing with whatever resolved");
    }
    try {
        Reflect::Init();
    } catch (...) {
        PluginLog("reflect: init threw");
    }
    try {
        GameThread::Init();
    } catch (...) {
        PluginLog("gamethread: init threw");
    }

    Events::Init();
    Actions::Init();
    Admin::Init();  // lane L3b: TAKARO_ADMIN_STEAMIDS
    Http::Start(); // optional authenticated loopback diagnostics only
    // Register before the bounded readiness wait as well as after C++ global
    // construction: an early process exit must join this thread before the
    // plugin's process-lifetime state is destroyed.
    std::atexit(ShutdownPlugin);

    // Takaro may request a player immediately after identify. Do not open its
    // socket until the game-thread pump has produced a player snapshot. Empty
    // [] is valid on a new server; a 503 means the pump has not served it.
    const auto readyBy = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!g_stop && GameThread::TickCount() == 0 && std::chrono::steady_clock::now() < readyBy) {
        FlushPluginLogs();
        Sleep(500);
    }
    if (GameThread::TickCount() == 0) {
        PluginState::Get().SetCapability("reflection", "degraded", "no engine tick observed; boot validation skipped");
        PluginLog("gamethread: no tick after 5 minutes - reflection validations skipped");
    } else {
        if (!GameThread::Run([] { Reflect::Validate(); }, 20000))
            PluginState::Get().SetCapability("reflection", "degraded", "boot validation job timed out");
        unsigned attempts = 0;
        bool snapshotReady = false;
        while (!g_stop && GameThread::Alive() && std::chrono::steady_clock::now() < readyBy) {
            ++attempts;
            if (Actions::Players().status == 200 && GameThread::Alive()) {
                snapshotReady = true;
                break;
            }
            FlushPluginLogs();
            Sleep(500);
        }
        if (snapshotReady) {
            PluginLog("native: first player snapshot ready after %u attempt(s); Takaro transport may start", attempts);
        } else if (!g_stop) {
            PluginLog("native: player snapshot unavailable after %u attempt(s); starting degraded", attempts);
        }
    }
    {
        std::lock_guard<std::mutex> lifecycle(g_lifecycle);
        if (!g_stop) NativeBridge::Start();
    }

    while (!g_stop) {
        Sleep(2000);
        try {
            Events::Housekeep();
            Actions::Housekeep();
            Admin::Housekeep();
            FlushPluginLogs();
        } catch (...) {
        }
    }
    return nullptr;
}

}  // namespace

__attribute__((constructor)) static void TakaroPluginInit() {
    // Only attach to the dedicated server binary; steamcmd and helper processes must be untouched.
    const std::string& exe = ExePath();
    if (exe.find("VeinServer") == std::string::npos) return;
    // Fallback for a process that exits before InitThread reaches its late
    // registration. ShutdownPlugin is idempotent across both registrations.
    std::atexit(ShutdownPlugin);
    g_initStarted = pthread_create(&g_initThread, nullptr, InitThread, nullptr) == 0;
}

__attribute__((destructor)) static void TakaroPluginShutdown() {
    ShutdownPlugin();
}
