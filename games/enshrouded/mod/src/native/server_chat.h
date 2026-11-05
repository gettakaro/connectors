// The sender name on chat the connector sends. Takaro's senderNameOverride (filled from the game server's
// serverChatName setting by newer Takaro releases) wins over plugin.json "serverChatName" /
// TAKARO_SERVER_CHAT_NAME; neither keeps the text as Takaro sent it. Every sendMessage renders through here.
#pragma once
#include "common.h"

#include <string>

namespace native {

// The name to show, or "" for none. Blank and whitespace-only values count as unset.
std::string ResolveServerChatName(const JsonValue& opts, const std::string& configured);
// The chat text. The game's notice has no sender field, so the name goes in front of the text: players read
// "X: message" in global chat and in a whisper.
std::string RenderServerChat(const std::string& name, const std::string& message);

}  // namespace native
