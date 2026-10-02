// Takaro Conan Exiles native library: entry point.
//
// Loaded with LD_PRELOAD into ConanSandboxServer-Linux-Shipping. It installs nothing unless the
// server binary is exactly build 25639945 (GNU build-id and ProcessEvent prologue both match),
// so a game update leaves the server running unmodified and sendMessage reports the bridge as
// not connected.
#include "chat.h"
#include "common.h"
#include "hook.h"
#include "poller.h"
#include "proto.h"
#include "ue.h"
#include "gamethread.h"

#include <unistd.h>

#include <cstring>
#include <string>

namespace {
bool g_started = false;

std::string ExePath() {
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
}

// <root>/ConanSandbox/Binaries/Linux/<exe> -> <root>/ConanSandbox/Saved/Logs/TakaroConanNative.log
std::string DefaultLogPath(const std::string& exe) {
    size_t at = exe.rfind("/Binaries/Linux/");
    return at == std::string::npos ? "" : exe.substr(0, at) + "/Saved/Logs/TakaroConanNative.log";
}

__attribute__((constructor)) void Init() {
    const std::string exe = ExePath();
    // steamcmd, the launcher script and anything else in the container inherit LD_PRELOAD too.
    if (exe.find("ConanSandboxServer-Linux-Shipping") == std::string::npos) return;
    SetNativeLogPath(EnvOr("TAKARO_CONAN_NATIVE_LOG", DefaultLogPath(exe)));
    NativeLog("Takaro Conan native " TAKARO_CONAN_NATIVE_VERSION " loading (pid %d)", (int)getpid());

    if (EnvOr("TAKARO_CONAN_NATIVE_DISABLE", "") == "1") {
        NativeLog("disabled by TAKARO_CONAN_NATIVE_DISABLE=1; not hooking");
        FlushNativeLogs();
        return;
    }
    const std::string buildId = ReadElfBuildId("/proc/self/exe");
    if (buildId != UE::kBuildId) {
        NativeLog("server build-id %s is not the supported %s (build 25639945); not hooking",
                  buildId.empty() ? "<none>" : buildId.c_str(), UE::kBuildId);
        FlushNativeLogs();
        return;
    }
    if (memcmp((const void*)UE::kProcessEvent, UE::kProcessEventPrologue, sizeof UE::kProcessEventPrologue) != 0) {
        NativeLog("ProcessEvent prologue does not match build 25639945; not hooking");
        FlushNativeLogs();
        return;
    }
    Poller::Config cfg;
    const std::string url = EnvOr("TAKARO_CONAN_BRIDGE_URL", "http://127.0.0.1:3010");
    if (!ParseHttpUrl(url, cfg.url)) {
        NativeLog("TAKARO_CONAN_BRIDGE_URL=%s is not an http://host:port URL; not hooking", url.c_str());
        FlushNativeLogs();
        return;
    }
    if (!Hook::Install()) {
        NativeLog("ProcessEvent hook failed; not starting");
        FlushNativeLogs();
        return;
    }
    NativeLog("ProcessEvent hooked; polling %s:%d%s/mod/poll as %s", cfg.url.host.c_str(), cfg.url.port,
              cfg.url.basePath.c_str(), kModSource);
    Poller::Start(cfg, Chat::Send);
    g_started = true;
}

// The library is never unloaded (LD_PRELOAD), so the hook stays in place; at exit only the
// poller thread is stopped and the log flushed.
__attribute__((destructor)) void Fini() {
    if (!g_started) return;
    GameThread::Stop();
    Poller::Stop();
    FlushNativeLogs();
}
}  // namespace
