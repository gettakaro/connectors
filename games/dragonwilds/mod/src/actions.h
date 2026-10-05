// Background-callable action handlers shared by the native Takaro bridge and diagnostic HTTP.
// Each returns an HTTP-style status plus a JSON body. UObject reads/calls run in owned
// GameThread::Run jobs that return copied fields; response encoding and plugin persistence run on
// the caller's background thread.
//
// Contract for L3:
//   * Init() runs on the plugin init thread; register one capability per action with
//     PluginState::SetCapability (players, playerLocation, playerInventory, giveItem, listItems,
//     listEntities, listLocations, sendMessage, teleport, kick, ban, unban, listBans,
//     executeCommand, shutdown).
//   * Player snapshots stay lazy; no per-tick catalogue work.
//   * Every handler returns {status, body} and never throws.
#pragma once
#include "common.h"

namespace Actions {

struct Result {
    int status = 501;
    std::string body = "{\"error\":\"unimplemented\"}";
};

void Init();
void Housekeep();

Result Players();
Result Player(const std::string& gameId);
Result PlayerLocation(const std::string& gameId);
Result PlayerInventory(const std::string& gameId);
Result Items(const std::string& search);
Result Entities();
Result Locations();
Result Bans();
Result Message(const JsonValue& body);
Result Teleport(const JsonValue& body);
Result Give(const JsonValue& body);
Result Kick(const JsonValue& body);
Result Ban(const JsonValue& body);
Result Unban(const JsonValue& body);
// Expiry must recheck the captured revision on the game thread, before mutation.
Result UnbanIfRevision(const JsonValue& body, uint64_t expectedRevision);
// Includes cancelled queued jobs until the pump releases them, and started jobs that outlive a
// caller timeout. Ban recovery must wait for this to reach zero.
size_t PendingBanJobs();
Result Command(const JsonValue& body);
Result Shutdown();

// Kicks an online player that the plugin's ban list refuses. Must be called on the game thread
// (lane L2's join resolver already is); a no-op when the player is not online.
bool KickBanned(const std::string& gameId);

// The SteamID64 held in a player state's PlatformData, or "" when none is there. Game thread only.
// The event path and the players snapshot must agree, so both use this one reader.
std::string SteamIdOfPlayerState(void* playerState);

// Debug only (TAKARO_PLUGIN_DEBUG=1): kills the nearest AI character to a player through the game's
// own damage pipeline, with that player as the instigator. It exists so that entity-killed can be
// proven without a human swinging a sword.
Result KillNearest(const JsonValue& body);

}  // namespace Actions
