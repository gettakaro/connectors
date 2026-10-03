// Player identity: the one place that decides a player's Takaro gameId (the Steam64).
//
// ConanPlayerController.UserIDFromURLOptions is NOT always the Steam64: for a Funcom Live Services
// account it holds the FLS id (`A-1HFFLI28NN`, seen on the fresh Windows server), while the server
// logs `Login request: userId: STEAM:7656... platform: Fls`. The Steam64 lives in
// PlayerState.UniqueID (FUniqueNetIdRepl, reflected StructProperty, 48 bytes): a vtable, then a
// TSharedPtr whose object (+8) is an FUniqueNetIdString with the id FString at +0x10 on the Linux
// build (live 2026-10-03: "76561198000735875"). That object is not reflected, so it is read through
// a safe reader (UE::Mem) and nothing here can crash on a stale pointer.
//
// Every path uses this: getPlayers/getPlayer (reads), events, bans, kick, give, teleport and the
// directed sendMessage. Rule: Steam64 from UniqueID, else UserIDFromURLOptions when it holds a
// Steam64, else the UniqueID string, else UserIDFromURLOptions (so ids stay consistent even for a
// player without a Steam64).
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
    std::string raw;      // the first printable id string found (may be the Steam64 with a prefix)
    std::string how;      // "fstring@+0x10", "uint64@+0x18", ... or why it failed
};
// FUniqueNetIdRepl layout: the TSharedPtr object pointer at +8; the id FString slots tried first
// (Linux clang +0x10, then the MSVC candidates), then a generic scan of the object.
constexpr int32_t kNetIdObject = 8;
constexpr int32_t kNetIdStringSlots[4] = {0x10, 0x18, 0x08, 0x20};

// `uniqueIdAddr` = PlayerState + offset of UniqueID. Any thread (all reads go through `m`).
UniqueIdProbe Steam64FromUniqueId(const UE::Mem& m, uintptr_t uniqueIdAddr);

// The gameId rule above.
std::string GameIdFrom(const UniqueIdProbe& uniqueId, const std::string& userIdFromUrl);

}  // namespace conan
