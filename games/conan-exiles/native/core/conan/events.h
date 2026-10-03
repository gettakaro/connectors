// Takaro game events from inside the Conan server (stage 2 lane L2c; method: El-Limon
// context/games/conan-exiles/evidence/spikes/S4-events.md).
//
//   player-connected     BaseGameMode_C.K2_PostLogin (After): Steam64 from PlayerState.UniqueID
//                        (conan/identity.h), name + IP from the PlayerState; cached per controller.
//                        Sent once the controller possesses its character (+500 ms; 120 s cap):
//                        Takaro asks getPlayerLocation right after the event and drops the event
//                        when that fails, which it does while a client is still loading the world.
//                        A player who leaves before spawning produces neither event.
//   player-disconnected  K2_OnLogout (Before): the PostLogin cache (PlayerState is already null).
//   chat-message         ConanPlayerController.ServerSendChatMessage (Before): Steam64 from the
//                        called controller, channel + text from ChatRpcData. Stage 1 sendMessage
//                        calls ClientReceiveChatMessage through the trampoline, so it never loops.
//   player-death         EventOnDeath with NewState Dead (1) from another state, on a character
//                        controlled by a ConanPlayerController; deduped per actor for 5 s. Killer
//                        from OnOwnerKilled, cause/instigator from the last ReceiveAnyDamage /
//                        ReceivePointDamage on the victim (<= 250 ms, else damage arriving within
//                        60 ms after). Names are read inside the hook while the actors are alive.
//   entity-killed        the same transition on a non-player character whose last damage was
//                        instigated by a ConanPlayerController (thrall and NPC kills are ignored,
//                        as the old bridge did). Entity = the character's display name (spawn-table
//                        row name cache, m_CharacterName, SpawnDataTable row); weapon = the item
//                        name of DamageCauser.OwnerItem (GameItem.GetItemName), "Unarmed" for fists.
//   log                  conan/logtail.h.
//
// The game thread only runs the hook bodies (a few reads and an enqueue; microseconds) and the
// rare re-read jobs. JSON, the whitelist and the hand-off to the outbox run on the events worker.
#pragma once

#include "takaro/game.h"

#include <functional>
#include <string>
#include <vector>

namespace conan {

struct EventsOptions {
    std::function<void(takaro::GameEvent)> emit;  // Adapter::Emit (durable outbox via the bridge)
    std::string savedDir;                         // ConanSandbox/Saved
    std::string healthFile;                       // events-health.json (written every 10 s)
    std::vector<std::string> secrets;             // scrubbed from forwarded log lines
    bool hooks = true;                            // false on an unverified build: log tail only
};

void StartEvents(const EventsOptions& o);
void StopEvents();
std::string EventsHealthJson();

}  // namespace conan
