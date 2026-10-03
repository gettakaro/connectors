// Runtime auto-detection of the three raw engine globals on the Windows server, for re-pinning only
// (TAKARO_CONAN_REPIN=1). The shipped path is the signature scan in core/pins; this finds the anchors
// of a build that has no signatures yet, the way Dumper-7 does, so tools/sigderive.py --pe can mint
// them:
//   - GUObjectArray.ObjObjects: a {Objects**, Num, Max, NumChunks, MaxChunks} block in a writable
//     section whose items carry their own index (UObject +0xC) and a vtable in the image;
//   - FNamePool blocks: a block table with {CurrentBlock, Cursor} just before it, where FName 0
//     decodes to "None" and FName 1 to "ByteProperty";
//   - UObject::ProcessEvent: the vtable slot of a live UObject whose code tests FUNC_Native (0x400)
//     in UFunction::FunctionFlags (+0xB0), near the Linux slot (78; MSVC has one destructor slot,
//     Itanium two).
// Every read is checked against VirtualQuery, so a wrong guess cannot fault the server.
#pragma once

#include <cstdint>
#include <string>

namespace winplat {

struct AutoAnchors {
    uintptr_t objObjects = 0, nameBlocks = 0, processEvent = 0;
    int processEventSlot = -1;
    int32_t objectCount = 0;
    std::string report;  // human-readable, one line per finding (RVAs, candidates, first bytes)
};

// Waits (polling every second, up to `timeoutMs`) until the object array is populated, then detects.
// Worker thread only. Returns false when an anchor was not found; `out.report` says which.
bool AutoDetectAnchors(AutoAnchors& out, unsigned timeoutMs);

// The ANSI text of FName `id` in the pool at `nameBlocks` ("" for a wide or unreadable entry).
std::string NameText(uintptr_t nameBlocks, uint32_t id);
// The name of a UObject (+0x18) and of its class.
std::string ObjectName(uintptr_t nameBlocks, uintptr_t obj);
std::string ClassName(uintptr_t nameBlocks, uintptr_t obj);

// Re-pin diagnostic: finds the UClass named `className` in the object array and returns a dump of its
// layout (SuperStruct, ChildProperties and, per FField, the raw header plus the names at the offsets
// core/ue reads), to cross-check the Linux reflection layout on this build.
std::string DumpClass(uintptr_t objObjects, uintptr_t nameBlocks, const char* className);

// Re-pin diagnostic for player identity: for every live PlayerState, the strings reachable from
// PlayerState.UniqueID (FUniqueNetIdRepl) within two pointer hops that look like a Steam64
// ("7656119...", ANSI or UTF-16), with the path that reached them, plus UserIDFromURLOptions of
// the owning controller and PlayerNamePrivate. "" when no PlayerState is live.
std::string ProbePlayerIds(uintptr_t objObjects, uintptr_t nameBlocks);

// Readable for `len` bytes (committed, not a guard page, not PAGE_NOACCESS).
bool Readable(uintptr_t a, size_t len);

}  // namespace winplat
