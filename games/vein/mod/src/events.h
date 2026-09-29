// Event sources (lane L2). Everything here pushes into PluginState::EmitEvent(type, dataJson);
// GET /events serves the ring buffer and knows nothing about the sources.
//
// Contract for L2:
//   * Init() runs on the plugin init thread, after Sym/Reflect/GameThread are up. It may install
//     hooks (Hooks::HookProcessEvent / HookVTableSymbol) and must set one capability per event type
//     via PluginState::SetCapability(), degrading with a reason instead of failing.
//   * Housekeep() runs every 2 s on the housekeeping thread: re-hook late-loaded subclasses,
//     expire dedupe windows, poll the log tail. It must never touch UObjects directly - use
//     GameThread::Run().
//   * Emitted types and payloads are specified in docs/API.md (player-connected,
//     player-disconnected, chat-message, player-death, entity-killed, log).
#pragma once
#include "common.h"

#include "actions.h"  // lane L2: Actions::Result is the return type of KillNearest below
#include <functional>

namespace Events {
void Init();        // TODO(L2): install PostLogin / PreLogout / Logout / chat / death / kill hooks
void Housekeep();   // TODO(L2): log tail + re-hook sweep + dedupe expiry
std::string DiagnosticsJson();
// True when the PreLogin hook is installed, i.e. a ban refuses a rejoin on the running server.
bool BanEnforcementLive();
// Complete raw lines from the existing background tailer. The sink only enqueues
// owned data; its consumer must redact before persisting or forwarding.
void SetRawLogSink(std::function<void(std::string)> sink, bool customJoin = false, bool customChat = false);

// ---- added by lane L2 (never a change to the signatures above) ---------------------------------
// POST /debug/kill-nearest {gameId?, radius?}: kills the AI nearest to a player by driving
// UGameplayStatics::ApplyDamage through the game's own pipeline, so entity-killed can be proven
// without a human swinging a weapon. Lives here because the damage pipeline, the AI class set and
// the entity-killed hook are all L2's; http.cpp routes the endpoint through Actions::KillNearest,
// which should forward to this. Debug-gated by http.cpp (token + TAKARO_PLUGIN_DEBUG=1).
Actions::Result KillNearest(const JsonValue& body);

// GET /debug/nearby?gameId=&radius=: read-only inventory of the AI around a player (lane L2b).
Actions::Result Nearby(const std::string& gameId, double radius);
}  // namespace Events
