#include "elfscan.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace linuxplat {

std::string ReadElfBuildId(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    std::string id;
    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) == (ssize_t)sizeof eh && memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
        eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_phentsize == sizeof(Elf64_Phdr) && eh.e_phnum < 256) {
        std::vector<Elf64_Phdr> ph(eh.e_phnum);
        ssize_t want = (ssize_t)(ph.size() * sizeof(Elf64_Phdr));
        if (pread(fd, ph.data(), want, (off_t)eh.e_phoff) == want) {
            for (auto& p : ph) {
                if (p.p_type != PT_NOTE || p.p_filesz > 65536 || !id.empty()) continue;
                std::vector<uint8_t> buf(p.p_filesz);
                if (pread(fd, buf.data(), buf.size(), (off_t)p.p_offset) != (ssize_t)buf.size()) continue;
                for (size_t off = 0; off + sizeof(Elf64_Nhdr) <= buf.size();) {
                    Elf64_Nhdr nh;
                    memcpy(&nh, buf.data() + off, sizeof nh);
                    size_t name = off + sizeof nh, desc = name + ((nh.n_namesz + 3) & ~3u);
                    size_t next = desc + ((nh.n_descsz + 3) & ~3u);
                    if (next > buf.size()) break;
                    if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && memcmp(buf.data() + name, "GNU", 4) == 0) {
                        static const char* hex = "0123456789abcdef";
                        for (size_t k = 0; k < nh.n_descsz; k++) {
                            id += hex[buf[desc + k] >> 4];
                            id += hex[buf[desc + k] & 15];
                        }
                        break;
                    }
                    off = next;
                }
            }
        }
    }
    close(fd);
    return id;
}

std::vector<pins::Region> ExecutableRegions(const std::string& path, const std::string& mapsText) {
    std::string text = mapsText;
    if (text.empty()) {
        FILE* f = fopen("/proc/self/maps", "r");
        if (!f) return {};
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
    }
    std::vector<pins::Region> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // start-end perms offset dev inode path
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        int pathAt = -1;
        if (sscanf(line.c_str(), "%llx-%llx %7s %*s %*s %*s %n", &start, &end, perms, &pathAt) < 3 || pathAt < 0)
            continue;
        std::string mapped = line.substr((size_t)pathAt);
        if (mapped != path || perms[0] != 'r' || perms[2] != 'x' || end <= start) continue;
        pins::Region r;
        r.data = (const uint8_t*)(uintptr_t)start;
        r.size = (size_t)(end - start);
        r.address = (uintptr_t)start;
        out.push_back(r);
    }
    return out;
}

}  // namespace linuxplat
