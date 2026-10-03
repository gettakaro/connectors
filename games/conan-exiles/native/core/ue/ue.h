// Unreal reflection for the Conan Exiles dedicated server.
//
// The raw globals (GUObjectArray.ObjObjects, the FNamePool block table, ProcessEvent) come from the
// startup pins (core/pins), which refuse any build they cannot verify, so nothing here is used on an
// unverified build. Everything above the core layout (property offsets, the chat RPC, the
// GameState) is found by reflection at runtime and type-checked.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace UE {

using ProcessEventFn = void (*)(void* obj, void* func, void* parms);

// Called once at startup, before any other function here, with the pinned addresses and the
// platform's way to call the original ProcessEvent (the detour's trampoline).
void SetGlobals(uintptr_t objObjects, uintptr_t nameBlocks, ProcessEventFn callProcessEvent);
bool HaveGlobals();
// Game thread only: calls the original (un-detoured) ProcessEvent.
void CallProcessEvent(void* obj, void* func, void* parms);

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
