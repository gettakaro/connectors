// Background-callable action handlers shared by native Takaro and diagnostic HTTP.
// UObject reads/calls use owned GameThread::Run jobs. Jobs return copied fields;
// response encoding and plugin persistence run on the caller's background thread.
//
// Contract for L3:
//   * Init() runs on the plugin init thread; register one capability per action with
//     PluginState::SetCapability (players, playerLocation, playerInventory, giveItem, listItems,
//     listEntities, listLocations, sendMessage, teleport, kick, ban, unban, listBans,
//     executeCommand, shutdown).
//   * Player snapshots stay lazy and cached; no new per-tick catalogue work.
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
// Includes cancelled queued jobs until the pump releases them, and started jobs
// that outlive a caller timeout. Ban recovery must wait for this to reach zero.
size_t PendingBanJobs();
Result Command(const JsonValue& body);
Result Shutdown();

// Kicks an online player that the plugin's ban list refuses. Must be called on the game thread
// (lane L2's join resolver already is); a no-op when the player is not online.
bool KickBanned(const std::string& gameId);

// Lane L2c: the display name of the UItem an `AEquippedItem` actor represents ("Baseball Bat"),
// or "" when `actor` is not an equipped item / carries no resolvable item. Reads UObjects by
// reflection, so it must be called ON THE GAME THREAD; entity-killed uses it to name the weapon
// the killer swung instead of printing the DamageCauser actor's class.
std::string EquippedItemName(void* actor);

// Debug only (TAKARO_PLUGIN_DEBUG=1): kills the nearest AI character to a player through the game's
// own damage pipeline, with that player as the instigator. It exists so that entity-killed can be
// proven without a human swinging a sword.
Result KillNearest(const JsonValue& body);

// Debug only (TAKARO_PLUGIN_DEBUG=1), lane L3f / finding F19: shows, for one player, the
// controller's current pawn, the player state's PawnPrivate, the character id, the single
// inventory component the plugin now answers from, and every UBaseInventoryComponent the OLD
// pawn+controller sweep would have found (with its owner and whether that owner is the current
// pawn). This is the endpoint that makes "Takaro is reading the wrong container" a measurement
// instead of a hypothesis.
Result DebugInventories(const std::string& gameId);

}  // namespace Actions
