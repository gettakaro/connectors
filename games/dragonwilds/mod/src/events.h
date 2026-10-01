// Event sources (lane L2). Everything here pushes into PluginState::EmitEvent(Deferred); the native
// Takaro bridge and the diagnostic GET /events read the ring buffer and know nothing about sources.
//
// Contract:
//   * Init() runs on the plugin init thread, after Sym/Reflect/GameThread are up. It may install
//     hooks and must set one capability per event type via PluginState::SetCapability(),
//     degrading with a reason instead of failing.
//   * Housekeep() runs every 2 s on the housekeeping thread: poll the log tail, and - only when a
//     phase has work - enter the game thread once to re-hook late-loaded subclasses, resolve joins,
//     reap connections and sample health edges. It must never touch UObjects directly.
//   * Hooks only capture owned primitives; JSON is rendered later on a background reader.
//   * Emitted types and payloads are specified in docs/API.md (player-connected,
//     player-disconnected, chat-message, player-death, entity-killed, log).
#pragma once
#include "common.h"

#include <functional>

namespace Events {
void Init();
void Housekeep();
std::string DiagnosticsJson();
// True when the PreLogin hook is installed, i.e. a ban refuses a rejoin on the running server.
bool BanEnforcementLive();
// Complete raw lines from the background log tailer. The sink only enqueues owned data; its
// consumer must redact before persisting or forwarding. With a sink set, the tailer no longer
// emits `log` ring events itself.
void SetRawLogSink(std::function<void(std::string)> sink);
}  // namespace Events
