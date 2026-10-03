#include "conan/identity.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace conan {

namespace {
constexpr uint64_t kSteamMin = 76561197960265728ULL;  // individual account, universe public
constexpr uint64_t kSteamMax = 76561202255233023ULL;
constexpr size_t kScanBytes = 0x80;  // of the FUniqueNetId object
}  // namespace

bool IsSteam64Number(uint64_t v) { return v >= kSteamMin && v <= kSteamMax; }

std::string ExtractSteam64(const std::string& s) {
    for (size_t i = 0; i + 17 <= s.size(); i++) {
        if (s.compare(i, 4, "7656") != 0) continue;
        if (i > 0 && isdigit((unsigned char)s[i - 1])) continue;
        bool digits = true;
        for (size_t k = 0; k < 17 && digits; k++) digits = isdigit((unsigned char)s[i + k]) != 0;
        if (!digits || (i + 17 < s.size() && isdigit((unsigned char)s[i + 17]))) continue;
        return s.substr(i, 17);
    }
    return "";
}

namespace {
UniqueIdProbe ScanObject(const UE::Mem& m, uintptr_t obj) {
    UniqueIdProbe r;
    uint8_t buf[kScanBytes];
    if (!m.Read(obj, buf, sizeof buf)) return r;
    // 1. A Steam64 stored as a number (FUniqueNetIdSteam layout).
    for (size_t o = 8; o + 8 <= sizeof buf; o += 4) {
        uint64_t v;
        memcpy(&v, buf + o, 8);
        if (IsSteam64Number(v)) {
            r.steam64 = std::to_string((unsigned long long)v);
            char how[48];
            snprintf(how, sizeof how, "uint64@+0x%zx", o);
            r.how = how;
            return r;
        }
    }
    // 2. An FString holding "7656..." / "STEAM:7656..." (FUniqueNetIdString; build 25639945: +0x10).
    for (size_t o = 8; o + 16 <= sizeof buf; o += 8) {
        uintptr_t data;
        int32_t num, max;
        memcpy(&data, buf + o, 8);
        memcpy(&num, buf + o + 8, 4);
        memcpy(&max, buf + o + 12, 4);
        if (!UE::Plausible(data & ~(uintptr_t)7) || num < 2 || num > 128 || max < num) continue;
        std::string s;
        if (!m.ReadFString(obj + o, s, 128)) continue;
        std::string id = ExtractSteam64(s);
        if (!id.empty()) {
            r.steam64 = id;
            char how[96];
            snprintf(how, sizeof how, "fstring@+0x%zx \"%.40s\"", o, s.c_str());
            r.how = how;
            return r;
        }
    }
    return r;
}
}  // namespace

// FUniqueNetIdRepl on build 25639945 (live, 2026-10-03): +0 vtable (FUniqueNetIdWrapper is
// polymorphic), +8 FUniqueNetId* (TSharedPtr object), +0x10 its reference controller, +32
// ReplicationBytes. The FUniqueNetId object holds the Steam64 as an FString at +0x10. The first
// three words are all tried, so a layout shift between builds degrades to a miss, not a crash.
UniqueIdProbe Steam64FromUniqueId(const UE::Mem& m, uintptr_t at) {
    UniqueIdProbe r;
    uintptr_t words[3] = {0, 0, 0};
    if (!m.Read(at, words, sizeof words)) {
        r.how = "UniqueID unreadable";
        return r;
    }
    for (int i = 0; i < 3; i++) {
        if (!UE::Plausible(words[i])) continue;
        UniqueIdProbe p = ScanObject(m, words[i]);
        if (!p.steam64.empty()) {
            char at8[16];
            snprintf(at8, sizeof at8, "+0x%x -> ", i * 8);
            p.how = at8 + p.how;
            return p;
        }
    }
    r.how = "no Steam64 in the net id object";
    return r;
}

}  // namespace conan
