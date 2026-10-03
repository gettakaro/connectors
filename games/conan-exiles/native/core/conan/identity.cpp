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

UniqueIdProbe Steam64FromUniqueId(const UE::Mem& m, uintptr_t at) {
    UniqueIdProbe r;
    uintptr_t obj = 0;
    if (!m.Get(at, obj) || !UE::Plausible(obj)) {
        r.how = "UniqueID holds no net id object";
        return r;
    }
    uint8_t buf[kScanBytes];
    if (!m.Read(obj, buf, sizeof buf)) {
        r.how = "net id object unreadable";
        return r;
    }
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
    // 2. An FString holding "STEAM:7656..." (FUniqueNetIdString and platform wrappers).
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
    r.how = "no Steam64 in the net id object";
    return r;
}

}  // namespace conan
