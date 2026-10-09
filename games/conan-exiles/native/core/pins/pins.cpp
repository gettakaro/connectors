#include "pins/pins.h"

#include "common.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace pins {

const uint8_t kProcessEventPrologue[kProcessEventPrologueLen] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                                 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                                                 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00};

namespace {

// Derived with tools/sigderive.py from build 25639945 (see pins.json). The ObjObjects reference
// sits inside ProcessEvent itself (its object-index bounds check), so one function anchors both.
const std::vector<Signature> kLinux = {
    {"processEvent",
     "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ?? ?? 00 00 48 89 D3 49 89 FE 8B 47 0C 3B 05 ?? ?? ?? ?? "
     "7D ?? 0F B7 C8 48 8B 15 ?? ?? ?? ??",
     Capture::Start, 0},
    {"objObjects",
     "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ?? ?? 00 00 48 89 D3 49 89 FE 8B 47 0C 3B 05 ?? ?? ?? ?? "
     "7D ?? 0F B7 C8 48 8B 15 ?? ?? ?? ??",
     Capture::Rip32, 43},
    // FName -> entry: mov eax,ebx; shr eax,16; mov rax,[rax*8+Blocks]; movzx ecx,bx; movzx r9d,word [rax+rcx*2]
    {"nameBlocks", "89 D8 C1 E8 10 48 8B 04 C5 ?? ?? ?? ?? 0F B7 CB 44 0F B7 0C 48", Capture::Abs32, 9},
};
// Derived with tools/sigderive.py --pe from the Windows build 25639945 (ConanSandboxServer-Win64-Shipping.exe,
// MSVC, ASLR): the anchors were first found at run time by platform/windows/anchor_scan (TAKARO_CONAN_REPIN=1;
// ProcessEvent is vtable slot 77 there, 78 on Linux) and then frozen here. Captures resolve to absolute
// addresses in the relocated image; the pinned values below are RVAs.
const std::vector<Signature> kWindows = {
    {"processEvent",
     "40 55 56 57 41 54 41 55 41 56 41 57 ?? ?? ?? ?? 01 00 00 48 8D 6C 24 30 48 ?? ?? ?? ?? 00 ?? ?? ?? ?? ?? ?? "
     "?? ?? 48 33 C5 48 ?? ?? ?? ?? 00 00 8B",
     Capture::Start, 0},
    // mov r10,[rip+ObjObjects]; movzx ecx,bx (object index -> chunk table)
    {"objObjects", "4C 8B 15 ?? ?? ?? ?? 0F B7 CB", Capture::Rip32, 3},
    // lea r8,[rip+Blocks]; cmp r14,rdi (FNamePool block table)
    {"nameBlocks", "4C 8D 05 ?? ?? ?? ?? 4C 3B F7", Capture::Rip32, 3},
};
const std::vector<Signature> kNone;

#ifdef TAKARO_DEBUG_WRONG_BUILD_ID
#define TAKARO_LINUX_25639945_ID "0000000000000000000000000000000000000000"  // degrade proof only
#define TAKARO_WINDOWS_25639945_ID "00000000-0000000"
#define TAKARO_LINUX_25738716_ID "0000000000000000000000000000000000000001"
#define TAKARO_WINDOWS_25738716_ID "00000000-0000001"
#define TAKARO_LINUX_25792439_ID "0000000000000000000000000000000000000002"
#define TAKARO_WINDOWS_25792439_ID "00000000-0000002"
#else
#define TAKARO_WINDOWS_25639945_ID "a0e8d0c4-b619000"
#define TAKARO_LINUX_25639945_ID "3a05a6ef0c873f2bbf754ec495bdf2a686d3768d"
#define TAKARO_WINDOWS_25738716_ID "6118eb61-b64b000"
#define TAKARO_LINUX_25738716_ID "8d5382c18fa73ab61bee077e65d3acc427825502"
#define TAKARO_WINDOWS_25792439_ID "d2f81866-b64b000"
#define TAKARO_LINUX_25792439_ID "ad0c0103e496b7c19bfda90800c59ae494295d35"
#endif

const std::vector<BuildPin> kPinned = {
    {"linux", TAKARO_LINUX_25639945_ID, "25639945", {0x3f12340, 0xc35a580, 0xc2a5d40}},
    // PE "<TimeDateStamp>-<SizeOfImage>"; anchors are RVAs (sha256 of the exe in pins.json).
    {"windows", TAKARO_WINDOWS_25639945_ID, "25639945", {0x16408d0, 0xa92cdc0, 0xa85e010}},
    {"linux", TAKARO_LINUX_25738716_ID, "25738716", {0x3f25340, 0xc38f680, 0xc2dae40}},
    {"windows", TAKARO_WINDOWS_25738716_ID, "25738716", {0x1642150, 0xa95a040, 0xa88b290}},
    {"linux", TAKARO_LINUX_25792439_ID, "25792439", {0x3f25340, 0xc38f680, 0xc2dae40}},
    {"windows", TAKARO_WINDOWS_25792439_ID, "25792439", {0x1642190, 0xa95a040, 0xa88b290}},
};

uintptr_t& Slot(Anchors& a, const std::string& name) {
    static uintptr_t dummy;
    if (name == "processEvent") return a.processEvent;
    if (name == "objObjects") return a.objObjects;
    if (name == "nameBlocks") return a.nameBlocks;
    return dummy = 0;
}

std::string Hex(uintptr_t v) {
    char b[32];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

// memmem without the GNU extension, so core/ stays portable (MSVC/mingw have none).
const uint8_t* Find(const uint8_t* hay, size_t n, const uint8_t* needle, size_t m) {
    if (!m || n < m) return nullptr;
    const uint8_t* end = hay + (n - m) + 1;
    for (const uint8_t* p = hay; p < end;) {
        p = (const uint8_t*)memchr(p, needle[0], (size_t)(end - p));
        if (!p) return nullptr;
        if (memcmp(p + 1, needle + 1, m - 1) == 0) return p;
        p++;
    }
    return nullptr;
}

bool ReadField(const std::vector<Region>& regions, uintptr_t at, int32_t& out) {
    for (auto& r : regions) {
        if (at >= r.address && at + 4 <= r.address + r.size) {
            memcpy(&out, r.data + (at - r.address), 4);
            return true;
        }
    }
    return false;
}

}  // namespace

const std::vector<Signature>& SignaturesFor(const std::string& platform) {
    return platform == "linux" ? kLinux : platform == "windows" ? kWindows : kNone;
}

const std::vector<BuildPin>& PinnedBuilds() { return kPinned; }

bool ParsePattern(const std::string& pattern, std::vector<int>& out) {
    out.clear();
    size_t i = 0;
    while (i < pattern.size()) {
        while (i < pattern.size() && pattern[i] == ' ') i++;
        if (i >= pattern.size()) break;
        if (i + 2 > pattern.size() || (i + 2 < pattern.size() && pattern[i + 2] != ' ')) return false;
        std::string tok = pattern.substr(i, 2);
        i += 2;
        if (tok == "??") {
            out.push_back(-1);
            continue;
        }
        int v = 0;
        for (char c : tok) {
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        out.push_back(v);
    }
    return !out.empty() && out.front() >= 0;
}

std::vector<uintptr_t> FindMatches(const std::vector<Region>& regions, const std::vector<int>& pattern,
                                   size_t limit) {
    std::vector<uintptr_t> hits;
    if (pattern.empty()) return hits;
    // memmem on the longest literal run, then verify the whole pattern around it.
    size_t bestStart = 0, bestLen = 0;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] < 0) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < pattern.size() && pattern[j] >= 0) j++;
        if (j - i > bestLen) {
            bestStart = i;
            bestLen = j - i;
        }
        i = j;
    }
    std::vector<uint8_t> needle;
    for (size_t k = 0; k < bestLen; k++) needle.push_back((uint8_t)pattern[bestStart + k]);
    for (auto& r : regions) {
        if (r.size < pattern.size()) continue;
        const uint8_t* p = r.data + bestStart;
        const uint8_t* end = r.data + (r.size - pattern.size()) + bestStart + bestLen;
        while (p < end) {
            const uint8_t* f = Find(p, (size_t)(end - p), needle.data(), needle.size());
            if (!f) break;
            const uint8_t* start = f - bestStart;
            bool ok = true;
            for (size_t k = 0; k < pattern.size() && ok; k++)
                ok = pattern[k] < 0 || start[k] == (uint8_t)pattern[k];
            if (ok) {
                hits.push_back(r.address + (uintptr_t)(start - r.data));
                if (hits.size() >= limit) return hits;
            }
            p = f + 1;
        }
    }
    return hits;
}

Result Resolve(const std::string& platform, const std::string& buildId, const std::vector<Region>& regions,
               bool allowUnpinned, uintptr_t imageBase) {
    auto t0 = std::chrono::steady_clock::now();
    Result res;
    res.platform = platform;
    res.buildId = buildId;
    const auto& sigs = SignaturesFor(platform);
    bool allUnique = !sigs.empty();
    std::string firstFailure;
    for (auto& s : sigs) {
        std::vector<int> pat;
        std::string line = std::string(s.anchor) + ": ";
        if (!ParsePattern(s.pattern, pat)) {
            line += "malformed pattern";
        } else {
            auto hits = FindMatches(regions, pat, 2);
            if (hits.size() != 1) {
                line += hits.empty() ? "no match" : "more than one match";
            } else {
                uintptr_t at = hits[0] + (uintptr_t)s.offset, value = 0;
                int32_t field = 0;
                if (s.capture == Capture::Start) {
                    value = at;
                } else if (!ReadField(regions, at, field)) {
                    line += "capture outside the scanned code";
                } else if (s.capture == Capture::Rip32) {
                    value = at + 4 + (intptr_t)field;
                } else {
                    value = (uint32_t)field;
                }
                if (value) {
                    Slot(res.anchors, s.anchor) = value;
                    line += Hex(value) + " (match at " + Hex(hits[0]) + ")";
                }
            }
        }
        if (line.find("0x") == std::string::npos) {
            allUnique = false;
            if (firstFailure.empty()) firstFailure = line;
        }
        res.details.push_back(line);
    }
    const BuildPin* pin = nullptr;
    for (auto& p : kPinned)
        if (platform == p.platform && buildId == p.buildId) pin = &p;
    if (pin) res.build = pin->build;

    if (sigs.empty()) {
        res.reason = "no signatures for platform " + platform;
    } else if (!pin && !allowUnpinned) {
        std::string builds;
        for (auto& p : kPinned)
            if (platform == p.platform) builds += std::string(builds.empty() ? "" : ", ") + p.build;
        res.reason = "unsupported server build (" + (buildId.empty() ? std::string("no build id") : buildId) +
                     "); this connector is pinned to build " + (builds.empty() ? std::string("(none)") : builds);
        if (!allUnique) res.reason += " (signature scan: " + firstFailure + ")";
    } else if (!allUnique) {
        res.reason = "signature scan failed (" + firstFailure + ")";
    } else if (pin) {
        const Anchors& e = pin->expected;
        if (res.anchors.processEvent - imageBase != e.processEvent ||
            res.anchors.objObjects - imageBase != e.objObjects || res.anchors.nameBlocks - imageBase != e.nameBlocks)
            res.reason = "signature scan disagrees with the pinned addresses of build " + std::string(pin->build);
        else
            res.ok = true;
    } else {
        res.ok = true;
        res.details.push_back("build not pinned; accepted because TAKARO_CONAN_ALLOW_UNPINNED_BUILD=1");
    }
    res.scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

std::string TableJson() {
    std::string o = "{\"signatures\":{";
    bool firstP = true;
    for (const char* platform : {"linux", "windows"}) {
        o += std::string(firstP ? "" : ",") + JsonStr(platform) + ":[";
        firstP = false;
        bool first = true;
        for (auto& s : SignaturesFor(platform)) {
            const char* cap = s.capture == Capture::Start ? "start" : s.capture == Capture::Rip32 ? "rip32" : "abs32";
            o += std::string(first ? "" : ",") + "{\"anchor\":" + JsonStr(s.anchor) + ",\"pattern\":" +
                 JsonStr(s.pattern) + ",\"capture\":" + JsonStr(cap) + ",\"offset\":" + std::to_string(s.offset) +
                 "}";
            first = false;
        }
        o += "]";
    }
    o += "},\"builds\":[";
    bool first = true;
    for (auto& p : kPinned) {
        o += std::string(first ? "" : ",") + "{\"platform\":" + JsonStr(p.platform) + ",\"buildId\":" +
             JsonStr(p.buildId) + ",\"build\":" + JsonStr(p.build) + ",\"anchors\":{\"processEvent\":" +
             JsonStr(Hex(p.expected.processEvent)) + ",\"objObjects\":" + JsonStr(Hex(p.expected.objObjects)) +
             ",\"nameBlocks\":" + JsonStr(Hex(p.expected.nameBlocks)) + "}}";
        first = false;
    }
    return o + "]}";
}

}  // namespace pins
