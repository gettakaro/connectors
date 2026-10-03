// Anchor fixture tests: the production signature scan (core/pins) over windows of real server
// binaries (*.anchors, written by tools/repin.py), so the runner proves match-exactly-once and the
// refusal rules without the 200 MB binary. The fixtures hold server machine code and are kept out of
// the public repo: tests/run.sh runs this only when CONAN_ANCHOR_FIXTURES names the private copy. Each fixture holds the real
// bytes around every signature's match plus its nearest decoys from the full .text.
//
// Generic over the number of anchors: anchor values are read back from Resolve()'s detail lines
// ("<anchor>: 0x... (match at 0x...)"), so a fourth anchor needs no change here.
// Usage: pins_fixture_test <fixture>...
#include "pins/pins.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_fail = 0, g_pass = 0;

void Check(bool ok, const std::string& what) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what.c_str());
    }
}

struct Fixture {
    std::string file, platform, buildId, build;
    uintptr_t imageBase = 0;                  // PE fixtures: addresses are VAs, pinned anchors RVAs
    std::map<std::string, uintptr_t> expect;  // absolute (VA) after Load
    std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> bytes;  // merged contiguous regions
    std::vector<pins::Region> regions;
};

bool Load(const char* path, Fixture& f) {
    std::ifstream in(path);
    if (!in) return false;
    f.file = path;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "platform") ss >> f.platform;
        else if (key == "buildid") ss >> f.buildId;
        else if (key == "build") ss >> f.build;
        else if (key == "imagebase") {
            std::string v;
            ss >> v;
            f.imageBase = (uintptr_t)strtoull(v.c_str(), nullptr, 16);
        }
        else if (key == "expect") {
            std::string name, v;
            ss >> name >> v;
            f.expect[name] = (uintptr_t)strtoull(v.c_str(), nullptr, 16);
        } else if (key == "region") {
            std::string a, hex;
            ss >> a >> hex;
            uintptr_t addr = (uintptr_t)strtoull(a.c_str(), nullptr, 16);
            std::vector<uint8_t> data;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) data.push_back((uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
            if (!f.bytes.empty() && f.bytes.back().first + f.bytes.back().second.size() == addr)
                f.bytes.back().second.insert(f.bytes.back().second.end(), data.begin(), data.end());
            else
                f.bytes.push_back({addr, data});
        }
    }
    for (auto& b : f.bytes) f.regions.push_back({b.second.data(), b.second.size(), b.first});
    for (auto& kv : f.expect) kv.second += f.imageBase;  // the fixture's expect lines are RVAs on a PE
    return !f.platform.empty() && !f.regions.empty();
}

std::map<std::string, uintptr_t> Found(const pins::Result& r) {
    std::map<std::string, uintptr_t> out;
    for (auto& d : r.details) {
        size_t colon = d.find(": 0x");
        if (colon == std::string::npos) continue;
        out[d.substr(0, colon)] = (uintptr_t)strtoull(d.c_str() + colon + 2, nullptr, 16);
    }
    return out;
}

const pins::BuildPin* Pinned(const std::string& platform, const std::string& id) {
    for (auto& p : pins::PinnedBuilds())
        if (platform == p.platform && id == p.buildId) return &p;
    return nullptr;
}

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

std::string Hex(uintptr_t v) {
    char b[32];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

void TestFixture(const Fixture& f, const std::vector<Fixture>& all) {
    const std::string tag = f.platform + " build " + f.build + ": ";
    const bool resolvable = !f.expect.empty();
    const size_t nsig = pins::SignaturesFor(f.platform).size();

    // 1. As shipped: the real build-id.
    pins::Result r = pins::Resolve(f.platform, f.buildId, f.regions, false, f.imageBase);
    if (Pinned(f.platform, f.buildId)) {
        Check(r.ok, tag + "pinned build accepted (" + r.reason + ")");
        Check(Found(r) == f.expect, tag + "scan reproduces the pinned anchors");
        printf("  %s pinned build %s: %zu/%zu signatures match exactly once and reproduce the pinned anchors (%.2f ms)\n",
               r.ok && Found(r) == f.expect ? "PASS" : "FAIL", f.build.c_str(), Found(r).size(), nsig, r.scanMs);
    } else {
        Check(!r.ok && Has(r.reason, "unsupported server build"), tag + "unpinned build refused: " + r.reason);
        printf("  %s unpinned build %s refused: %s\n", !r.ok ? "PASS" : "FAIL", f.build.c_str(), r.reason.c_str());
    }

    // 2. Scan only (TAKARO_CONAN_ALLOW_UNPINNED_BUILD=1): the signatures follow the build or fail cleanly.
    r = pins::Resolve(f.platform, "fixture-unpinned", f.regions, true, f.imageBase);
    if (resolvable) {
        Check(r.ok && Found(r) == f.expect, tag + "scan of an unpinned build finds the fixture's anchors");
        std::string got;
        for (auto& kv : Found(r)) got += " " + kv.first + "=" + Hex(kv.second);
        printf("  %s build %s scan without a pin:%s\n", r.ok ? "PASS" : "FAIL", f.build.c_str(), got.c_str());
    } else {
        Check(!r.ok && Has(r.reason, "signature scan failed") && Has(r.reason, "no match"),
              tag + "build without matches refused even when unpinned builds are allowed: " + r.reason);
        printf("  %s build %s (signatures gone) refused even with unpinned builds allowed: %s\n", !r.ok ? "PASS" : "FAIL",
               f.build.c_str(), r.reason.c_str());
    }

    // 3. Wrong build: same bytes, an identity nobody pinned. No hook, structured reason.
    r = pins::Resolve(f.platform, "0000000000000000000000000000000000000000", f.regions, false, f.imageBase);
    Check(!r.ok && Has(r.reason, "unsupported server build") && Has(r.reason, "0000000000"), tag + "wrong build-id refused");

    if (!resolvable) return;

    // 4. A second copy of a match: "exactly once" must refuse, never pick one.
    std::vector<pins::Region> dup = f.regions;
    for (auto& reg : f.regions) {
        bool holds = false;
        for (auto& kv : f.expect) holds = holds || (kv.second >= reg.address && kv.second < reg.address + reg.size);
        if (holds) {
            dup.push_back({reg.data, reg.size, reg.address + 0x10000000});
            break;
        }
    }
    r = pins::Resolve(f.platform, f.buildId, dup, true, f.imageBase);
    Check(!r.ok && Has(r.reason, "more than one match"), tag + "duplicated match refused: " + r.reason);
    printf("  %s build %s with a duplicated match refused: %s\n", !r.ok ? "PASS" : "FAIL", f.build.c_str(), r.reason.c_str());

    // 5. Every literal byte of the code gone: no match, refused.
    std::vector<std::vector<uint8_t>> wiped;
    std::vector<pins::Region> blank;
    for (auto& reg : f.regions) {
        wiped.emplace_back(reg.size, 0xCC);
        blank.push_back({wiped.back().data(), reg.size, reg.address});
    }
    r = pins::Resolve(f.platform, f.buildId, blank, true, f.imageBase);
    Check(!r.ok && Has(r.reason, "no match"), tag + "wiped code refused: " + r.reason);

    // 6. A pinned identity with another build's bytes: the oracle catches moved anchors.
    for (auto& p : pins::PinnedBuilds()) {
        if (p.platform != f.platform || p.buildId == f.buildId) continue;
        const Fixture* pf = nullptr;
        for (auto& g : all) pf = (g.buildId == p.buildId) ? &g : pf;
        if (!pf || pf->expect == f.expect) continue;
        r = pins::Resolve(f.platform, p.buildId, f.regions, false, f.imageBase);
        Check(!r.ok && Has(r.reason, "disagrees with the pinned addresses"),
              tag + "bytes of build " + f.build + " under the identity of " + p.build + " refused: " + r.reason);
        printf("  %s bytes of build %s under the pinned identity of build %s refused: %s\n", !r.ok ? "PASS" : "FAIL",
               f.build.c_str(), p.build, r.reason.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<Fixture> all(argc > 1 ? (size_t)argc - 1 : 0);
    for (int i = 1; i < argc; i++) {
        if (!Load(argv[i], all[(size_t)i - 1])) {
            printf("FAIL cannot read fixture %s\n", argv[i]);
            return 1;
        }
    }
    if (all.empty()) {
        printf("FAIL no fixtures given\n");
        return 1;
    }
    // Every pinned build must have a fixture (repin.py fixtures checks the values too).
    for (auto& p : pins::PinnedBuilds()) {
        bool have = false;
        for (auto& f : all) have = have || (f.platform == p.platform && f.buildId == p.buildId);
        Check(have, std::string("pinned ") + p.platform + " build " + p.build + " has an anchor fixture");
    }
    for (auto& f : all) TestFixture(f, all);
    // A platform without signatures refuses whatever it is given.
    pins::Result w = pins::Resolve("macos", "anything", all[0].regions, true);
    Check(!w.ok && Has(w.reason, "no signatures"), "a platform without signatures refused: " + w.reason);
    printf("%s anchor fixtures: %d/%d checks over %zu fixture(s)\n", g_fail ? "FAIL" : "PASS", g_pass, g_pass + g_fail,
           all.size());
    return g_fail ? 1 : 0;
}
