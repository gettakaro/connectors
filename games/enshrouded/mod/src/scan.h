// In-memory PE scanning: string anchors in .rdata, RIP-relative xrefs in .text,
// .pdata function-root resolution, direct call targets.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scan {

bool Init();  // parse the main module's headers; false if something is badly off
uintptr_t Base();
uint32_t TimeDateStamp();

// Exact NUL-terminated C string located in a read-only data section. Returns RVA or 0.
uint32_t FindCString(const char* s);
// All RVAs of instructions `REX.W 8D /r` with RIP-relative disp32 pointing at `targetRva`.
std::vector<uint32_t> FindLeaXrefs(uint32_t targetRva);
// All RVAs of E8 (call rel32) instructions whose destination is `targetRva`.
std::vector<uint32_t> FindDirectCalls(uint32_t targetRva);
// E8 call destinations inside [startRva, endRva).
std::vector<uint32_t> CallTargetsIn(uint32_t startRva, uint32_t endRva);
// Walk .pdata (incl. chained unwind info) back to the primary function. Returns begin RVA or 0,
// and optionally the end of the primary RUNTIME_FUNCTION.
uint32_t FunctionRoot(uint32_t rva, uint32_t* primaryEnd = nullptr);
// Byte-pattern compare at an RVA; pattern like "40 53 55 56 48 83 EC ?? 49".
bool MatchAt(uint32_t rva, const char* pattern);
bool InText(uint32_t rva);
// All RVAs in executable sections where `pattern` matches (stops after maxHits).
std::vector<uint32_t> FindPattern(const char* pattern, size_t maxHits = 8);
// Destination of an E8 call at `rva`, or 0 when there is no E8 there.
uint32_t CallDest(uint32_t rva);
// RVA of the main module's IAT slot for dll!fn (case-insensitive dll match), or 0.
uint32_t FindImportSlot(const char* dll, const char* fn);
// All RVAs of `FF 15 rel32` (call [rip+disp]) whose memory operand is `slotRva`.
std::vector<uint32_t> FindIndirectCalls(uint32_t slotRva);

inline void* Ptr(uint32_t rva) { return (void*)(Base() + rva); }

}  // namespace scan
