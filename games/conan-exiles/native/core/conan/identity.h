// Player identity: the Steam64 that Takaro uses as gameId.
//
// ConanPlayerController.UserIDFromURLOptions is NOT always the Steam64: on a fresh server/account
// it holds the Funcom Live Services id (`A-1HFFLI28NN`, lane L3 on Windows), while the server logs
// `Login request: userId: STEAM:7656... platform: Fls`. The Steam64 lives in the player's
// PlayerState.UniqueID (FUniqueNetIdRepl, reflected StructProperty, 48 bytes). Its first 16 bytes
// are a TSharedPtr<const FUniqueNetId>; the FUniqueNetId object is not reflected, so the helper
// reads it through a safe reader (UE::Mem: process_vm_readv) and accepts either a Steam64 stored
// as a uint64, or an FString that contains "STEAM:7656..." / "7656...". Nothing here can crash on
// a stale pointer.
//
// Order used by every lane: UniqueID first, then UserIDFromURLOptions when that itself is a Steam64.
#pragma once

#include "ue/mem.h"

#include <cstdint>
#include <string>

namespace conan {

// "STEAM:76561198000735875", "steam:7656...", "7656..." (17 digits) -> the Steam64; else "".
std::string ExtractSteam64(const std::string& s);
bool IsSteam64Number(uint64_t v);

struct UniqueIdProbe {
    std::string steam64;  // "" when not found
    std::string how;      // "uint64@+0x18", "fstring@+0x20 \"STEAM:...\"", or why it failed
};
// `uniqueIdAddr` = PlayerState + offset of UniqueID. Any thread (all reads go through `m`).
UniqueIdProbe Steam64FromUniqueId(const UE::Mem& m, uintptr_t uniqueIdAddr);

}  // namespace conan
