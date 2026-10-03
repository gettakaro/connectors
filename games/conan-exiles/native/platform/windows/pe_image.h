// Windows identity and layout of the server image (ConanSandboxServer-Win64-Shipping.exe) as mapped in
// this process. ASLR is on (DYNAMIC_BASE + HIGH_ENTROPY_VA), so everything is an RVA against the
// runtime base. Mirrors platform/linux/elfscan.h.
#pragma once

#include "pins/pins.h"

#include <cstdint>
#include <string>
#include <vector>

namespace winplat {

struct Section {
    std::string name;
    uintptr_t address = 0;  // runtime address
    size_t size = 0;        // VirtualSize
    uint32_t characteristics = 0;
    bool Executable() const { return characteristics & 0x20000000; }
    bool Writable() const { return characteristics & 0x80000000; }
};

uintptr_t ImageBase();  // runtime base of the main module
// "<TimeDateStamp>-<SizeOfImage>" of the main module's PE header, lower-case hex, e.g.
// "a0e8d0c4-b619000". The pins build id on Windows.
std::string PeIdentity();
std::vector<Section> Sections();
std::vector<pins::Region> ExecutableRegions();  // every executable section
bool InImage(uintptr_t a);
bool InSection(uintptr_t a, const char* name);

}  // namespace winplat
