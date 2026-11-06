#pragma once
// The sender name on chat the connector sends, kept free of game types so the unit tests can
// exercise it. Every send path resolves and renders the sender through here.
#include "common.h"

#include <string>

namespace ChatSender {

inline std::string Clean(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// TAKARO_SENDER_NAME, else its alias TAKARO_SERVER_CHAT_NAME; empty when neither holds a name.
inline std::string Configured() {
    std::string name = Clean(ConfigValue("TAKARO_SENDER_NAME"));
    return name.empty() ? Clean(ConfigValue("TAKARO_SERVER_CHAT_NAME")) : name;
}

// Takaro's senderNameOverride (filled from the server's serverChatName setting by newer Takaro
// releases), then the configured name, then the server name, then "Server". Empty or whitespace
// counts as unset at every step.
inline std::string Resolve(const std::string& senderNameOverride, const std::string& configured,
                           const std::string& serverName) {
    for (const std::string* v : {&senderNameOverride, &configured, &serverName}) {
        std::string name = Clean(*v);
        if (!name.empty()) return name;
    }
    return "Server";
}

// The client labels a message with the receiving player's own name (it renders the label from
// the sender net id, and the server has none), so the sender goes in front of the text.
inline std::string Render(const std::string& senderName, const std::string& text) {
    return senderName.empty() ? text : "[" + senderName + "] " + text;
}

}  // namespace ChatSender
