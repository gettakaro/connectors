// Unreal reflection for the Conan Exiles Linux dedicated server, build 25639945 only.
//
// The server binary is stripped and non-PIE, so the globals below are fixed addresses. main.cpp
// refuses to hook unless the build-id and the ProcessEvent prologue match, so these are never
// dereferenced on another build. Everything above the core layout (property offsets, the chat
// RPC, the GameState) is found by reflection at runtime and type-checked.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace UE {

#ifdef TAKARO_DEBUG_WRONG_BUILD_ID
constexpr const char* kBuildId = "0000000000000000000000000000000000000000";  // degrade proof only
#else
constexpr const char* kBuildId = "3a05a6ef0c873f2bbf754ec495bdf2a686d3768d";
#endif
constexpr uintptr_t kProcessEvent = 0x3f12340;
constexpr uintptr_t kGObjObjects = 0xc35a580;  // FChunkedFixedUObjectArray
constexpr uintptr_t kNameBlocks = 0xc2a5d40;   // FNamePool blocks; CurrentBlock/Cursor just before
constexpr uint8_t kProcessEventPrologue[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                               0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00};

// Game thread only (the engine appends to the name pool from it). One linear scan of the pool,
// repeated only until every name the library needs exists.
bool ResolveNames();

// Game thread only. Walks the object array in slices of 16384 objects per call, looking for the
// chat RPC and the live GameState(s). Returns true once a full pass is complete.
bool DiscoverStep();
// Game thread only. True if the last full discovery found what sendMessage needs and those
// objects are still alive.
bool Ready();
// Restarts discovery (after the world changed).
void ResetDiscovery();
std::string DiscoveryError();

struct Controller {
    uintptr_t object;
    std::string userId;      // ConanPlayerController.UserIDFromURLOptions (Steam64)
    std::string playerName;  // PlayerState.PlayerNamePrivate
};
// Game thread only, after Ready(): the player controllers of every player in the GameState.
std::vector<Controller> OnlineControllers();
// The ClientReceiveChatMessage UFunction (game thread only, after Ready()).
void* ChatFunction();

}  // namespace UE
