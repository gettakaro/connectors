#include "pe_image.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace winplat {

namespace {
const IMAGE_NT_HEADERS64* Nt() {
    uintptr_t base = ImageBase();
    auto* dos = (const IMAGE_DOS_HEADER*)base;
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = (const IMAGE_NT_HEADERS64*)(base + (uintptr_t)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return nullptr;
    return nt;
}
}  // namespace

uintptr_t ImageBase() { return (uintptr_t)GetModuleHandleW(nullptr); }

std::string PeIdentity() {
    const IMAGE_NT_HEADERS64* nt = Nt();
    if (!nt) return "";
    char b[64];
    snprintf(b, sizeof b, "%lx-%lx", (unsigned long)nt->FileHeader.TimeDateStamp,
             (unsigned long)nt->OptionalHeader.SizeOfImage);
    return b;
}

std::vector<Section> Sections() {
    std::vector<Section> out;
    const IMAGE_NT_HEADERS64* nt = Nt();
    if (!nt) return out;
    uintptr_t base = ImageBase();
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        Section sec;
        char name[9] = {0};
        memcpy(name, s->Name, 8);
        sec.name = name;
        sec.address = base + s->VirtualAddress;
        sec.size = s->Misc.VirtualSize;
        sec.characteristics = s->Characteristics;
        out.push_back(sec);
    }
    return out;
}

std::vector<pins::Region> ExecutableRegions() {
    std::vector<pins::Region> out;
    for (auto& s : Sections()) {
        if (!s.Executable() || !s.size) continue;
        pins::Region r;
        r.data = (const uint8_t*)s.address;
        r.size = s.size;
        r.address = s.address;
        out.push_back(r);
    }
    return out;
}

bool InImage(uintptr_t a) {
    const IMAGE_NT_HEADERS64* nt = Nt();
    uintptr_t base = ImageBase();
    return nt && a >= base && a < base + nt->OptionalHeader.SizeOfImage;
}

bool InSection(uintptr_t a, const char* name) {
    for (auto& s : Sections())
        if (s.name == name && a >= s.address && a < s.address + s.size) return true;
    return false;
}

}  // namespace winplat
