// The only door from the Takaro protocol half (core/takaro) into the game half (core/conan).
// core/takaro never includes a UE or Conan header; host tests implement this with a fake game.
#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace takaro {

struct ActionResult {
    bool ok = false;
    JsonValue payload;  // ok: the response payload (Null answers {})
    std::string error;  // !ok: the error text Takaro shows
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
    // Bridge thread. Moves the events produced since the last call into `out`; never blocks.
    virtual void DrainEvents(std::vector<GameEvent>& out) = 0;
    // Any thread. A small JSON object for the health snapshot (pins, readiness, perf).
    virtual std::string HealthJson() = 0;
};

}  // namespace takaro
