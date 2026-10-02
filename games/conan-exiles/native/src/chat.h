// sendMessage: renders a Takaro line in the vanilla Conan chat feed by calling
// ConanPlayerController::ClientReceiveChatMessage on the game thread.
#pragma once

#include "proto.h"

namespace Chat {

// Poller thread. Waits for the game thread; never throws.
ChatOutcome Send(const PollCommand& cmd);

}  // namespace Chat
