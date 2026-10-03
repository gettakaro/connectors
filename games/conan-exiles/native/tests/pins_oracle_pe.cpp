// Windows pins oracle: runs the production signature scan (core/pins) over the executable sections of
// the real Windows server binary, mapped at a non-default base to prove the RVA handling, and checks the
// result against the anchors that platform/windows/anchor_scan found at run time in the live server
// (El-Limon evidence native-stage2/L3). Portable (no Windows headers): the PE is parsed by hand.
// Usage: pins_oracle_pe <ConanSandboxServer-Win64-Shipping.exe>
#include "pins/pins.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

template <typename T>
T At(const std::vector<uint8_t>& d, size_t o) {
    T v{};
    if (o + sizeof v <= d.size()) memcpy(&v, d.data() + o, sizeof v);
    return v;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 0x400 || d[0] != 'M' || d[1] != 'Z') return 3;
    const uint32_t pe = At<uint32_t>(d, 0x3C);
    if (At<uint32_t>(d, pe) != 0x4550) return 3;
    const uint16_t nsec = At<uint16_t>(d, pe + 6), optSize = At<uint16_t>(d, pe + 20);
    const uint32_t stamp = At<uint32_t>(d, pe + 8), sizeOfImage = At<uint32_t>(d, pe + 24 + 56);
    char id[64];
    snprintf(id, sizeof id, "%lx-%lx", (unsigned long)stamp, (unsigned long)sizeOfImage);
    const uintptr_t base = (uintptr_t)0x7ff612340000ull;  // an ASLR-style base, not the preferred 0x140000000
    std::vector<pins::Region> regions;
    for (uint16_t i = 0; i < nsec; i++) {
        size_t o = pe + 24 + optSize + 40u * i;
        uint32_t vsize = At<uint32_t>(d, o + 8), va = At<uint32_t>(d, o + 12), raw = At<uint32_t>(d, o + 16),
                 rawOff = At<uint32_t>(d, o + 20), ch = At<uint32_t>(d, o + 36);
        if (!(ch & 0x20000000) || rawOff + raw > d.size()) continue;
        regions.push_back({d.data() + rawOff, (size_t)(raw < vsize ? raw : vsize), base + va});
    }
    pins::Result r = pins::Resolve("windows", id, regions, false, base);
    for (auto& line : r.details) printf("  %s\n", line.c_str());
    printf("  PE identity %s -> build %s, scan %.1f ms over %zu executable section(s)\n", id, r.build.c_str(), r.scanMs,
           regions.size());
    // Found at run time by the auto-detect scanner in the live Windows server (2026-10-03), independent of
    // the signatures: ProcessEvent = vtable slot 77 of object 0, proven by the UFunctions passing through it.
    const bool match = r.anchors.processEvent - base == 0x16408d0 && r.anchors.objObjects - base == 0xa92cdc0 &&
                       r.anchors.nameBlocks - base == 0xa85e010;
    printf("%s pins scan of Windows build %s reproduces the run-time anchors (ProcessEvent rva 0x16408d0, ObjObjects "
           "rva 0xa92cdc0, NameBlocks rva 0xa85e010)%s%s\n",
           r.ok && match ? "PASS" : "FAIL", r.build.c_str(), r.ok ? "" : ": ", r.reason.c_str());
    return r.ok && match ? 0 : 1;
}
