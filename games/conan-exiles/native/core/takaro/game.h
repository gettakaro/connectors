// The only door from the Takaro protocol half (core/takaro) into the game half (core/conan).
// core/takaro never includes a UE or Conan header; host tests implement this with a fake game.
#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace takaro {

struct ActionResult {
    bool ok = false;
    JsonValue payload;  // ok: the response payload (Null answers {} on protocol 0, null on protocol 1)
    std::string error;  // !ok: the error text Takaro shows
    std::string code;   // !ok: the protocol 1 error code (takaro/protocol.h); empty = game_error
};

struct GameEvent {
    std::string type;  // one of the 6 Takaro event types
    JsonValue data;
};

class Game {
public:
    virtual ~Game() = default;
    // Action worker thread. May wait on the game thread for a bounded time; never touches the socket.
    virtual ActionResult Execute(const std::string& action, const JsonValue& args) = 0;
    // The same on a connection that speaks `protocolVersion` (0 or 1).
    virtual ActionResult Execute(const std::string& action, const JsonValue& args, int protocolVersion) {
        (void)protocolVersion;
        return Execute(action, args);
    }
    // Bridge thread, cheap, never blocks. The protocol 1 Capability Manifest as a JSON array; it may
    // change while running (the map is declared once the world is loaded). "" = offer protocol 0 only.
    virtual std::string CapabilityManifest() { return ""; }
    // Bridge thread. Moves the events produced since the last call into `out`; never blocks.
    virtual void DrainEvents(std::vector<GameEvent>& out) = 0;
    // Any thread. A small JSON object for the health snapshot (pins, readiness, perf).
    virtual std::string HealthJson() = 0;
};

}  // namespace takaro
