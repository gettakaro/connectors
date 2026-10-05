// Pins oracle: runs the production signature scan (core/pins) over the .text of a real server
// binary and checks the result against the stage 1 addresses, which were found by hand and proven
// live on build 25639945. Usage: pins_oracle <ConanSandboxServer-Linux-Shipping>
#include "elfscan.h"
#include "pins/pins.h"

#include <elf.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < sizeof(Elf64_Ehdr)) return 3;
    Elf64_Ehdr eh;
    memcpy(&eh, d.data(), sizeof eh);
    std::vector<pins::Region> regions;
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh;
        memcpy(&sh, d.data() + eh.e_shoff + (size_t)i * eh.e_shentsize, sizeof sh);
        if ((sh.sh_flags & SHF_EXECINSTR) && sh.sh_type == SHT_PROGBITS && sh.sh_offset + sh.sh_size <= d.size())
            regions.push_back({d.data() + sh.sh_offset, (size_t)sh.sh_size, (uintptr_t)sh.sh_addr});
    }
    const std::string id = linuxplat::ReadElfBuildId(argv[1]);
    pins::Result r = pins::Resolve("linux", id, regions, false);
    for (auto& line : r.details) printf("  %s\n", line.c_str());
    printf("  build-id %s -> build %s, scan %.1f ms over %zu executable section(s)\n", id.c_str(), r.build.c_str(),
           r.scanMs, regions.size());
    // The stage 1 oracle (src/ue.h on gettakaro 2117664), independent of the pins table.
    const bool match = r.anchors.processEvent == 0x3f12340 && r.anchors.objObjects == 0xc35a580 &&
                       r.anchors.nameBlocks == 0xc2a5d40;
    printf("%s pins scan of build %s reproduces the stage 1 addresses (ProcessEvent 0x3f12340, ObjObjects 0xc35a580, "
           "NameBlocks 0xc2a5d40)%s%s\n",
           r.ok && match ? "PASS" : "FAIL", r.build.c_str(), r.ok ? "" : ": ", r.reason.c_str());
    return r.ok && match ? 0 : 1;
}
