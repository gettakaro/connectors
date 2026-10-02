// The bridge client: polls GET /mod/poll, runs each command, answers with POST /mod/result.
// Owns one background thread; nothing here runs on the game thread.
#pragma once

#include <functional>

#include "proto.h"

namespace Poller {

struct Config {
    HttpUrl url;
    int intervalMs = 500;      // between empty polls
    int maxBackoffMs = 10000;  // while the bridge is unreachable
    int requestTimeoutMs = 3000;
};

using Executor = std::function<ChatOutcome(const PollCommand&)>;

// Runs one command: sendMessage goes to `chat`, anything else is refused.
ChatOutcome Execute(const PollCommand& cmd, const Executor& chat);

void Start(const Config& cfg, Executor chat);
void Stop();  // signals the thread and waits up to 2 s for it

}  // namespace Poller
