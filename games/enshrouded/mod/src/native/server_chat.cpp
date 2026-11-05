#include "native/server_chat.h"

#include "native/json_util.h"

namespace native {

std::string ResolveServerChatName(const JsonValue& opts, const std::string& configured) {
    if (auto o = Str(opts.get("senderNameOverride"))) return *o;
    return Trim(configured);
}

std::string RenderServerChat(const std::string& name, const std::string& message) {
    return name.empty() ? message : name + ": " + message;
}

}  // namespace native
