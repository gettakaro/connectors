// In-process entry points of the plugin contract (mod/docs/API.md) and the native Takaro connector.
#pragma once
#include <string>

struct PluginResponse {
    int status = 200;
    std::string body;
};

// Serves one contract request (method, "/path?query", JSON body) without a socket and without the Bearer
// check: the in-process caller is trusted. May block on the game's queues for a bounded time, so never call
// it from a game thread or from the WebSocket transport thread.
PluginResponse PluginCall(const std::string& method, const std::string& target, const std::string& body);

// src/native_glue.cpp: starts the direct Takaro connection (after HooksInit) and reports it in /health.
void NativeStart();
std::string NativeHealthJson();
