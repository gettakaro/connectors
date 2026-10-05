// sendMessage: renders a Takaro line in the vanilla Conan chat feed by calling
// ConanPlayerController::ClientReceiveChatMessage on the game thread (stage 1, proven on 25639945).
#pragma once

#include <string>

namespace conan {

struct ChatRequest {
    std::string message;
    std::string recipient;  // normalised: Steam64 or a character name; empty = everyone
    std::string sender;     // shown as the chat user name
};

struct ChatOutcome {
    bool success = false;
    std::string error;
    int online = 0;
    int delivered = 0;  // controllers the line was sent to
    double gameThreadMs = 0;
};

// Action worker thread. Waits for the game thread (bounded); never throws.
ChatOutcome SendChat(const ChatRequest& req);

}  // namespace conan
