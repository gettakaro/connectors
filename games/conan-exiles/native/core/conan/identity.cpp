#include "conan/identity.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace conan {

namespace {
constexpr uint64_t kSteamMin = 76561197960265728ULL;  // individual account, universe public
constexpr uint64_t kSteamMax = 76561202255233023ULL;
constexpr size_t kScanBytes = 0x80;  // of the FUniqueNetId object

bool Printable(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c <= 32 || c >= 127) return false;
    return true;
}

// The known id FString slots of the net id object (FUniqueNetIdString).
bool ProbeSlots(const UE::Mem& m, uintptr_t obj, UniqueIdProbe& r) {
    for (int32_t slot : kNetIdStringSlots) {
        std::string s;
        if (!m.ReadFString(obj + (uintptr_t)slot, s, 128) || !Printable(s)) continue;
        if (r.raw.empty()) r.raw = s;
        std::string id = ExtractSteam64(s);
        if (!id.empty()) {
            char how[48];
            snprintf(how, sizeof how, "fstring@+0x%x", (unsigned)slot);
            r.steam64 = id;
            r.raw = s;
            r.how = how;
            return true;
        }
    }
    return false;
}

// Any uint64 Steam64 or "STEAM:7656..." FString in the first 0x80 bytes of the object.
bool ScanObject(const UE::Mem& m, uintptr_t obj, UniqueIdProbe& r) {
    uint8_t buf[kScanBytes];
    if (!m.Read(obj, buf, sizeof buf)) return false;
    char how[48];
    for (size_t o = 8; o + 8 <= sizeof buf; o += 4) {  // FUniqueNetIdSteam layout
        uint64_t v;
        memcpy(&v, buf + o, 8);
        if (IsSteam64Number(v)) {
            r.steam64 = std::to_string((unsigned long long)v);
            snprintf(how, sizeof how, "uint64@+0x%zx", o);
            r.how = how;
            return true;
        }
    }
    for (size_t o = 8; o + 16 <= sizeof buf; o += 8) {  // platform wrappers
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
            r.raw = s;
            snprintf(how, sizeof how, "fstring@+0x%zx", o);
            r.how = how;
            return true;
        }
    }
    return false;
}
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
    uintptr_t words[3] = {0, 0, 0};
    if (!at || !m.Read(at, words, sizeof words)) {
        r.how = "UniqueID unreadable";
        return r;
    }
    const int order[3] = {kNetIdObject / 8, 0, 2};
    for (int k = 0; k < 3; k++) {
        const int i = order[k];
        if (!UE::Plausible(words[i])) continue;
        bool found = (i == kNetIdObject / 8 && ProbeSlots(m, words[i], r));
        if (!found) {
            UniqueIdProbe scan;
            found = ScanObject(m, words[i], scan);
            if (found) {
                scan.raw = scan.raw.empty() ? r.raw : scan.raw;
                r = scan;
            }
        }
        if (found) {
            char pre[24];
            snprintf(pre, sizeof pre, "+0x%x -> ", i * 8);
            r.how = pre + r.how;
            return r;
        }
    }
    r.how = "no Steam64 in the net id object";
    return r;
}

std::string GameIdFrom(const UniqueIdProbe& uid, const std::string& url) {
    if (!uid.steam64.empty()) return uid.steam64;
    std::string fromUrl = ExtractSteam64(url);
    if (!fromUrl.empty()) return fromUrl;
    if (!uid.raw.empty()) return uid.raw;
    return url;
}

}  // namespace conan
