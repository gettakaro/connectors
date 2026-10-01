// Optional loopback diagnostics: HTTP/1.1 on 127.0.0.1:18890 (TAKARO_PLUGIN_PORT), bearer-token
// authenticated. Off unless TAKARO_PLUGIN_TOKEN (or "token" in plugin.json) is set; the Takaro
// connection does not need it. /debug/* additionally needs TAKARO_PLUGIN_DEBUG=1.
#pragma once
#include "common.h"

namespace Http {
void Start();          // spawns the listener thread when a token is configured; never blocks
int Port();
bool TokenConfigured();
std::string StatsJson();
}  // namespace Http
