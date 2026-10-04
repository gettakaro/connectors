// Layout probes for the pinned, non-reflected offsets (ComponentToWorld, the GameItem stat arrays,
// the FText data layouts). They run only after a startup self-check failed, read through a safe
// UE::Mem, and log where the engine's answer actually sits, so a re-pin (a new build, or the
// MSVC layout on Windows) can take the offset from one native log instead of a debugger session.
#pragma once

#include "ue/mem.h"

#include <cstdint>
#include <string>

namespace conan {

// Hex of n bytes at addr ("unreadable" when any byte cannot be read).
std::string HexBytes(const UE::Mem& m, uintptr_t addr, size_t n);

// Offsets in [0x100, 0x400) of `root` holding three doubles equal to `engine` (within 2 units).
std::string ProbeVector(const UE::Mem& m, uintptr_t root, const double engine[3]);

// TArray headers in [0xC0, 0x240) of the item that look like stat arrays, with their first bytes,
// plus the engine values to look for.
std::string ProbeStats(const UE::Mem& m, uintptr_t item, int32_t stack, float dur, float maxDur);

// Where the engine's display string of the FText at `ftextAddr` sits: an FString directly in the
// text data object, or one or two pointers further.
std::string ProbeText(const UE::Mem& m, uintptr_t ftextAddr, const std::string& expected);

}  // namespace conan
