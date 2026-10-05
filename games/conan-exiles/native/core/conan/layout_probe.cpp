#include "conan/layout_probe.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace conan {

std::string HexBytes(const UE::Mem& m, uintptr_t addr, size_t n) {
    std::vector<uint8_t> b(n);
    if (!m.Read(addr, b.data(), n)) return "unreadable";
    std::string s;
    char h[4];
    for (size_t i = 0; i < n; i++) {
        if (i && i % 8 == 0) s += ' ';
        snprintf(h, sizeof h, "%02x", b[i]);
        s += h;
    }
    return s;
}

std::string ProbeVector(const UE::Mem& m, uintptr_t root, const double engine[3]) {
    std::string out;
    char buf[48];
    for (uintptr_t off = 0x100; off < 0x400; off += 8) {
        double v[3];
        if (!m.Read(root + off, v, sizeof v)) continue;
        if (std::fabs(v[0] - engine[0]) <= 2 && std::fabs(v[1] - engine[1]) <= 2 && std::fabs(v[2] - engine[2]) <= 2) {
            snprintf(buf, sizeof buf, "%s+0x%llx", out.empty() ? "" : " ", (unsigned long long)off);
            out += buf;
        }
    }
    return out.empty() ? "no match" : out;
}

std::string ProbeStats(const UE::Mem& m, uintptr_t item, int32_t stack, float dur, float maxDur) {
    char buf[160];
    snprintf(buf, sizeof buf, "engine stack=%d dur=%.2f max=%.2f;", stack, dur, maxDur);
    std::string out = buf;
    for (uintptr_t off = 0xC0; off < 0x240; off += 8) {
        struct {
            uintptr_t data;
            int32_t num, max;
        } a;
        if (!m.Read(item + off, &a, sizeof a)) continue;
        if (a.num < 1 || a.num > 64 || a.max < a.num || a.max > 256 || !UE::Plausible(a.data)) continue;
        size_t n = (size_t)a.num * 48;
        if (n > 144) n = 144;
        snprintf(buf, sizeof buf, " arr@+0x%llx num=%d max=%d: ", (unsigned long long)off, a.num, a.max);
        out += buf;
        out += HexBytes(m, a.data, n);
        out += ';';
    }
    return out;
}

std::string ProbeText(const UE::Mem& m, uintptr_t ftextAddr, const std::string& expected) {
    uintptr_t data = m.Rd<uintptr_t>(ftextAddr);
    if (!UE::Plausible(data)) return "no text data";
    std::string out = "data " + HexBytes(m, data, 0x60) + ";";
    char buf[96];
    auto isIt = [&](uintptr_t at) {
        std::string s;
        return m.ReadFString(at, s, 1024) && s == expected;
    };
    for (uintptr_t a = 0; a < 0x60; a += 8) {
        if (isIt(data + a)) {
            snprintf(buf, sizeof buf, " fstring@+0x%llx;", (unsigned long long)a);
            out += buf;
        }
        uintptr_t p1 = m.Rd<uintptr_t>(data + a);
        if (!UE::Plausible(p1)) continue;
        for (uintptr_t b = 0; b < 0x60; b += 8) {
            if (isIt(p1 + b)) {
                snprintf(buf, sizeof buf, " +0x%llx->fstring@+0x%llx;", (unsigned long long)a, (unsigned long long)b);
                out += buf;
            }
            uintptr_t p2 = m.Rd<uintptr_t>(p1 + b);
            if (!UE::Plausible(p2)) continue;
            for (uintptr_t c = 0; c < 0x40; c += 8)
                if (isIt(p2 + c)) {
                    snprintf(buf, sizeof buf, " +0x%llx->+0x%llx->fstring@+0x%llx;", (unsigned long long)a,
                             (unsigned long long)b, (unsigned long long)c);
                    out += buf;
                }
        }
    }
    return out;
}

}  // namespace conan
