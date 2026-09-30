// The only door from the native Takaro connector (src/native/) into the game plugin.
//
// native/ never includes world.h or hooks.h. It reaches game truth through this interface, which
// src/native_glue.cpp implements by calling the plugin's own request router in-process (the same
// handlers that serve the documented loopback contract in mod/docs/API.md). Host tests implement it
// with a fake that mirrors the sidecar's mock plugin, so the protocol half is tested without a game.
#pragma once
#include <cstdint>
#include <string>

namespace native {

// status: an HTTP-style code from the plugin contract (200, 404, 501, 503, ...). 0 means the call
// never reached the plugin (body then holds the reason).
struct GameResponse {
    int status = 0;
    std::string body;
};

class GameApi {
public:
    virtual ~GameApi() = default;
    // One plugin contract call, e.g. ("GET", "/players/7656.../location", ""). Runs on the caller's
    // thread and may block on the game's own queues for a bounded time (see hooks.h); never call it
    // from the transport thread.
    virtual GameResponse Call(const std::string& method, const std::string& path, const std::string& body) = 0;
    // Reports a connector-side capability in the plugin's /health (e.g. connectorState).
    virtual void SetCapability(const std::string& name, const std::string& status, const std::string& detail) = 0;
    // Directory holding enshrouded_server.exe (and dbghelp.dll); relative config paths resolve here.
    virtual std::string BaseDir() = 0;
};

}  // namespace native
