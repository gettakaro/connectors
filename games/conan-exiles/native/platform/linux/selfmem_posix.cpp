// UE::SelfMem on Linux: process_vm_readv on our own pid. A bad address returns EFAULT instead of
// a SIGSEGV, so worker threads can follow game pointers that the game thread may free at any
// time. If the syscall is unavailable (seccomp, very old kernel) a pipe write/read fallback
// gives the same guarantee (write() on an unmapped source returns EFAULT).
#include "ue/mem.h"

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <vector>

namespace UE {
namespace {

class LinuxSelfMem : public Mem {
public:
    LinuxSelfMem() {
        pid_ = getpid();
        volatile uint64_t probe = 0x1122334455667788ULL;
        uint64_t got = 0;
        iovec l{&got, 8}, r{(void*)&probe, 8};
        vm_ = process_vm_readv(pid_, &l, 1, &r, 1, 0) == 8 && got == probe;
    }

    bool Read(uintptr_t addr, void* out, size_t n) const override {
        if (n == 0) return true;
        if (addr < 0x10000 || addr + n < addr || addr + n > 0x800000000000ULL) return false;
        Count();
        if (vm_) {
            iovec l{out, n}, r{(void*)addr, n};
            return process_vm_readv(pid_, &l, 1, &r, 1, 0) == (ssize_t)n;
        }
        return PipeRead(addr, out, n);
    }

    // Batches up to 1024 spans per syscall. process_vm_readv stops at the first remote iovec it
    // cannot read, so the call is repeated from the span after the failing one.
    void ReadMany(Span* spans, size_t count) const override {
        if (!vm_) return Mem::ReadMany(spans, count);
        constexpr size_t kBatch = 1024;
        std::vector<iovec> l, r;
        size_t i = 0;
        while (i < count) {
            l.clear();
            r.clear();
            const size_t first = i;
            for (size_t k = i; k < count && l.size() < kBatch; k++) {
                spans[k].ok = false;
                if (spans[k].n == 0 || spans[k].addr < 0x10000 || spans[k].addr + spans[k].n > 0x800000000000ULL)
                    break;
                l.push_back({spans[k].out, spans[k].n});
                r.push_back({(void*)spans[k].addr, spans[k].n});
            }
            ssize_t got = 0;
            if (!l.empty()) {
                Count();
                got = process_vm_readv(pid_, l.data(), l.size(), r.data(), r.size(), 0);
            }
            size_t done = 0, j = first;
            for (; j < first + l.size(); j++) {
                if (got < 0 || done + spans[j].n > (size_t)got) break;
                spans[j].ok = true;
                done += spans[j].n;
            }
            if (j == first + l.size() && l.size() == kBatch) {
                i = j;  // a full batch succeeded: continue with the next one
            } else {
                // span j failed (or was refused before the call): leave ok=false and go on after it
                if (j < count && spans[j].n == 0) spans[j].ok = true;
                i = j + 1;
            }
        }
    }

private:
    bool PipeRead(uintptr_t addr, void* out, size_t n) const {
        thread_local int fds[2] = {-1, -1};
        if (fds[0] < 0 && pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return false;
        uint8_t* dst = (uint8_t*)out;
        while (n) {
            size_t c = n > 32768 ? 32768 : n;
            ssize_t w = write(fds[1], (const void*)addr, c);
            if (w <= 0) return false;
            ssize_t got = read(fds[0], dst, (size_t)w);
            if (got != w) return false;
            addr += (size_t)w;
            dst += w;
            n -= (size_t)w;
        }
        return true;
    }

    pid_t pid_ = 0;
    bool vm_ = false;
};

}  // namespace

const Mem& SelfMem() {
    static LinuxSelfMem* m = new LinuxSelfMem;  // leaked: worker threads may outlive static destruction
    return *m;
}

}  // namespace UE
