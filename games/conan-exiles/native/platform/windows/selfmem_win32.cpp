// UE::SelfMem on Windows: ReadProcessMemory on our own process. A bad, freed or guard-page address
// makes the call fail (ERROR_PARTIAL_COPY / ERROR_NOACCESS) instead of raising an access violation,
// so worker threads can follow game pointers the game thread may free at any time. The twin of
// platform/linux/selfmem_posix.cpp (process_vm_readv); ReadMany uses the portable per-span loop.
#include "ue/mem.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace UE {
namespace {

class WinSelfMem : public Mem {
public:
    bool Read(uintptr_t addr, void* out, size_t n) const override {
        if (n == 0) return true;
        if (addr < 0x10000 || addr + n < addr || addr + n > 0x800000000000ULL) return false;
        Count();
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
    }
};

}  // namespace

const Mem& SelfMem() {
    static WinSelfMem* m = new WinSelfMem;  // leaked: worker threads may outlive static destruction
    return *m;
}

}  // namespace UE
