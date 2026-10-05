// Startup pins: the three raw engine globals that reflection cannot find by itself
// (UObject::ProcessEvent, GUObjectArray.ObjObjects and the FNamePool block table) are located by
// a byte-signature scan over the server's own code, then checked against the pinned table for
// the server build. Everything else is found by UE reflection at runtime.
//
// A build is accepted only when every signature matches exactly once AND the server build is in
// the pinned table with the same addresses (the oracle). Anything else is an unknown build: the
// library installs no hook, still connects to Takaro, sends one critical notice and refuses every
// action with a structured error. Portable: the platform layer supplies the code regions and the
// build identity (GNU build-id on Linux, PE identity on Windows).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pins {

enum class Capture {
    Start,  // the anchor is the match address + offset (a function)
    Rip32,  // a RIP-relative disp32 at match + offset; anchor = field + 4 + disp
    Abs32,  // an absolute 32-bit address at match + offset (non-PIE images)
};

struct Signature {
    const char* anchor;   // "processEvent" | "objObjects" | "nameBlocks"
    const char* pattern;  // "48 8B ?? ..." (upper/lower hex, ?? = any byte)
    Capture capture;
    int offset;
};

struct Anchors {
    uintptr_t processEvent = 0;
    uintptr_t objObjects = 0;  // FChunkedFixedUObjectArray (GUObjectArray.ObjObjects)
    uintptr_t nameBlocks = 0;  // FNamePool block pointer table; CurrentBlock/Cursor just before
};

struct BuildPin {
    const char* platform;  // "linux" | "windows"
    const char* buildId;   // GNU build-id (linux) / PE identity "<TimeDateStamp>-<SizeOfImage>" (windows)
    const char* build;     // Steam build id, for people
    Anchors expected;      // linux: absolute (non-PIE); windows: RVAs (ASLR), compared after subtracting the base
};

// One readable code region of the server image, as mapped in this process.
struct Region {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uintptr_t address = 0;  // runtime address of data[0]
};

struct Result {
    bool ok = false;
    std::string platform, buildId, build;  // build: the Steam build when pinned
    std::string reason;                    // why the build was refused (empty when ok)
    Anchors anchors;                       // what the scan found (partial when refused)
    std::vector<std::string> details;      // one line per signature
    double scanMs = 0;
};

// The 20-byte ProcessEvent prologue the inline detour relocates: whole instructions, none
// RIP-relative. The hook refuses any other prologue.
constexpr size_t kProcessEventPrologueLen = 20;
extern const uint8_t kProcessEventPrologue[kProcessEventPrologueLen];

const std::vector<Signature>& SignaturesFor(const std::string& platform);
const std::vector<BuildPin>& PinnedBuilds();

// Parses a pattern. false on a malformed token or an empty pattern.
bool ParsePattern(const std::string& pattern, std::vector<int>& out);
// Matches of `pattern` across `regions`, stopping after `limit`. Returns the match addresses.
std::vector<uintptr_t> FindMatches(const std::vector<Region>& regions, const std::vector<int>& pattern, size_t limit);

// Scans and checks. `allowUnpinned` (TAKARO_CONAN_ALLOW_UNPINNED_BUILD=1, re-pin work only)
// accepts a build that is not in the table when every signature still matches exactly once.
// `imageBase` is the runtime base of a relocated image (Windows): the found anchors stay absolute,
// the pinned ones are RVAs and are compared as found - imageBase. 0 for the non-PIE Linux image.
Result Resolve(const std::string& platform, const std::string& buildId, const std::vector<Region>& regions,
               bool allowUnpinned, uintptr_t imageBase = 0);

// The compiled-in table as JSON, for the drift test against pins.json.
std::string TableJson();

}  // namespace pins
