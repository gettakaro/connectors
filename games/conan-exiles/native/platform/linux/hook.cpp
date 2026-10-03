#include "hook.h"

#include "common.h"
#include "gamethread.h"
#include "pins/pins.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace Hook {
namespace {
using ProcessEventFn = void (*)(void*, void*, void*);

ProcessEventFn g_original = nullptr;
pid_t g_gameThread = 0;
// -1 unknown, 0 other thread, 1 game thread. initial-exec: the library is always preloaded.
__thread int t_isGameThread __attribute__((tls_model("initial-exec"))) = -1;
__thread bool t_draining __attribute__((tls_model("initial-exec"))) = false;

constexpr size_t kPatchLen = pins::kProcessEventPrologueLen;  // 20 whole instructions, no RIP-relative

// jmp qword ptr [rip+0]; .quad target
void WriteAbsJump(uint8_t* at, uintptr_t target) {
    at[0] = 0xff;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &target, 8);
}

void Detour(void* obj, void* func, void* parms) {
    if (GameThread::g_pending.load(std::memory_order_acquire) && !t_draining) {
        if (t_isGameThread < 0) t_isGameThread = (pid_t)syscall(SYS_gettid) == g_gameThread;
        if (t_isGameThread) {
            t_draining = true;
            GameThread::Drain();
            t_draining = false;
        }
    }
    g_original(obj, func, parms);
}
}  // namespace

bool Install(uintptr_t address, std::string& error) {
    uint8_t* target = (uint8_t*)address;
    if (!target || memcmp(target, pins::kProcessEventPrologue, kPatchLen) != 0) {
        error = "ProcessEvent prologue is not the one the detour can relocate";
        return false;
    }
    uint8_t* tramp = (uint8_t*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        error = "mmap for the trampoline failed";
        return false;
    }
    memcpy(tramp, target, kPatchLen);
    WriteAbsJump(tramp + kPatchLen, (uintptr_t)target + kPatchLen);
    if (mprotect(tramp, 4096, PROT_READ | PROT_EXEC) != 0) {
        error = "mprotect of the trampoline failed";
        return false;
    }
    g_original = (ProcessEventFn)tramp;
    g_gameThread = getpid();  // the UE game thread is the process's main thread

    const long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~(uintptr_t)(pageSize - 1);
    size_t span = ((uintptr_t)target + kPatchLen - page + pageSize - 1) & ~(size_t)(pageSize - 1);
    if (mprotect((void*)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        error = "mprotect of ProcessEvent failed";
        return false;
    }
    uint8_t patch[kPatchLen];
    WriteAbsJump(patch, (uintptr_t)&Detour);
    memset(patch + 14, 0xcc, kPatchLen - 14);  // never reached
    memcpy(target, patch, kPatchLen);
    if (mprotect((void*)page, span, PROT_READ | PROT_EXEC) != 0)
        NativeLog("warning: could not restore ProcessEvent page protection (left RWX)");
    __builtin___clear_cache((char*)target, (char*)target + kPatchLen);
    return true;
}

void CallProcessEvent(void* obj, void* func, void* parms) { g_original(obj, func, parms); }

}  // namespace Hook
