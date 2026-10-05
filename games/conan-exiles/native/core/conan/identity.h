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
    std::string how;      // "+0x8 -> fstring@+0x10", "+0x8 -> uint64@+0x18", ... or why it failed
};
// FUniqueNetIdRepl layout (build 25639945, live): +0 vtable, +8 the TSharedPtr object, +0x10 its
// reference controller. The object at +8 is read first: its id FString slots (Linux clang +0x10,
// then the MSVC candidates), then a scan of the object for a uint64 Steam64 or a "STEAM:..."
// FString. The other two words are scanned last, so a layout shift degrades to a miss, not a crash.
constexpr int32_t kNetIdObject = 8;
constexpr int32_t kNetIdStringSlots[4] = {0x10, 0x18, 0x08, 0x20};

// `uniqueIdAddr` = PlayerState + offset of UniqueID. Any thread (all reads go through `m`).
UniqueIdProbe Steam64FromUniqueId(const UE::Mem& m, uintptr_t uniqueIdAddr);

// The gameId rule above.
std::string GameIdFrom(const UniqueIdProbe& uniqueId, const std::string& userIdFromUrl);

}  // namespace conan
