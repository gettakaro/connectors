// Probe entry point: the build guard, the ProcessEvent detour (chained when the stage 1
// library already patched it) and the background threads.
#include "probe.h"

#include "gamethread.h"
#include "proto.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>
#include <thread>

namespace Probe {
namespace {
constexpr const char* kBuildId = "3a05a6ef0c873f2bbf754ec495bdf2a686d3768d";
constexpr uintptr_t kProcessEvent = 0x3f12340;
constexpr uint8_t kPrologue[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                   0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00};
using PE = void (*)(void*, void*, void*);
PE g_next = nullptr;  // previous detour (stage 1) or our trampoline to the original
pid_t g_gameThread = 0;
// Function-local: the library constructor runs before this file's dynamic initializers.
std::string& Dir() {
    static std::string* d = new std::string;
    return *d;
}
__thread int t_isGame __attribute__((tls_model("initial-exec"))) = -1;
__thread bool t_draining __attribute__((tls_model("initial-exec"))) = false;
__thread bool t_inTrace __attribute__((tls_model("initial-exec"))) = false;

void WriteAbsJump(uint8_t* at, uintptr_t target) {
    at[0] = 0xff;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &target, 8);
}

void Detour(void* obj, void* func, void* parms) {
    if (GameThread::g_pending.load(std::memory_order_acquire) && !t_draining && OnGameThread()) {
        t_draining = true;
        GameThread::Drain();
        t_draining = false;
    }
    if (Trace::g_on.load(std::memory_order_relaxed) && func && !t_inTrace) {
        t_inTrace = true;
        uint32_t cookie = Trace::Pre(obj, func, parms);
        t_inTrace = false;
        g_next(obj, func, parms);
        if (cookie) {
            t_inTrace = true;
            Trace::Post(cookie, obj, func, parms);
            t_inTrace = false;
        }
        return;
    }
    g_next(obj, func, parms);
}

bool Install(std::string& how) {
    uint8_t* target = (uint8_t*)kProcessEvent;
    const long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~(uintptr_t)(pageSize - 1);
    size_t span = ((uintptr_t)target + 20 - page + pageSize - 1) & ~(size_t)(pageSize - 1);
    if (memcmp(target, kPrologue, sizeof kPrologue) == 0) {
        uint8_t* tramp = (uint8_t*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tramp == MAP_FAILED) return false;
        memcpy(tramp, target, 20);
        WriteAbsJump(tramp + 20, (uintptr_t)target + 20);
        if (mprotect(tramp, 4096, PROT_READ | PROT_EXEC) != 0) return false;
        g_next = (PE)tramp;
        if (mprotect((void*)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
        uint8_t patch[20];
        WriteAbsJump(patch, (uintptr_t)&Detour);
        memset(patch + 14, 0xcc, 6);
        memcpy(target, patch, 20);
        how = "patched the original prologue";
    } else if (target[0] == 0xff && target[1] == 0x25 && !target[2] && !target[3] && !target[4] && !target[5]) {
        // Another library (stage 1) already detoured ProcessEvent: chain in front of it.
        uintptr_t prev;
        memcpy(&prev, target + 6, 8);
        g_next = (PE)prev;
        if (mprotect((void*)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
        uintptr_t mine = (uintptr_t)&Detour;
        memcpy(target + 6, &mine, 8);
        how = "chained in front of an existing detour at " + Hex(prev);
    } else {
        return false;
    }
    mprotect((void*)page, span, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char*)target, (char*)target + 20);
    return true;
}

std::string ExePath() {
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
}

__attribute__((constructor)) void Init() {
    const std::string exe = ExePath();
    if (exe.find("ConanSandboxServer-Linux-Shipping") == std::string::npos) return;
    size_t at = exe.rfind("/Binaries/Linux/");
    Dir() = EnvOr("TAKARO_PROBE_DIR", at == std::string::npos ? "/tmp/TakaroProbe" : exe.substr(0, at) + "/Saved/TakaroProbe");
    mkdir(Dir().c_str(), 0700);
    SetNativeLogPath(Dir() + "/probe.log");
    NativeLog("Takaro Conan probe %s loading (pid %d)", kVersion, (int)getpid());
    if (EnvOr("TAKARO_PROBE_DISABLE", "") == "1") {
        NativeLog("disabled by TAKARO_PROBE_DISABLE=1");
        FlushNativeLogs();
        return;
    }
    if (ReadElfBuildId("/proc/self/exe") != kBuildId) {
        NativeLog("server build-id is not %s (25639945); probe not hooking", kBuildId);
        FlushNativeLogs();
        return;
    }
    Mem::Init();
    NativeLog("safe reads: %s", Mem::Safe() ? "process_vm_readv" : "pipe fallback");
    g_gameThread = getpid();
    std::string how;
    if (!Install(how)) {
        NativeLog("ProcessEvent hook failed (unexpected prologue); probe idle");
        FlushNativeLogs();
        return;
    }
    NativeLog("ProcessEvent hooked: %s", how.c_str());
    std::thread([] {
        // Background: log flushing, then the server once the engine is up.
        bool started = false;
        uint64_t tryAt = NowMs() + 5000;
        for (;;) {
            FlushNativeLogs();
            if (!started && NowMs() >= tryAt) {
                std::string err;
                started = StartServer(err);
                if (!started) {
                    NativeLog("server not started: %s", err.c_str());
                    tryAt = NowMs() + 30000;
                }
            }
            usleep(500 * 1000);
        }
    }).detach();
    Trace::StartWriter();
    FlushNativeLogs();
}
}  // namespace

std::string ProbeDir() { return Dir(); }
void CallProcessEvent(void* obj, void* func, void* parms) { g_next(obj, func, parms); }
bool OnGameThread() {
    if (t_isGame < 0) t_isGame = (pid_t)syscall(SYS_gettid) == g_gameThread;
    return t_isGame == 1;
}

}  // namespace Probe
