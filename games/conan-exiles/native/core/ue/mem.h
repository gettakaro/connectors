// Safe memory reads for worker threads.
//
// Every read the read actions make off the game thread goes through a Mem. The production Mem
// (SelfMem, platform/linux/selfmem_posix.cpp) copies with process_vm_readv on our own pid, so a
// stale or freed pointer returns false instead of crashing the server. Host tests use an
// in-memory image of a fake or recorded server (tests/reads_test.cpp).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace UE {

class Mem {
public:
    virtual ~Mem() = default;
    // Copies n bytes from addr. False (and `out` unspecified) when any byte is unreadable.
    virtual bool Read(uintptr_t addr, void* out, size_t n) const = 0;

    struct Span {
        uintptr_t addr;
        void* out;
        size_t n;
        bool ok;
    };
    // Many small reads at once (the platform batches them into few syscalls). Sets each `ok`.
    virtual void ReadMany(Span* spans, size_t count) const;

    template <typename T>
    bool Get(uintptr_t a, T& v) const {
        return Read(a, &v, sizeof v);
    }
    // Zero on failure.
    template <typename T>
    T Rd(uintptr_t a) const {
        T v{};
        if (!Read(a, &v, sizeof v)) return T{};
        return v;
    }
    // FString {TCHAR* data, int32 num (incl. NUL), int32 max} at `at`, as UTF-8. False when the
    // header or the characters are unreadable or implausible (num > maxChars + 1).
    bool ReadFString(uintptr_t at, std::string& out, int32_t maxChars = 4096) const;

    uint64_t ReadCount() const { return reads_.load(std::memory_order_relaxed); }

protected:
    void Count(uint64_t n = 1) const { reads_.fetch_add(n, std::memory_order_relaxed); }

private:
    mutable std::atomic<uint64_t> reads_{0};
};

// The platform's reader of this process's own memory (process_vm_readv on Linux). Any thread.
const Mem& SelfMem();

// Plausible user-space pointer on x86-64 (cheap pre-check before a read).
inline bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x800000000000ULL && (p & 7) == 0; }

}  // namespace UE
